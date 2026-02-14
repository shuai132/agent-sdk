#pragma once

#include "agent/llm/provider.hpp"
#include "agent/net/http_client.hpp"
#include "agent/net/sse_client.hpp"

namespace agent::llm {

// Anthropic Claude provider
class AnthropicProvider : public Provider {
public:
    AnthropicProvider(const ProviderConfig& config, asio::io_context& io_ctx);
    
    std::string name() const override { return "anthropic"; }
    std::vector<ModelInfo> models() const override;
    
    std::future<LlmResponse> complete(const LlmRequest& request) override;
    
    void stream(
        const LlmRequest& request,
        StreamCallback callback,
        std::function<void()> on_complete
    ) override;
    
    void cancel() override;

private:
    void parse_sse_event(const std::string& data, StreamCallback& callback);
    
    ProviderConfig config_;
    asio::io_context& io_ctx_;
    net::HttpClient http_client_;
    std::unique_ptr<net::SseClient> sse_client_;
    
    std::string base_url_ = "https://api.anthropic.com";
    std::string api_version_ = "2023-06-01";
    
    // Accumulate tool call arguments during streaming
    std::map<std::string, std::string> tool_call_args_;
};

}  // namespace agent::llm
