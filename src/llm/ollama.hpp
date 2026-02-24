#pragma once

#include "openai.hpp"

namespace agent::llm {

// Ollama provider for local LLM serving
// Inherits from OpenAIProvider for OpenAI-compatible API support
// Only overrides model discovery to use Ollama's /api/tags endpoint
class OllamaProvider : public OpenAIProvider {
 public:
  OllamaProvider(const ProviderConfig& config, asio::io_context& io_ctx);

  std::string name() const override {
    return "ollama";
  }

  std::vector<ModelInfo> models() const override;

 private:
  std::vector<ModelInfo> fetch_ollama_models() const;

  // Cache for models to avoid repeated API calls
  mutable std::vector<ModelInfo> cached_models_;
  mutable bool models_cached_ = false;
};

}  // namespace agent::llm
