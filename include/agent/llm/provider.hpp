#pragma once

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "agent/core/types.hpp"
#include "agent/core/message.hpp"
#include "agent/tool/tool.hpp"

namespace agent::llm {

using json = nlohmann::json;

// Stream event types
struct TextDelta {
    std::string text;
};

struct ToolCallDelta {
    std::string id;
    std::string name;
    std::string arguments_delta;  // Partial JSON
};

struct ToolCallComplete {
    std::string id;
    std::string name;
    json arguments;
};

struct FinishStep {
    FinishReason reason;
    TokenUsage usage;
};

struct StreamError {
    std::string message;
    bool retryable = false;
};

using StreamEvent = std::variant<
    TextDelta,
    ToolCallDelta,
    ToolCallComplete,
    FinishStep,
    StreamError
>;

// Stream callback
using StreamCallback = std::function<void(const StreamEvent&)>;

// LLM request
struct LlmRequest {
    std::string model;
    std::vector<Message> messages;
    std::string system_prompt;
    
    // Tool definitions
    std::vector<std::shared_ptr<Tool>> tools;
    
    // Generation parameters
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    std::optional<std::vector<std::string>> stop_sequences;
    
    // Convert to API-specific format
    json to_anthropic_format() const;
    json to_openai_format() const;
};

// LLM response (non-streaming)
struct LlmResponse {
    Message message;
    FinishReason finish_reason;
    TokenUsage usage;
    std::optional<std::string> error;
    
    bool ok() const { return !error.has_value(); }
};

// Abstract LLM provider interface
class Provider {
public:
    virtual ~Provider() = default;
    
    // Provider name
    virtual std::string name() const = 0;
    
    // Available models
    virtual std::vector<ModelInfo> models() const = 0;
    
    // Get model info
    virtual std::optional<ModelInfo> get_model(const std::string& model_id) const;
    
    // Non-streaming completion
    virtual std::future<LlmResponse> complete(const LlmRequest& request) = 0;
    
    // Streaming completion
    virtual void stream(
        const LlmRequest& request,
        StreamCallback callback,
        std::function<void()> on_complete
    ) = 0;
    
    // Cancel current request
    virtual void cancel() = 0;
};

// Provider factory
class ProviderFactory {
public:
    static ProviderFactory& instance();
    
    // Create provider by name
    std::shared_ptr<Provider> create(
        const std::string& name,
        const ProviderConfig& config,
        asio::io_context& io_ctx
    );
    
    // Register custom provider factory
    using FactoryFunc = std::function<std::shared_ptr<Provider>(
        const ProviderConfig&, asio::io_context&)>;
    void register_provider(const std::string& name, FactoryFunc factory);

private:
    std::map<std::string, FactoryFunc> factories_;
};

}  // namespace agent::llm
