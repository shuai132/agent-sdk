#pragma once

#include "net/http_client.hpp"
#include "net/sse_client.hpp"
#include "provider.hpp"

namespace agent::llm {

// Qwen provider for Tongyi/Qwen API
class QwenProvider : public Provider {
 public:
  QwenProvider(const ProviderConfig& config, asio::io_context& io_ctx);

  std::string name() const override {
    return "qwen";
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

  std::string base_url_ = "https://dashscope.aliyuncs.com";  // Default Qwen API endpoint

  // Track tool calls during streaming (by index)
  struct ToolCallInfo {
    std::string id;
    std::string name;
    std::string args_json;
  };
  std::map<int, ToolCallInfo> tool_calls_;
};

}  // namespace agent::llm