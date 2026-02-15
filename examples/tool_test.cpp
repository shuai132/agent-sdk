#include <spdlog/spdlog.h>

#include <asio.hpp>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "agent/agent.hpp"

using namespace agent;

int main() {
  // spdlog::set_level(spdlog::level::debug);  // Uncomment for debugging

  std::cout << "agent-sdk - Tool Call Test\n";
  std::cout << "===========================\n\n";

  // Initialize agent framework
  agent::init();

  // Load configuration from environment variables
  auto config = Config::from_env();

  if (config.providers.empty()) {
    std::cerr << "Error: No API key configured.\n";
    std::cerr << "Please set one of the following:\n";
    std::cerr << "  - ANTHROPIC_API_KEY or ANTHROPIC_AUTH_TOKEN\n";
    std::cerr << "  - OPENAI_API_KEY\n";
    std::cerr << "  - QWEN_OAUTH=true\n";
    return 1;
  }

  // Print provider info
  for (const auto& [name, cfg] : config.providers) {
    std::cout << "Provider: " << name << "\n";
    std::cout << "API URL: " << cfg.base_url << "\n";
  }
  std::cout << "Model: " << config.default_model << "\n\n";

  // Create agent config with model - use "build" as key since AgentType::Build maps to "build"
  AgentConfig agent_cfg;
  agent_cfg.id = "build";  // Must match to_string(AgentType::Build)
  agent_cfg.type = AgentType::Build;
  agent_cfg.model = config.default_model;
  agent_cfg.system_prompt =
      "You are a helpful assistant. When asked to list files, use the glob tool with pattern '*' to list files in the current directory. Be concise.";
  agent_cfg.default_permission = Permission::Allow;
  config.agents["build"] = agent_cfg;

  // Initialize ASIO
  asio::io_context io_ctx;

  // Track completion
  std::mutex mtx;
  std::condition_variable cv;
  bool completed = false;
  std::string full_response;

  // Create session
  auto session = Session::create(io_ctx, config, AgentType::Build);

  if (!session) {
    std::cerr << "Failed to create session\n";
    return 1;
  }

  // Set up callbacks
  session->on_stream([&full_response](const std::string& text) {
    std::cout << text << std::flush;
    full_response += text;
  });

  session->on_tool_call([](const std::string& tool, const json& args) {
    std::cout << "\n[Tool: " << tool << " Args: " << args.dump() << "]\n";
  });

  session->on_complete([&](FinishReason reason) {
    std::cout << "\n[Completed: " << to_string(reason) << "]\n";
    std::lock_guard<std::mutex> lock(mtx);
    completed = true;
    cv.notify_all();
  });

  session->on_error([&](const std::string& error) {
    std::cerr << "\n[Error: " << error << "]\n";
    std::lock_guard<std::mutex> lock(mtx);
    completed = true;
    cv.notify_all();
  });

  // Auto-allow all permissions for testing
  session->set_permission_handler([](const std::string& perm, const std::string& desc) {
    std::cout << "[Permission auto-allowed: " << perm << "]\n";
    std::promise<bool> p;
    p.set_value(true);
    return p.get_future();
  });

  // Run IO context in background
  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  // Send prompt
  std::cout << "User: 列出当前文件夹的内容\n\n";
  std::cout << "Assistant: ";

  // Run prompt in separate thread to not block
  std::thread prompt_thread([&session]() {
    session->prompt("列出当前文件夹的内容");
  });

  // Wait for completion with timeout
  {
    std::unique_lock<std::mutex> lock(mtx);
    if (!cv.wait_for(lock, std::chrono::seconds(120), [&] {
          return completed;
        })) {
      std::cerr << "\nTimeout waiting for response\n";
    }
  }

  // Cleanup
  session->cancel();
  io_ctx.stop();

  if (prompt_thread.joinable()) prompt_thread.join();
  if (io_thread.joinable()) io_thread.join();

  std::cout << "\nTest completed.\n";
  return 0;
}
