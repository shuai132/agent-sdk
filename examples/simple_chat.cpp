#include <unistd.h>

#include <asio.hpp>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "core/version.hpp"
#include "spdlog/cfg/env.h"

using namespace agent;

// Global session pointer for signal handler
static std::shared_ptr<Session> g_session;
static std::atomic<bool> g_running{false};        // true when agent is processing
static std::atomic<int64_t> g_last_sigint_ms{0};  // timestamp of last idle Ctrl+C

static int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// write() is async-signal-safe, so we can use it in signal handler
static void signal_write(const char* msg) {
  ::write(STDOUT_FILENO, msg, strlen(msg));
}

static void sigint_handler(int) {
  if (g_running.load() && g_session) {
    g_session->cancel();
    signal_write("\n[Interrupted]\n\n> ");
  } else {
    auto now = now_ms();
    auto last = g_last_sigint_ms.load();
    if (last > 0 && (now - last) < 2000) {
      // Double Ctrl+C within 2s — exit
      signal_write("\n");
      std::signal(SIGINT, SIG_DFL);
      std::raise(SIGINT);
    }
    g_last_sigint_ms.store(now);
    signal_write("\nPress Ctrl+C again or /q to exit.\n\n> ");
  }
}

// Helper: set up session callbacks
static void setup_callbacks(std::shared_ptr<Session>& session) {
  session->on_stream([](const std::string& text) {
    std::cout << text << std::flush;
  });

  session->on_tool_call([](const std::string& tool, const json& args) {
    std::cout << "\n[Calling tool: " << tool << "]\n";
    std::cout << "[Arguments: " << args.dump(2) << "]\n";
  });

  session->on_tool_result([](const std::string& tool, const std::string& result, bool is_error) {
    std::cout << "\n[Tool " << tool << " " << (is_error ? "failed" : "completed") << "]\n";
    // 截断过长的输出
    if (result.size() > 500) {
      std::cout << "[Result: " << result.substr(0, 500) << "... (" << result.size() << " chars total)]\n";
    } else {
      std::cout << "[Result: " << result << "]\n";
    }
  });

  session->on_complete([](FinishReason reason) {
    // 正常结束不需要提示，只在异常情况下显示
    if (reason != FinishReason::Stop && reason != FinishReason::ToolCalls) {
      std::cout << "\n\n[Session ended: " << to_string(reason) << "]";
    }
  });

  session->on_error([](const std::string& error) {
    std::cerr << "\n[Error: " << error << "]\n";
  });

  session->set_permission_handler([](const std::string& permission, const std::string& description) {
    std::promise<bool> promise;
    auto future = promise.get_future();

    std::cout << "\n[Permission requested: " << permission << "]\n";
    std::cout << description << "\n";
    std::cout << "Allow? (y/n): ";

    std::string input;
    std::getline(std::cin, input);

    promise.set_value(input == "y" || input == "Y" || input == "yes");
    return future;
  });
}

