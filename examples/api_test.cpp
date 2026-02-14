#include <asio.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "agent/agent.hpp"

using namespace agent;

int main() {
  std::cout << "agent-sdk - API Test\n";
  std::cout << "====================\n\n";

  // Initialize agent framework (registers providers)
  agent::init();

  // Get configuration from environment
  const char* api_key = std::getenv("ANTHROPIC_AUTH_TOKEN");
  if (!api_key) {
    api_key = std::getenv("ANTHROPIC_API_KEY");
  }

  const char* base_url = std::getenv("ANTHROPIC_BASE_URL");
  const char* model = std::getenv("ANTHROPIC_MODEL");

  if (!api_key) {
    std::cerr << "Error: ANTHROPIC_AUTH_TOKEN or ANTHROPIC_API_KEY not set\n";
    return 1;
  }

  std::string url = base_url ? base_url : "https://api.anthropic.com";
  std::string model_name = model ? model : "claude-sonnet-4-20250514";

  std::cout << "API URL: " << url << "\n";
  std::cout << "Model: " << model_name << "\n";
  std::cout << "API Key: " << std::string(api_key).substr(0, 10) << "...\n\n";

  // Initialize ASIO
  asio::io_context io_ctx;

  // Create provider config
  ProviderConfig config;
  config.name = "anthropic";
  config.api_key = api_key;
  config.base_url = url;

  // Create provider
  auto provider = llm::ProviderFactory::instance().create("anthropic", config, io_ctx);

  if (!provider) {
    std::cerr << "Error: Failed to create provider\n";
    return 1;
  }

  std::cout << "Provider: " << provider->name() << "\n";
  std::cout << "Available models:\n";
  for (const auto& m : provider->models()) {
    std::cout << "  - " << m.id << " (context: " << m.context_window << ")\n";
  }
  std::cout << "\n";

  // Create a simple request
  llm::LlmRequest request;
  request.model = model_name;
  request.system_prompt = "You are a helpful assistant. Respond briefly.";
  request.messages = {Message(Role::User, "What is 2+2? Reply in one word.")};
  request.max_tokens = 100;

  std::cout << "Sending test request...\n\n";

  // Run IO context in background
  std::atomic<bool> done{false};
  std::thread io_thread([&io_ctx, &done]() {
    auto work = asio::make_work_guard(io_ctx);
    while (!done) {
      io_ctx.run_for(std::chrono::milliseconds(100));
    }
  });

  // Send request
  auto future = provider->complete(request);

  // Wait for response with timeout
  auto status = future.wait_for(std::chrono::seconds(60));

  if (status == std::future_status::timeout) {
    std::cerr << "Error: Request timed out\n";
    done = true;
    io_thread.join();
    return 1;
  }

  auto response = future.get();

  if (!response.ok()) {
    std::cerr << "Error: " << response.error.value_or("Unknown error") << "\n";
    done = true;
    io_thread.join();
    return 1;
  }

  std::cout << "Response received!\n";
  std::cout << "Finish reason: " << to_string(response.finish_reason) << "\n";
  std::cout << "Usage: input=" << response.usage.input_tokens << ", output=" << response.usage.output_tokens << "\n\n";

  std::cout << "Assistant: ";
  for (const auto& part : response.message.parts()) {
    if (auto* text = std::get_if<TextPart>(&part)) {
      std::cout << text->text;
    }
  }
  std::cout << "\n\n";

  std::cout << "Test completed successfully!\n";

  done = true;
  io_ctx.stop();
  io_thread.join();

  return 0;
}
