#include "openai.hpp"

#include <spdlog/spdlog.h>

#include "plugin/auth_provider.hpp"

namespace agent::llm {

OpenAIProvider::OpenAIProvider(const ProviderConfig& config, asio::io_context& io_ctx) : config_(config), io_ctx_(io_ctx), http_client_(io_ctx) {
  if (!config.base_url.empty()) {
    base_url_ = config.base_url;
  }
}

std::vector<ModelInfo> OpenAIProvider::models() const {
  return {
      {"gpt-4.1", "openai", 1047576, 32768, true, true},      {"gpt-4.1-mini", "openai", 1047576, 32768, true, true},
      {"gpt-4.1-nano", "openai", 1047576, 32768, true, true}, {"gpt-4o", "openai", 128000, 16384, true, true},
      {"gpt-4o-mini", "openai", 128000, 16384, true, true},   {"o3", "openai", 200000, 100000, true, true},
      {"o3-mini", "openai", 200000, 100000, false, true},     {"o4-mini", "openai", 200000, 100000, true, true},
  };
}

std::future<LlmResponse> OpenAIProvider::complete(const LlmRequest& request) {
  auto promise = std::make_shared<std::promise<LlmResponse>>();
  auto future = promise->get_future();

  auto body = request.to_openai_format();

  // Get authorization header via plugin system
  std::string auth_header = plugin::AuthProviderRegistry::instance().get_auth_header(config_.api_key);

  net::HttpOptions options;
  options.method = "POST";
  options.body = body.dump();
  options.headers = {{"Content-Type", "application/json"}, {"Authorization", auth_header}};

  // Add organization header if configured
  if (config_.organization && !config_.organization->empty()) {
    options.headers["OpenAI-Organization"] = *config_.organization;
  }

  // Add any custom headers
  for (const auto& [key, value] : config_.headers) {
    options.headers[key] = value;
  }

  http_client_.request(base_url_ + "/v1/chat/completions", options, [promise](net::HttpResponse response) {
    LlmResponse result;

    if (!response.error.empty()) {
      result.error = "Network error: " + response.error;
      promise->set_value(result);
      return;
    }

    if (!response.ok()) {
      result.error = "HTTP error: " + std::to_string(response.status_code);
      if (!response.body.empty()) {
        try {
          auto err = json::parse(response.body);
          if (err.contains("error") && err["error"].contains("message")) {
            result.error = err["error"]["message"].get<std::string>();
          }
        } catch (...) {
          result.error = result.error.value_or("") + " - " + response.body;
        }
      }
      promise->set_value(result);
      return;
    }

    try {
      auto j = json::parse(response.body);

      // Parse response
      Message msg(Role::Assistant, "");

      if (j.contains("choices") && !j["choices"].empty()) {
        auto& choice = j["choices"][0];
        auto& message = choice["message"];

        // Parse text content
        if (message.contains("content") && !message["content"].is_null()) {
          msg.add_text(message["content"].get<std::string>());
        }

        // Parse tool calls
        if (message.contains("tool_calls")) {
          for (const auto& tc : message["tool_calls"]) {
            std::string id = tc.value("id", "");
            std::string name = tc["function"].value("name", "");
            json arguments;
            try {
              arguments = json::parse(tc["function"].value("arguments", "{}"));
            } catch (...) {
              arguments = json::object();
            }
            msg.add_tool_call(id, name, arguments);
          }
        }

        // Parse finish reason
        std::string finish_reason = choice.value("finish_reason", "stop");
        if (finish_reason == "tool_calls") {
          result.finish_reason = FinishReason::ToolCalls;
        } else if (finish_reason == "length") {
          result.finish_reason = FinishReason::Length;
        } else {
          result.finish_reason = FinishReason::Stop;
        }
      }

      // Parse usage
      if (j.contains("usage")) {
        result.usage.input_tokens = j["usage"].value("prompt_tokens", 0);
        result.usage.output_tokens = j["usage"].value("completion_tokens", 0);
        // OpenAI may include cached tokens in newer API versions
        if (j["usage"].contains("prompt_tokens_details")) {
          result.usage.cache_read_tokens = j["usage"]["prompt_tokens_details"].value("cached_tokens", 0);
        }
      }

      msg.set_finished(true);
      msg.set_finish_reason(result.finish_reason);
      msg.set_usage(result.usage);
      result.message = std::move(msg);

    } catch (const std::exception& e) {
      result.error = std::string("Parse error: ") + e.what();
    }

    promise->set_value(result);
  });

  return future;
}

void OpenAIProvider::stream(const LlmRequest& request, StreamCallback callback, std::function<void()> on_complete) {
  auto body = request.to_openai_format();
  body["stream"] = true;

  // Get authorization header via plugin system
  std::string auth_header = plugin::AuthProviderRegistry::instance().get_auth_header(config_.api_key);

  std::map<std::string, std::string> headers = {
      {"Content-Type", "application/json"}, {"Accept", "text/event-stream"}, {"Authorization", auth_header}};

  if (config_.organization && !config_.organization->empty()) {
    headers["OpenAI-Organization"] = *config_.organization;
  }

  for (const auto& [key, value] : config_.headers) {
    headers[key] = value;
  }

  // Reset state
  tool_calls_.clear();

  net::HttpOptions options;
  options.method = "POST";
  options.body = body.dump();
  options.headers = headers;

  spdlog::debug("OpenAI request URL: {}/v1/chat/completions", base_url_);
  spdlog::debug("OpenAI request body: {}", options.body);

  auto shared_callback = std::make_shared<StreamCallback>(std::move(callback));
  auto shared_complete = std::make_shared<std::function<void()>>(std::move(on_complete));
  auto sse_buffer = std::make_shared<std::string>();

  // Use streaming HTTP request for real-time SSE processing
  http_client_.request_stream(
      base_url_ + "/v1/chat/completions", options,
      [this, shared_callback, sse_buffer](const std::string& chunk) {
        // Accumulate chunk into SSE buffer and parse complete events
        *sse_buffer += chunk;

        // Process complete SSE events (ended by \n\n or \r\n\r\n)
        size_t pos;
        while ((pos = sse_buffer->find("\n\n")) != std::string::npos || (pos = sse_buffer->find("\r\n\r\n")) != std::string::npos) {
          std::string event_block = sse_buffer->substr(0, pos);
          size_t skip = (sse_buffer->substr(pos, 4) == "\r\n\r\n") ? 4 : 2;
          *sse_buffer = sse_buffer->substr(pos + skip);

          // Parse SSE event from block
          std::istringstream stream(event_block);
          std::string line;
          std::string event_data;

          while (std::getline(stream, line)) {
            // Remove \r if present
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }

            if (line.starts_with("data: ")) {
              if (!event_data.empty()) event_data += "\n";
              event_data += line.substr(6);
            }
          }

          if (!event_data.empty()) {
            parse_sse_event(event_data, *shared_callback);
          }
        }
      },
      [shared_callback, shared_complete](int status_code, const std::string& error) {
        if (!error.empty()) {
          StreamError err;
          err.message = error;
          (*shared_callback)(err);
        }
        (*shared_complete)();
      });
}