// Helper: format timestamp for display
static std::string format_time(const Timestamp& ts) {
  auto time_t = std::chrono::system_clock::to_time_t(ts);
  std::tm tm{};
  localtime_r(&time_t, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

// Helper: print session list
static void print_sessions(const std::vector<SessionMeta>& sessions) {
  std::cout << "\n--- Saved Sessions ---\n";
  for (size_t i = 0; i < sessions.size(); ++i) {
    const auto& s = sessions[i];
    std::cout << "  " << (i + 1) << ". " << (s.title.empty() ? "(untitled)" : s.title) << "\n";
    std::cout << "     " << format_time(s.updated_at) << " | " << to_string(s.agent_type) << " | tokens: " << s.total_usage.total() << "\n";
  }
}

// Helper: print conversation history of current session
static void print_history(const std::shared_ptr<Session>& session) {
  const auto& messages = session->messages();
  if (messages.empty()) return;

  // Find the most recent summary
  int summary_index = -1;
  for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
    if (messages[i].is_summary() && messages[i].is_finished()) {
      summary_index = i;
      break;
    }
  }

  std::cout << "\n--- Conversation History ---\n";

  if (summary_index > 0) {
    std::cout << "[" << summary_index << " earlier messages compacted]\n";
  }

  // Start from summary (or beginning if no summary)
  size_t start = (summary_index >= 0) ? static_cast<size_t>(summary_index) : 0;

  for (size_t i = start; i < messages.size(); ++i) {
    const auto& msg = messages[i];
    if (msg.role() == Role::System) continue;

    if (msg.is_summary()) {
      std::cout << "\n[Summary]\n" << msg.text() << "\n";
      continue;
    }

    if (msg.role() == Role::User) {
      // Skip tool result messages (user messages that only contain tool results)
      auto tool_results = msg.tool_results();
      if (!tool_results.empty() && msg.text().empty()) continue;

      std::cout << "\n> " << msg.text() << "\n";
    } else if (msg.role() == Role::Assistant) {
      auto text = msg.text();
      if (!text.empty()) {
        std::cout << "\nAssistant: " << text << "\n";
      }

      // Show tool calls briefly
      for (const auto* tc : msg.tool_calls()) {
        std::cout << "[Tool: " << tc->name << "]\n";
      }
    }
  }
  std::cout << "----------------------------\n\n";
}

// Helper: print help message
static void print_help() {
  std::cout << "\n--- Available Commands ---\n";
  std::cout << "  /s, /sessions          — List saved sessions\n";
  std::cout << "  /s <N>                 — Load session by number\n";
  std::cout << "  /s save                — Save current session\n";
  std::cout << "  /s d [N]               — Delete session (interactive or by number)\n";
  std::cout << "  /h, /help              — Show this help message\n";
  std::cout << "  /q, /quit              — Exit the program\n";
  std::cout << "--------------------------\n\n";
}

// Helper: delete session by index or interactively
static void handle_delete_command(const std::string& arg, asio::io_context& io_ctx, Config& config, std::shared_ptr<JsonMessageStore>& store,
                                  std::shared_ptr<Session>& session) {
  auto sessions = store->list_sessions();

  if (sessions.empty()) {
    std::cout << "\n[No saved sessions to delete]\n\n";
    return;
  }

  int index = -1;

  if (!arg.empty() && std::all_of(arg.begin(), arg.end(), ::isdigit)) {
    // "/s d <N>" — delete by index directly
    index = std::stoi(arg);
  } else {
    // "/s d" — list and choose
    print_sessions(sessions);
    std::cout << "\nEnter number to delete, or press Enter to cancel: ";

    std::string choice;
    std::getline(std::cin, choice);

    if (choice.empty() || !std::all_of(choice.begin(), choice.end(), ::isdigit)) {
      std::cout << "\n";
      return;
    }
    index = std::stoi(choice);
  }

  if (index < 1 || index > static_cast<int>(sessions.size())) {
    std::cout << "\n[Invalid session number: " << index << "]\n\n";
    return;
  }

  const auto& meta = sessions[index - 1];
  bool is_current = (meta.id == session->id());

  store->remove_session(meta.id);
  std::cout << "\n[Deleted session: " << (meta.title.empty() ? "(untitled)" : meta.title) << "]\n";

  // If we deleted the current session, create a fresh one
  if (is_current) {
    session->cancel();
    session = Session::create(io_ctx, config, AgentType::Build, store);
    g_session = session;
    setup_callbacks(session);
    std::cout << "[Started new session]\n";
  }

  std::cout << "\n";
}

// Helper: list sessions and optionally load one
// Returns true if a session was loaded (session pointer is replaced)
static bool handle_sessions_command(const std::string& arg, asio::io_context& io_ctx, Config& config, std::shared_ptr<JsonMessageStore>& store,
                                    std::shared_ptr<Session>& session) {
  // "/s save" — save current session
  if (arg == "save") {
    std::cout << "\n[Session saved: " << session->id() << "]\n";
    std::cout << "[Title: " << session->title() << "]\n\n";
    return false;
  }

  // "/s d ..." — delete
  if (arg == "d" || arg.substr(0, 2) == "d ") {
    auto delete_arg = (arg.size() > 2) ? arg.substr(2) : "";
    handle_delete_command(delete_arg, io_ctx, config, store, session);
    return false;
  }

  // "/s <number>" — load by index directly
  if (!arg.empty() && std::all_of(arg.begin(), arg.end(), ::isdigit)) {
    int index = std::stoi(arg);
    auto sessions = store->list_sessions();

    if (index < 1 || index > static_cast<int>(sessions.size())) {
      std::cout << "\n[Invalid session number: " << index << "]\n\n";
      return false;
    }

    const auto& meta = sessions[index - 1];
    session->cancel();
    auto resumed = Session::resume(io_ctx, config, meta.id, store);
    if (resumed) {
      session = resumed;
      g_session = session;
      setup_callbacks(session);
      std::cout << "\n[Loaded session: " << meta.title << "]\n";
      std::cout << "[Messages: " << session->messages().size() << "]\n";
      print_history(session);
      return true;
    } else {
      std::cout << "\n[Failed to load session]\n\n";
      return false;
    }
  }

  // Default: list all sessions
  auto sessions = store->list_sessions();

  if (sessions.empty()) {
    std::cout << "\n[No saved sessions]\n\n";
    return false;
  }

  print_sessions(sessions);
  std::cout << "\nEnter number to load, or press Enter to cancel: ";

  std::string choice;
  std::getline(std::cin, choice);

  if (choice.empty() || !std::all_of(choice.begin(), choice.end(), ::isdigit)) {
    std::cout << "\n";
    return false;
  }

  int index = std::stoi(choice);
  if (index < 1 || index > static_cast<int>(sessions.size())) {
    std::cout << "[Invalid number]\n\n";
    return false;
  }

  const auto& meta = sessions[index - 1];
  session->cancel();
  auto resumed = Session::resume(io_ctx, config, meta.id, store);
  if (resumed) {
    session = resumed;
    g_session = session;
    setup_callbacks(session);
    std::cout << "\n[Loaded session: " << meta.title << "]\n";
    std::cout << "[Messages: " << session->messages().size() << "]\n";
    print_history(session);
    return true;
  } else {
    std::cout << "\n[Failed to load session]\n\n";
    return false;
  }
}

int main(int argc, char* argv[]) {
  std::cout << "agent-sdk " << AGENT_SDK_VERSION_STRING << " - Simple Chat Example\n";
  std::cout << "================================\n\n";

  // Log
  spdlog::cfg::load_env_levels();

  // Load configuration
  Config config = Config::load_default();
  std::cout << "Working dir: " << config.working_dir.string() << "\n";

  // Check for API keys in environment — register all available providers
  const char* anthropic_key = std::getenv("ANTHROPIC_API_KEY");
  if (!anthropic_key) {
    anthropic_key = std::getenv("ANTHROPIC_AUTH_TOKEN");
  }
  const char* openai_key = std::getenv("OPENAI_API_KEY");

  if (anthropic_key) {
    const char* base_url = std::getenv("ANTHROPIC_BASE_URL");
    const char* model = std::getenv("ANTHROPIC_MODEL");

    config.providers["anthropic"] = ProviderConfig{"anthropic", anthropic_key, base_url ? base_url : "https://api.anthropic.com", std::nullopt, {}};

    if (model) {
      config.default_model = model;
    }

    std::cout << "Provider: anthropic";
    if (base_url) std::cout << " (" << base_url << ")";
    std::cout << "\n";
  }

  if (openai_key) {
    const char* base_url = std::getenv("OPENAI_BASE_URL");
    const char* model = std::getenv("OPENAI_MODEL");

    config.providers["openai"] = ProviderConfig{"openai", openai_key, base_url ? base_url : "https://api.openai.com", std::nullopt, {}};

    // If OPENAI_MODEL is set, or no anthropic key available, use OpenAI model as default
    if (model) {
      config.default_model = model;
    } else if (!anthropic_key) {
      config.default_model = "gpt-4o";
    }

    std::cout << "Provider: openai";
    if (base_url) std::cout << " (" << base_url << ")";
    std::cout << "\n";
  }

  if (!anthropic_key && !openai_key) {
    std::cerr << "Error: No API key found. Set ANTHROPIC_API_KEY or OPENAI_API_KEY\n";
    return 0;
  }

  std::cout << "Model: " << config.default_model << "\n\n";

  // Initialize ASIO
  asio::io_context io_ctx;

  // Initialize agent framework (providers, builtin tools, skill discovery)
  agent::init();

  // Create persistent store
  auto store = std::make_shared<JsonMessageStore>(config_paths::config_dir() / "sessions");

  // Create session with persistent store
  auto session = Session::create(io_ctx, config, AgentType::Build, store);
  g_session = session;

  // Set up callbacks
  setup_callbacks(session);

  // Install SIGINT handler
  std::signal(SIGINT, sigint_handler);

  // Run IO context in background thread
  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  // Chat loop
  std::string input;
  std::cout << "Enter your message (or '/q' to exit):\n";
  std::cout << "Commands: /h — help, /s — sessions, /q — quit\n\n";

  while (true) {
    std::cout << "> ";
    std::getline(std::cin, input);

    // Handle cin interrupted by signal
    if (std::cin.fail()) {
      std::cin.clear();
      input.clear();
      continue;
    }

    // Handle EOF (e.g. Ctrl+D)
    if (std::cin.eof()) {
      break;
    }

    if (input == "/quit" || input == "/q") {
      break;
    }

    if (input.empty()) {
      continue;
    }

    // Handle /help and /h commands
    if (input == "/help" || input == "/h") {
      print_help();
      continue;
    }

    // Handle /sessions and /s commands
    if (input == "/s" || input == "/sessions" || input.substr(0, 3) == "/s " || input.substr(0, 10) == "/sessions ") {
      std::string arg;
      if (input.substr(0, 10) == "/sessions ") {
        arg = input.substr(10);
      } else if (input.substr(0, 3) == "/s ") {
        arg = input.substr(3);
      }
      handle_sessions_command(arg, io_ctx, config, store, session);
      continue;
    }

    std::cout << "\nAssistant: ";
    g_running.store(true);
    session->prompt(input);
    g_running.store(false);

    if (session->state() == SessionState::Cancelled) {
      // Signal handler already printed [Interrupted] and prompt
      if (std::cin.fail()) {
        std::cin.clear();
      }
    } else {
      std::cout << "\n\n";
    }
  }

  // Cleanup — session is auto-saved via store on every add_message
  session->cancel();
  io_ctx.stop();
  io_thread.join();

  if (!session->messages().empty()) {
    std::cout << "[Session saved: " << session->id() << "]\n";
  }
  std::cout << "Goodbye!\n";
  return 0;
}
