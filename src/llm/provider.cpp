#include "provider.hpp"

#include "llm/anthropic.hpp"
#include "llm/openai.hpp"
#include "llm/qwen.hpp"

namespace agent::llm {

std::optional<ModelInfo> Provider::get_model(const std::string& model_id) const {
  auto all_models = models();
  for (const auto& model : all_models) {
    if (model.id == model_id) {
      return model;
    }
  }
  return std::nullopt;
}

ProviderFactory& ProviderFactory::instance() {
  static ProviderFactory instance;
  return instance;
}

std::shared_ptr<Provider> ProviderFactory::create(const std::string& name, const ProviderConfig& config, asio::io_context& io_ctx) {
  // Ensure default providers are registered
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    // Register Anthropic provider
    instance().register_provider("anthropic", [](const ProviderConfig& cfg, asio::io_context& ctx) {
      return std::make_shared<AnthropicProvider>(cfg, ctx);
    });
    // Register OpenAI provider
    instance().register_provider("openai", [](const ProviderConfig& cfg, asio::io_context& ctx) {
      return std::make_shared<OpenAIProvider>(cfg, ctx);
    });
    // Register Qwen provider
    instance().register_provider("qwen", [](const ProviderConfig& cfg, asio::io_context& ctx) {
      return std::make_shared<QwenProvider>(cfg, ctx);
    });
  }

  auto it = factories_.find(name);
  if (it != factories_.end()) {
    return it->second(config, io_ctx);
  }
  return nullptr;
}

void ProviderFactory::register_provider(const std::string& name, FactoryFunc factory) {
  factories_[name] = std::move(factory);
}

// Helper to convert messages to Anthropic format
json LlmRequest::to_anthropic_format() const {
  json request;
  request["model"] = model;
  request["max_tokens"] = max_tokens.value_or(8192);

  if (!system_prompt.empty()) {
    request["system"] = system_prompt;
  }

  if (temperature) {
    request["temperature"] = *temperature;
  }

  if (stop_sequences && !stop_sequences->empty()) {
    request["stop_sequences"] = *stop_sequences;
  }

  // Convert messages
  json msgs = json::array();
  for (const auto& msg : messages) {
    if (msg.role() == Role::System) continue;  // System handled separately

    json m;
    m["role"] = msg.role() == Role::User ? "user" : "assistant";

    json content = json::array();
    for (const auto& part : msg.parts()) {
      if (auto* text = std::get_if<TextPart>(&part)) {
        content.push_back({{"type", "text"}, {"text", text->text}});
      } else if (auto* tc = std::get_if<ToolCallPart>(&part)) {
        content.push_back({{"type", "tool_use"}, {"id", tc->id}, {"name", tc->name}, {"input", tc->arguments}});
      } else if (auto* tr = std::get_if<ToolResultPart>(&part)) {
        content.push_back({{"type", "tool_result"}, {"tool_use_id", tr->tool_call_id}, {"content", tr->output}, {"is_error", tr->is_error}});
      } else if (auto* img = std::get_if<ImagePart>(&part)) {
        // Handle base64 images
        if (img->url.starts_with("data:")) {
          auto comma = img->url.find(',');
          if (comma != std::string::npos) {
            auto media_type_end = img->url.find(';');
            std::string media_type = img->url.substr(5, media_type_end - 5);
            std::string data = img->url.substr(comma + 1);
            content.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", media_type}, {"data", data}}}});
          }
        }
      }
    }

    if (content.size() == 1 && content[0]["type"] == "text") {
      m["content"] = content[0]["text"];
    } else {
      m["content"] = content;
    }

    msgs.push_back(m);
  }
  request["messages"] = msgs;

  // Convert tools
  if (!tools.empty()) {
    json tools_json = json::array();
    for (const auto& tool : tools) {
      tools_json.push_back(tool->to_json_schema());
    }
    request["tools"] = tools_json;
  }

  return request;
}

// Helper to convert messages to OpenAI format
json LlmRequest::to_openai_format() const {
  json request;
  request["model"] = model;

  if (max_tokens) {
    request["max_tokens"] = *max_tokens;
  }

  if (temperature) {
    request["temperature"] = *temperature;
  }

  if (stop_sequences && !stop_sequences->empty()) {
    request["stop"] = *stop_sequences;
  }

  // Convert messages
  json msgs = json::array();

  // Add system message if present
  if (!system_prompt.empty()) {
    msgs.push_back({{"role", "system"}, {"content", system_prompt}});
  }

  for (const auto& msg : messages) {
    if (msg.role() == Role::System) continue;

    // OpenAI requires tool results as separate role="tool" messages
    auto tool_results = msg.tool_results();
    if (!tool_results.empty()) {
      // Also include any text content from the message as a user message
      auto text = msg.text();
      if (!text.empty()) {
        msgs.push_back({{"role", "user"}, {"content", text}});
      }

      for (const auto* tr : tool_results) {
        json tool_msg;
        tool_msg["role"] = "tool";
        tool_msg["tool_call_id"] = tr->tool_call_id;
        tool_msg["content"] = tr->output;
        msgs.push_back(tool_msg);
      }
    } else {
      msgs.push_back(msg.to_api_format());
    }
  }
  request["messages"] = msgs;

  // Convert tools
  if (!tools.empty()) {
    json tools_json = json::array();
    for (const auto& tool : tools) {
      auto schema = tool->to_json_schema();
      // OpenAI uses "parameters" instead of "input_schema"
      json func = {{"name", schema["name"]}, {"description", schema["description"]}};
      if (schema.contains("input_schema")) {
        func["parameters"] = schema["input_schema"];
      }
      tools_json.push_back({{"type", "function"}, {"function", func}});
    }
    request["tools"] = tools_json;
  }

  return request;
}

}  // namespace agent::llm
