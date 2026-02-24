#include <asio.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "core/config.hpp"
#include "session/session.hpp"

using namespace agent;
using namespace agent::llm;
using namespace std::chrono_literals;

int main() {
  std::cout << "Testing Ollama reasoning/thinking parsing...\n\n";

  // Initialize the agent framework
  agent::init();

  // Load default config and override for Ollama
  Config config = Config::from_env();

  // Create Ollama provider config if not exists
  if (!config.get_provider("ollama")) {
    ProviderConfig ollama_config;
    ollama_config.name = "ollama";
    ollama_config.base_url = "http://localhost:11434";
    ollama_config.api_key = "";  // Ollama doesn't need API key
    config.providers["ollama"] = ollama_config;
  }

  // Set default model
  std::string model = std::getenv("OLLAMA_MODEL") ? std::getenv("OLLAMA_MODEL") : "qwen3:0.6b";
  config.default_model = model;

  std::cout << "Configuration:\n";
  std::cout << "  Default Model: " << config.default_model << "\n";
  if (auto provider_config = config.get_provider("ollama")) {
    std::cout << "  Base URL: " << provider_config->base_url << "\n";
  }
  std::cout << "\n";

  // Create asio context and session
  asio::io_context io_ctx;
  auto session = Session::create(io_ctx, config, AgentType::General);
  if (!session) {
    std::cout << "Failed to create session\n";
    return 1;
  }

  // Set up callbacks to capture thinking/reasoning content
  std::string thinking_content;
  std::string response_content;
  bool thinking_detected = false;

  session->on_stream([&](const std::string& text) {
    response_content += text;
    std::cout << text << std::flush;
  });

  session->on_thinking([&](const std::string& thinking) {
    thinking_content += thinking;
    thinking_detected = true;
    std::cout << "[THINKING] " << thinking << std::flush;
  });

  // Ask a question that should trigger reasoning
  std::cout << "Asking: \"Solve step by step: What is 15 * 23 + 7 - 12?\"\n";
  std::cout << "Response:\n";

  session->prompt("Solve step by step: What is 15 * 23 + 7 - 12?");

  // Run the io_context to process async operations
  auto work_guard = asio::make_work_guard(io_ctx);
  std::thread io_thread([&io_ctx] {
    io_ctx.run();
  });

  // Wait for response
  std::this_thread::sleep_for(10s);

  // Stop the io_context
  work_guard.reset();
  io_ctx.stop();
  if (io_thread.joinable()) {
    io_thread.join();
  }

  std::cout << "\n\nSummary:\n";
  std::cout << "Thinking detected: " << (thinking_detected ? "YES" : "NO") << "\n";
  std::cout << "Thinking content length: " << thinking_content.length() << " chars\n";
  std::cout << "Response content length: " << response_content.length() << " chars\n";

  if (thinking_detected) {
    std::cout << "\n✅ SUCCESS: Reasoning/thinking content was captured!\n";
    std::cout << "First 100 chars of thinking: \"" << thinking_content.substr(0, 100) << "...\"\n";
  } else {
    std::cout << "\n❌ ISSUE: No reasoning/thinking content detected\n";
    std::cout << "This might be because:\n";
    std::cout << "1. The model doesn't use reasoning fields\n";
    std::cout << "2. The question wasn't complex enough\n";
    std::cout << "3. The SSE parsing needs adjustment\n";
  }

  // Shutdown the agent framework
  agent::shutdown();

  return 0;
}
