#include <asio.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "agent/agent.hpp"

using namespace agent;

int main() {
  std::cout << "agent-sdk - Ollama Test\n";
  std::cout << "=======================\n\n";

  // Initialize agent framework (registers providers)
  agent::init();

  // Check for Ollama-specific environment variables
  const char* ollama_key = std::getenv("OLLAMA_API_KEY");
  const char* ollama_url = std::getenv("OLLAMA_BASE_URL");
  const char* ollama_model = std::getenv("OLLAMA_MODEL");

  std::cout << "Environment Variables:\n";
  std::cout << "  OLLAMA_API_KEY: " << (ollama_key ? (std::string(ollama_key).empty() ? "\"\" (empty, correct for Ollama)" : ollama_key) : "NOT SET")
            << "\n";
  std::cout << "  OLLAMA_BASE_URL: " << (ollama_url ? ollama_url : "NOT SET (will use default: http://localhost:11434)") << "\n";
  std::cout << "  OLLAMA_MODEL: " << (ollama_model ? ollama_model : "NOT SET (will use default: deepseek-r1:7b)") << "\n\n";

  // Load configuration from environment variables
  auto config = Config::from_env();

  if (config.providers.empty()) {
    std::cerr << "Error: No LLM provider configured.\n\n";
    std::cerr << "For Ollama, please set:\n";
    std::cerr << "  export OLLAMA_API_KEY=\"\"      # Empty string is correct\n";
    std::cerr << "  export OLLAMA_MODEL=\"qwen3\"   # Your model name\n";
    std::cerr << "  export OLLAMA_BASE_URL=\"http://localhost:11434\"  # Optional\n\n";
    std::cerr << "Make sure Ollama is running: ollama serve\n";
    return 1;
  }

  // Find Ollama provider specifically
  auto ollama_it = config.providers.find("ollama");
  if (ollama_it == config.providers.end()) {
    std::cerr << "Error: Ollama provider not found in configuration.\n";
    std::cerr << "Available providers: ";
    for (const auto& [name, cfg] : config.providers) {
      std::cerr << name << " ";
    }
    std::cerr << "\n";
    return 1;
  }

  const auto& provider_config = ollama_it->second;
  std::cout << "Ollama Configuration:\n";
  std::cout << "  Provider: ollama\n";
  std::cout << "  Base URL: " << provider_config.base_url << "\n";
  std::cout << "  Model: " << config.default_model << "\n";
  std::cout << "  API Key: " << (provider_config.api_key.empty() ? "\"\" (correct for Ollama)" : provider_config.api_key) << "\n\n";

  // Initialize ASIO
  asio::io_context io_ctx;

  // Create Ollama provider
  auto provider = llm::ProviderFactory::instance().create("ollama", provider_config, io_ctx);

  if (!provider) {
    std::cerr << "Error: Failed to create Ollama provider\n";
    return 1;
  }

  std::cout << "Testing connection to Ollama...\n";
  std::cout << "Provider name: " << provider->name() << "\n";

  // Test model discovery (skip for now to avoid blocking)
  std::cout << "\nSkipping model discovery to avoid blocking...\n";
  std::cout << "Assuming model '" << config.default_model << "' is available.\n";

  std::cout << "\n";

  // Create a simple test request
  llm::LlmRequest request;
  request.model = config.default_model;
  request.system_prompt = "You are a helpful assistant. Respond in Chinese briefly.";
  request.messages = {Message(Role::User, "你好，请说出2+2等于多少？")};
  request.max_tokens = 100;
  request.temperature = 0.1;

  std::cout << "Sending test request to model: " << request.model << "\n";
  std::cout << "Message: " << request.messages[0].text() << "\n\n";

  // Run IO context in background
  std::atomic<bool> done{false};
  std::thread io_thread([&io_ctx, &done]() {
    auto work = asio::make_work_guard(io_ctx);
    while (!done) {
      io_ctx.run_for(std::chrono::milliseconds(100));
    }
  });

  // Send request
  auto start_time = std::chrono::steady_clock::now();
  auto future = provider->complete(request);

  // Wait for response with shorter timeout for testing
  std::cout << "Waiting for response (timeout: 30 seconds)...\n";
  auto status = future.wait_for(std::chrono::seconds(30));

  if (status == std::future_status::timeout) {
    std::cerr << "Error: Request timed out after 30 seconds\n";
    std::cerr << "This might indicate:\n";
    std::cerr << "  1. Model is not loaded (try: ollama run " << config.default_model << ")\n";
    std::cerr << "  2. Ollama service is not running (try: ollama serve)\n";
    std::cerr << "  3. Model name is incorrect (check: ollama list)\n";
    done = true;
    io_thread.join();
    return 1;
  }

  auto end_time = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  try {
    auto response = future.get();

    if (!response.ok()) {
      std::cerr << "Error from Ollama: " << response.error.value_or("Unknown error") << "\n";
      std::cerr << "\nTroubleshooting:\n";
      std::cerr << "  1. Check if Ollama is running: curl http://localhost:11434/api/tags\n";
      std::cerr << "  2. Verify model exists: ollama list\n";
      std::cerr << "  3. Try pulling the model: ollama pull " << config.default_model << "\n";
      done = true;
      io_thread.join();
      return 1;
    }

    std::cout << "✓ Response received successfully!\n";
    std::cout << "Response time: " << duration.count() << "ms\n";
    std::cout << "Finish reason: " << to_string(response.finish_reason) << "\n";
    std::cout << "Token usage: input=" << response.usage.input_tokens << ", output=" << response.usage.output_tokens << "\n\n";

    std::cout << "Assistant response:\n";
    std::cout << "==================\n";
    for (const auto& part : response.message.parts()) {
      if (auto* text = std::get_if<TextPart>(&part)) {
        std::cout << text->text;
      }
    }
    std::cout << "\n==================\n\n";

    std::cout << "✓ Ollama test completed successfully!\n";
    std::cout << "Your Ollama setup is working correctly.\n";

  } catch (const std::exception& e) {
    std::cerr << "Exception during request: " << e.what() << "\n";
    done = true;
    io_thread.join();
    return 1;
  }

  done = true;
  io_ctx.stop();
  io_thread.join();

  return 0;
}