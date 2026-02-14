#include <gtest/gtest.h>
#include "agent/llm/provider.hpp"
#include "agent/llm/anthropic.hpp"

using namespace agent;
using namespace agent::llm;

TEST(LlmTest, ProviderFactory) {
    auto& factory = ProviderFactory::instance();
    
    // Without config, should return nullptr
    ProviderConfig empty_config;
    asio::io_context io_ctx;
    
    auto provider = factory.create("anthropic", empty_config, io_ctx);
    EXPECT_NE(provider, nullptr);
}

TEST(LlmTest, AnthropicModels) {
    asio::io_context io_ctx;
    ProviderConfig config;
    config.api_key = "test-key";
    
    AnthropicProvider provider(config, io_ctx);
    
    auto models = provider.models();
    EXPECT_GT(models.size(), 0);
    
    // Check for Claude Sonnet
    bool has_sonnet = false;
    for (const auto& model : models) {
        if (model.id.find("sonnet") != std::string::npos) {
            has_sonnet = true;
            break;
        }
    }
    EXPECT_TRUE(has_sonnet);
}

TEST(LlmTest, RequestFormat) {
    LlmRequest request;
    request.model = "claude-sonnet-4-20250514";
    request.system_prompt = "You are a helpful assistant.";
    request.messages.push_back(Message::user("Hello"));
    
    auto anthropic_json = request.to_anthropic_format();
    
    EXPECT_EQ(anthropic_json["model"], "claude-sonnet-4-20250514");
    EXPECT_EQ(anthropic_json["system"], "You are a helpful assistant.");
    EXPECT_TRUE(anthropic_json.contains("messages"));
}
