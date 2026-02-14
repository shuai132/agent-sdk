#include <asio.hpp>
#include <iostream>
#include <string>
#include <thread>

#include "agent/agent.hpp"

using namespace agent;

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
    std::cout << "\n\n[Session completed: " << to_string(reason) << "]\n";
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

  std::cout << "\n--- Conversation History ---\n";
  for (const auto& msg : messages) {
    if (msg.role() == Role::System) continue;

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
  std::cout << "Agent C++ - Simple Chat Example\n";
  std::cout << "================================\n\n";

  // Load configuration
  Config config = Config::load_default();
  std::cout << "Working dir: " << config.working_dir.string() << "\n";

  // Check for API key in environment (support both ANTHROPIC_API_KEY and ANTHROPIC_AUTH_TOKEN)
  const char* api_key = std::getenv("ANTHROPIC_API_KEY");
  if (!api_key) {
    api_key = std::getenv("ANTHROPIC_AUTH_TOKEN");
  }

  const char* base_url = std::getenv("ANTHROPIC_BASE_URL");
  const char* model = std::getenv("ANTHROPIC_MODEL");

  if (api_key) {
    config.providers["anthropic"] = ProviderConfig{"anthropic", api_key, base_url ? base_url : "https://api.anthropic.com", std::nullopt, {}};

    if (model) {
      config.default_model = model;
    }

    std::cout << "Using API: " << (base_url ? "$ANTHROPIC_BASE_URL" : "https://api.anthropic.com") << "\n";
    std::cout << "Model: " << config.default_model << "\n\n";
  } else {
    std::cerr << "Error: ANTHROPIC_API_KEY or ANTHROPIC_AUTH_TOKEN not set\n";
    return 0;
  }

  // Initialize ASIO
  asio::io_context io_ctx;

  // Register builtin tools
  tools::register_builtins();

  // Create persistent store
  auto store = std::make_shared<JsonMessageStore>(config_paths::config_dir() / "sessions");

  // Create session with persistent store
  auto session = Session::create(io_ctx, config, AgentType::Build, store);

  // Set up callbacks
  setup_callbacks(session);

  // Run IO context in background thread
  std::thread io_thread([&io_ctx]() {
    asio::io_context::work work(io_ctx);
    io_ctx.run();
  });

  // Chat loop
  std::string input;
  std::cout << "Enter your message (or 'quit' to exit):\n";
  std::cout << "Commands: /s — list sessions, /s <N> — load, /s d [N] — delete, /s save — save\n\n";

  while (true) {
    std::cout << "> ";
    std::getline(std::cin, input);

    if (input == "quit" || input == "exit") {
      break;
    }

    if (input.empty()) {
      continue;
    }

    // Handle /sessions and /s commands
    if (input == "/sessions" || input == "/s") {
      handle_sessions_command("", io_ctx, config, store, session);
      continue;
    }

    if (input.substr(0, 3) == "/s ") {
      auto arg = input.substr(3);
      handle_sessions_command(arg, io_ctx, config, store, session);
      continue;
    }

    std::cout << "\nAssistant: ";
    session->prompt(input);
    std::cout << "\n\n";
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
