#include "ollama.hpp"

#include <spdlog/spdlog.h>

#include <future>

namespace agent::llm {

OllamaProvider::OllamaProvider(const ProviderConfig& config, asio::io_context& io_ctx) : OpenAIProvider(config, io_ctx) {
  // Set Ollama default base URL if not specified
  if (config.base_url.empty()) {
    set_base_url("http://localhost:11434");
  }
}

std::vector<ModelInfo> OllamaProvider::models() const {
  if (models_cached_) {
    return cached_models_;
  }

  try {
    cached_models_ = fetch_ollama_models();
    models_cached_ = true;
    return cached_models_;
  } catch (const std::exception& e) {
    spdlog::warn("[Ollama] Failed to fetch models: {}, returning empty list", e.what());
    return {};
  }
}

std::vector<ModelInfo> OllamaProvider::fetch_ollama_models() const {
  auto promise = std::make_shared<std::promise<std::vector<ModelInfo>>>();
  auto future = promise->get_future();

  net::HttpOptions options;
  options.method = "GET";
  options.timeout = std::chrono::seconds(10);

  get_http_client().request(get_base_url() + "/api/tags", options, [promise](const net::HttpResponse& response) {
    if (response.status_code != 200 || !response.error.empty()) {
      promise->set_value({});
      return;
    }

    try {
      auto json_response = nlohmann::json::parse(response.body);
      std::vector<ModelInfo> models;

      if (json_response.contains("models") && json_response["models"].is_array()) {
        for (const auto& model : json_response["models"]) {
          if (model.contains("name")) {
            std::string model_name = model["name"];
            // Basic model info - Ollama doesn't provide detailed specs via API
            models.push_back({model_name, "ollama", 32768, 4096, true, true});
          }
        }
      }

      promise->set_value(models);
    } catch (const std::exception& e) {
      spdlog::error("[Ollama] Failed to parse models response: {}", e.what());
      promise->set_value({});
    }
  });

  return future.get();
}

}  // namespace agent::llm