void OpenAIProvider::parse_sse_event(const std::string& data, StreamCallback& callback) {
  if (data == "[DONE]") {
    // Emit finish events for any remaining tool calls
    for (auto& [index, tc] : tool_calls_) {
      if (!tc.id.empty()) {
        try {
          json args = tc.args_json.empty() ? json::object() : json::parse(tc.args_json);
          callback(ToolCallComplete{tc.id, tc.name, args});
        } catch (...) {
          callback(ToolCallComplete{tc.id, tc.name, json::object()});
        }
      }
    }
    tool_calls_.clear();
    return;
  }

  try {
    auto j = json::parse(data);

    // Handle error responses
    if (j.contains("error")) {
      StreamError error;
      error.message = j["error"].value("message", "Unknown error");
      callback(error);
      return;
    }

    // Handle usage info (sent as final chunk with stream_options)
    if (j.contains("usage") && !j["usage"].is_null()) {
      FinishStep finish;
      finish.usage.input_tokens = j["usage"].value("prompt_tokens", 0);
      finish.usage.output_tokens = j["usage"].value("completion_tokens", 0);
      if (j["usage"].contains("prompt_tokens_details")) {
        finish.usage.cache_read_tokens = j["usage"]["prompt_tokens_details"].value("cached_tokens", 0);
      }

      // Get finish reason from the choice if available
      if (j.contains("choices") && !j["choices"].empty()) {
        auto& choice = j["choices"][0];
        std::string finish_reason = choice.value("finish_reason", "");
        if (finish_reason == "tool_calls") {
          finish.reason = FinishReason::ToolCalls;
        } else if (finish_reason == "length") {
          finish.reason = FinishReason::Length;
        } else {
          finish.reason = FinishReason::Stop;
        }
      } else {
        finish.reason = FinishReason::Stop;
      }

      callback(finish);
      return;
    }

    if (!j.contains("choices") || j["choices"].empty()) {
      return;
    }

    auto& choice = j["choices"][0];
    auto& delta = choice["delta"];

    // Check finish reason
    std::string finish_reason;
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
      finish_reason = choice["finish_reason"].get<std::string>();
    }

    // Parse text delta
    if (delta.contains("content") && !delta["content"].is_null()) {
      std::string text = delta["content"].get<std::string>();
      if (!text.empty()) {
        callback(TextDelta{text});
      }
    }

    // Parse tool call deltas
    if (delta.contains("tool_calls")) {
      for (const auto& tc : delta["tool_calls"]) {
        int index = tc.value("index", 0);

        // New tool call starts with id and function name
        if (tc.contains("id") && !tc["id"].is_null()) {
          std::string id = tc["id"].get<std::string>();
          std::string name;
          if (tc.contains("function") && tc["function"].contains("name")) {
            name = tc["function"]["name"].get<std::string>();
          }
          tool_calls_[index] = ToolCallInfo{id, name, ""};
          callback(ToolCallDelta{id, name, ""});
        }

        // Accumulate function arguments
        if (tc.contains("function") && tc["function"].contains("arguments")) {
          std::string args_delta = tc["function"]["arguments"].get<std::string>();
          if (!args_delta.empty() && tool_calls_.count(index)) {
            tool_calls_[index].args_json += args_delta;
            callback(ToolCallDelta{tool_calls_[index].id, tool_calls_[index].name, args_delta});
          }
        }
      }
    }

    // Handle finish reason when no usage is present (non-stream_options mode)
    if (!finish_reason.empty()) {
      // If tool calls are pending, emit ToolCallComplete for each
      if (finish_reason == "tool_calls") {
        for (auto& [index, tc] : tool_calls_) {
          if (!tc.id.empty()) {
            try {
              json args = tc.args_json.empty() ? json::object() : json::parse(tc.args_json);
              callback(ToolCallComplete{tc.id, tc.name, args});
            } catch (...) {
              callback(ToolCallComplete{tc.id, tc.name, json::object()});
            }
          }
        }
        tool_calls_.clear();
      }

      // Only emit FinishStep if there's no stream_options usage coming later
      // The usage chunk will emit FinishStep instead
    }

  } catch (const std::exception& e) {
    spdlog::warn("Failed to parse OpenAI SSE event: {}", e.what());
  }
}

void OpenAIProvider::cancel() {
  if (sse_client_) {
    sse_client_->stop();
  }
}

}  // namespace agent::llm
