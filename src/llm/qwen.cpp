#include "qwen.hpp"

#include <spdlog/spdlog.h>

namespace agent::llm {

QwenProvider::QwenProvider(const ProviderConfig& config, asio::io_context& io_ctx) 
    : config_(config), io_ctx_(io_ctx), http_client_(io_ctx) {
  if (!config.base_url.empty()) {
    base_url_ = config.base_url;
  }
}

std::vector<ModelInfo> QwenProvider::models() const {
  return {
      {"qwen-max", "qwen", 32768, 8192, false, true},
      {"qwen-plus", "qwen", 128000, 8192, true, true},
      {"qwen-turbo", "qwen", 128000, 8192, true, true},
      {"qwen-long", "qwen", 1000000, 8192, false, true},
      {"qwen-vl-max", "qwen", 32768, 8192, true, false},
      {"qwen-vl-plus", "qwen", 128000, 8192, true, false},
      {"qwen-audio-turbo", "qwen", 128000, 8192, false, false},
  };
}

std::future<LlmResponse> QwenProvider::complete(const LlmRequest& request) {
  auto promise = std::make_shared<std::promise<LlmResponse>>();
  auto future = promise->get_future();

  auto body = request.to_openai_format();  // Using OpenAI format as Qwen is OpenAI-compatible

  net::HttpOptions options;
  options.method = "POST";
  options.body = body.dump();
  
  // Qwen API supports both API key and OAuth token authentication
  std::string auth_header = "Bearer " + config_.api_key;
  if (config_.headers.count("Authorization") > 0) {
    auth_header = config_.headers.at("Authorization");  // Allow custom auth header for OAuth
  }
  
  options.headers = {{"Content-Type", "application/json"}, {"Authorization", auth_header}};

  // Add any custom headers
  for (const auto& [key, value] : config_.headers) {
    if (key != "Authorization") {  // Skip Authorization as it's handled above
      options.headers[key] = value;
    }
  }

  http_client_.request(base_url_ + "/api/v1/services/aigc/text-generation/generation", options, [promise](net::HttpResponse response) {
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

      if (j.contains("output") && j["output"].contains("choices") && !j["output"]["choices"].empty()) {
        auto& choice = j["output"]["choices"][0];
        
        if (choice.contains("message")) {
          auto& message = choice["message"];

          // Parse text content
          if (message.contains("content") && !message["content"].is_null()) {
            std::string content = message["content"].get<std::string>();
            msg.add_text(content);
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
        result.usage.input_tokens = j["usage"].value("input_tokens", 0);
        result.usage.output_tokens = j["usage"].value("output_tokens", 0);
        result.usage.cache_read_tokens = j["usage"].value("cache_read_tokens", 0);
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

void QwenProvider::stream(const LlmRequest& request, StreamCallback callback, std::function<void()> on_complete) {
  auto body = request.to_openai_format();  // Using OpenAI format as Qwen is OpenAI-compatible
  body["stream"] = true;

  // Qwen API supports both API key and OAuth token authentication
  std::string auth_header = "Bearer " + config_.api_key;
  if (config_.headers.count("Authorization") > 0) {
    auth_header = config_.headers.at("Authorization");  // Allow custom auth header for OAuth
  }
  
  std::map<std::string, std::string> headers = {
      {"Content-Type", "application/json"}, 
      {"Accept", "text/event-stream"}, 
      {"Authorization", auth_header}
  };

  for (const auto& [key, value] : config_.headers) {
    if (key != "Authorization") {  // Skip Authorization as it's handled above
      headers[key] = value;
    }
  }

  // Reset state
  tool_calls_.clear();

  net::HttpOptions options;
  options.method = "POST";
  options.body = body.dump();
  options.headers = headers;

  spdlog::debug("Qwen request URL: {}/api/v1/services/aigc/text-generation/generation", base_url_);
  spdlog::debug("Qwen request body: {}", options.body);

  auto shared_callback = std::make_shared<StreamCallback>(std::move(callback));
  auto shared_complete = std::make_shared<std::function<void()>>(std::move(on_complete));
  auto sse_buffer = std::make_shared<std::string>();

  // Use streaming HTTP request for real-time SSE processing
  http_client_.request_stream(
      base_url_ + "/api/v1/services/aigc/text-generation/generation", options,
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

void QwenProvider::parse_sse_event(const std::string& data, StreamCallback& callback) {
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
      finish.usage.input_tokens = j["usage"].value("input_tokens", 0);
      finish.usage.output_tokens = j["usage"].value("output_tokens", 0);
      finish.usage.cache_read_tokens = j["usage"].value("cache_read_tokens", 0);

      // Get finish reason from the choice if available
      if (j.contains("output") && j["output"].contains("choices") && !j["output"]["choices"].empty()) {
        auto& choice = j["output"]["choices"][0];
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

    if (!j.contains("output") || !j["output"].contains("choices") || j["output"]["choices"].empty()) {
      return;
    }

    auto& choice = j["output"]["choices"][0];
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
    spdlog::warn("Failed to parse Qwen SSE event: {}", e.what());
  }
}

void QwenProvider::cancel() {
  if (sse_client_) {
    sse_client_->stop();
  }
}

}  // namespace agent::llm