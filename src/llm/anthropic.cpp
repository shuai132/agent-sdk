#include "agent/llm/anthropic.hpp"

#include <spdlog/spdlog.h>

namespace agent::llm {

AnthropicProvider::AnthropicProvider(const ProviderConfig& config, asio::io_context& io_ctx)
    : config_(config)
    , io_ctx_(io_ctx)
    , http_client_(io_ctx) {
    if (!config.base_url.empty()) {
        base_url_ = config.base_url;
    }
}

std::vector<ModelInfo> AnthropicProvider::models() const {
    return {
        {"claude-opus-4-20250514", "anthropic", 200000, 32000, true, true},
        {"claude-sonnet-4-20250514", "anthropic", 200000, 64000, true, true},
        {"claude-3-5-sonnet-20241022", "anthropic", 200000, 8192, true, true},
        {"claude-3-5-haiku-20241022", "anthropic", 200000, 8192, true, true},
        {"claude-3-opus-20240229", "anthropic", 200000, 4096, true, true},
    };
}

std::future<LlmResponse> AnthropicProvider::complete(const LlmRequest& request) {
    auto promise = std::make_shared<std::promise<LlmResponse>>();
    auto future = promise->get_future();
    
    auto body = request.to_anthropic_format();
    
    net::HttpOptions options;
    options.method = "POST";
    options.body = body.dump();
    options.headers = {
        {"Content-Type", "application/json"},
        {"x-api-key", config_.api_key},
        {"anthropic-version", api_version_}
    };
    
    // Add any custom headers
    for (const auto& [key, value] : config_.headers) {
        options.headers[key] = value;
    }
    
    http_client_.request(
        base_url_ + "/v1/messages",
        options,
        [promise](net::HttpResponse response) {
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
                            result.error = err["error"]["message"];
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
                
                for (const auto& content : j["content"]) {
                    std::string type = content["type"];
                    if (type == "text") {
                        msg.add_text(content["text"]);
                    } else if (type == "tool_use") {
                        msg.add_tool_call(
                            content["id"],
                            content["name"],
                            content["input"]
                        );
                    }
                }
                
                // Parse stop reason
                std::string stop_reason = j.value("stop_reason", "end_turn");
                if (stop_reason == "tool_use") {
                    result.finish_reason = FinishReason::ToolCalls;
                } else if (stop_reason == "max_tokens") {
                    result.finish_reason = FinishReason::Length;
                } else {
                    result.finish_reason = FinishReason::Stop;
                }
                
                // Parse usage
                if (j.contains("usage")) {
                    result.usage.input_tokens = j["usage"].value("input_tokens", 0);
                    result.usage.output_tokens = j["usage"].value("output_tokens", 0);
                    result.usage.cache_read_tokens = j["usage"].value("cache_read_input_tokens", 0);
                    result.usage.cache_write_tokens = j["usage"].value("cache_creation_input_tokens", 0);
                }
                
                msg.set_finished(true);
                msg.set_finish_reason(result.finish_reason);
                msg.set_usage(result.usage);
                result.message = std::move(msg);
                
            } catch (const std::exception& e) {
                result.error = std::string("Parse error: ") + e.what();
            }
            
            promise->set_value(result);
        }
    );
    
    return future;
}

void AnthropicProvider::stream(
    const LlmRequest& request,
    StreamCallback callback,
    std::function<void()> on_complete
) {
    auto body = request.to_anthropic_format();
    body["stream"] = true;
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json"},
        {"Accept", "text/event-stream"},
        {"x-api-key", config_.api_key},
        {"anthropic-version", api_version_}
    };
    
    for (const auto& [key, value] : config_.headers) {
        headers[key] = value;
    }
    
    // Reset state
    tool_call_args_.clear();
    
    net::HttpOptions options;
    options.method = "POST";
    options.body = body.dump();
    options.headers = headers;
    
    auto shared_callback = std::make_shared<StreamCallback>(std::move(callback));
    auto shared_complete = std::make_shared<std::function<void()>>(std::move(on_complete));
    auto sse_buffer = std::make_shared<std::string>();
    
    // Use streaming HTTP request for real-time SSE processing
    http_client_.request_stream(
        base_url_ + "/v1/messages",
        options,
        [this, shared_callback, sse_buffer](const std::string& chunk) {
            // Accumulate chunk into SSE buffer and parse complete events
            *sse_buffer += chunk;
            
            // Process complete SSE events (ended by \n\n or \r\n\r\n)
            size_t pos;
            while ((pos = sse_buffer->find("\n\n")) != std::string::npos ||
                   (pos = sse_buffer->find("\r\n\r\n")) != std::string::npos) {
                
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
        }
    );
}

void AnthropicProvider::parse_sse_event(const std::string& data, StreamCallback& callback) {
    if (data == "[DONE]") {
        return;
    }
    
    try {
        auto j = json::parse(data);
        std::string type = j.value("type", "");
        
        if (type == "content_block_delta") {
            auto delta = j["delta"];
            std::string delta_type = delta.value("type", "");
            
            if (delta_type == "text_delta") {
                callback(TextDelta{delta["text"]});
            } else if (delta_type == "input_json_delta") {
                // Accumulate tool call arguments
                int index = j.value("index", 0);
                std::string idx_str = std::to_string(index);
                tool_call_args_[idx_str] += delta.value("partial_json", "");
            }
        } else if (type == "content_block_start") {
            auto content_block = j["content_block"];
            std::string block_type = content_block.value("type", "");
            
            if (block_type == "tool_use") {
                int index = j.value("index", 0);
                std::string idx_str = std::to_string(index);
                tool_call_args_[idx_str] = "";  // Initialize
                
                callback(ToolCallDelta{
                    content_block["id"],
                    content_block["name"],
                    ""
                });
            }
        } else if (type == "content_block_stop") {
            int index = j.value("index", 0);
            std::string idx_str = std::to_string(index);
            
            // If we have accumulated tool call args, emit the complete event
            auto it = tool_call_args_.find(idx_str);
            if (it != tool_call_args_.end() && !it->second.empty()) {
                try {
                    auto args = json::parse(it->second);
                    // We need the tool call ID and name, which we should have stored
                    // For now, emit a generic complete
                    callback(ToolCallComplete{"", "", args});
                } catch (...) {
                    // Invalid JSON, skip
                }
            }
        } else if (type == "message_delta") {
            auto delta = j["delta"];
            std::string stop_reason = delta.value("stop_reason", "");
            
            FinishStep finish;
            if (stop_reason == "tool_use") {
                finish.reason = FinishReason::ToolCalls;
            } else if (stop_reason == "max_tokens") {
                finish.reason = FinishReason::Length;
            } else {
                finish.reason = FinishReason::Stop;
            }
            
            if (j.contains("usage")) {
                finish.usage.output_tokens = j["usage"].value("output_tokens", 0);
            }
            
            callback(finish);
        } else if (type == "message_start") {
            if (j.contains("message") && j["message"].contains("usage")) {
                // Initial usage info (input tokens)
            }
        } else if (type == "error") {
            StreamError error;
            if (j.contains("error")) {
                error.message = j["error"].value("message", "Unknown error");
            }
            callback(error);
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to parse SSE event: {}", e.what());
    }
}

void AnthropicProvider::cancel() {
    if (sse_client_) {
        sse_client_->stop();
    }
}

}  // namespace agent::llm
