#pragma once

#include "llm/provider.hpp"
#include "net/http_client.hpp"
#include "net/sse_client.hpp"

namespace agent::llm {

// Anthropic Claude provider
class AnthropicProvider : public Provider {
 public:
  AnthropicProvider(const ProviderConfig &config, asio::io_context &io_ctx);

  std::string name() const override {
    return "anthropic";
  }

  std::vector<ModelInfo> models() const override;

  std::future<LlmResponse> complete(const LlmRequest &request) override;

  void stream(const LlmRequest &request, StreamCallback callback, std::function<void()> on_complete) override;

  void cancel() override;

 private:
  void parse_sse_event(const std::string &data, StreamCallback &callback);

  ProviderConfig config_;
  asio::io_context &io_ctx_;
  net::HttpClient http_client_;
  std::unique_ptr<net::SseClient> sse_client_;

  std::string base_url_ = "https://api.anthropic.com";
  std::string api_version_ = "2023-06-01";

  // Track tool calls during streaming (by index)
  struct ToolCallInfo {
    std::string id;
    std::string name;
    std::string args_json;
  };
  std::map<int, ToolCallInfo> tool_calls_;
};

}  // namespace agent::llm
