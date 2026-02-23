#pragma once

#include "net/http_client.hpp"
#include "net/sse_client.hpp"
#include "provider.hpp"

namespace agent::llm {

// OpenAI GPT provider (also compatible with OpenAI API-compatible services)
class OpenAIProvider : public Provider {
 public:
  OpenAIProvider(const ProviderConfig& config, asio::io_context& io_ctx);

  std::string name() const override {
    return "openai";
  }

  std::vector<ModelInfo> models() const override;

  std::future<LlmResponse> complete(const LlmRequest& request) override;

  void stream(const LlmRequest& request, StreamCallback callback, std::function<void()> on_complete) override;

  void cancel() override;

 private:
  void parse_sse_event(const std::string& data, StreamCallback& callback);

  ProviderConfig config_;
  asio::io_context& io_ctx_;
  net::HttpClient http_client_;
  std::unique_ptr<net::SseClient> sse_client_;

  std::string base_url_ = "https://api.openai.com";

  // Track tool calls during streaming (by index)
  struct ToolCallInfo {
    std::string id;
    std::string name;
    std::string args_json;
  };
  std::map<int, ToolCallInfo> tool_calls_;

  // Track finish reason for streaming (some APIs send finish_reason before [DONE])
  FinishReason finish_reason_ = FinishReason::Stop;

  // Track whether we're inside a <think>...</think> block in content
  bool in_thinking_block_ = false;
};

}  // namespace agent::llm
