#include <termios.h>
#include <unistd.h>

#include <asio.hpp>
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "core/version.hpp"
#include "tui_callbacks.h"
#include "tui_components.h"
#include "tui_event_handler.h"
#include "tui_render.h"
#include "tui_state.h"

#ifdef AGENT_PLUGIN_QWEN
#include "plugin/qrcode.hpp"
#include "plugin/qwen/qwen_oauth.hpp"
#endif

using namespace agent;
using namespace agent_cli;
using namespace ftxui;

void print_usage(const char* program_name) {
  std::cout << "agent_cli " << AGENT_SDK_VERSION_STRING << " — AI Agent TUI\n\n";
  std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
  std::cout << "Options:\n";
  std::cout << "  -h, --help       Show this help message and exit\n";
  std::cout << "  -v, --version    Show version information and exit\n\n";
  std::cout << "Environment Variables (choose one):\n";
  std::cout << "  QWEN_OAUTH               Set to '1' to enable Qwen Portal OAuth\n";
  std::cout << "  QWEN_BASE_URL            Custom Qwen Portal base URL\n";
  std::cout << "  QWEN_MODEL               Custom Qwen model name\n\n";
  std::cout << "  ANTHROPIC_API_KEY        Anthropic API key\n";
  std::cout << "  ANTHROPIC_AUTH_TOKEN     Anthropic auth token (alternative)\n";
  std::cout << "  ANTHROPIC_BASE_URL       Custom Anthropic API base URL\n";
  std::cout << "  ANTHROPIC_MODEL          Custom Anthropic model name\n\n";
  std::cout << "  OPENAI_API_KEY           OpenAI API key\n";
  std::cout << "  OPENAI_BASE_URL          Custom OpenAI API base URL\n";
  std::cout << "  OPENAI_MODEL             Custom OpenAI model name\n\n";
  std::cout << "  OLLAMA_API_KEY           Set to '' (no API key required)\n";
  std::cout << "  OLLAMA_BASE_URL          Custom Ollama base URL (default: http://localhost:11434)\n";
  std::cout << "  OLLAMA_MODEL             Custom Ollama model name\n\n";
  std::cout << "Priority: QWEN_OAUTH > OPENAI_API_KEY > OLLAMA_API_KEY\n\n";
  std::cout << "Examples:\n";
  std::cout << "  # Use Qwen Portal with OAuth (no API key needed)\n";
  std::cout << "  export QWEN_OAUTH=1\n";
  std::cout << "  " << program_name << "\n\n";
  std::cout << "  # Use Anthropic\n";
  std::cout << "  export ANTHROPIC_API_KEY=\"your-api-key\"\n";
  std::cout << "  " << program_name << "\n\n";
  std::cout << "  # Use OpenAI-compatible API\n";
  std::cout << "  export OPENAI_API_KEY=\"your-api-key\"\n";
  std::cout << "  export OPENAI_BASE_URL=\"https://api.example.com/v1\"\n";
  std::cout << "  " << program_name << "\n\n";
  std::cout << "  # Use Ollama (local models)\n";
  std::cout << "  export OLLAMA_API_KEY=\"\"\n";
  std::cout << "  export OLLAMA_MODEL=\"deepseek-r1:7b\"\n";
  std::cout << "  " << program_name << "\n\n";
  std::cout << "TUI Commands:\n";
  std::cout << "  /help, /h        Show help in TUI\n";
  std::cout << "  /quit, /q        Exit the program\n";
  std::cout << "  /sessions, /s    Manage sessions\n";
  std::cout << "  /clear           Clear chat history\n";
  std::cout << "  /copy, /c        Copy chat to clipboard\n\n";
  std::cout << "For more information, visit: https://github.com/shuai132/agent-sdk\n";
}

void print_version() {
  std::cout << "agent_cli " << AGENT_SDK_VERSION_STRING << "\n";
  std::cout << "Build: C++" << __cplusplus << "\n";
#ifdef __clang__
  std::cout << "Compiler: Clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__ << "\n";
#elif defined(__GNUC__)
  std::cout << "Compiler: GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__ << "\n";
#endif
}

int main(int argc, char* argv[]) {
  // ===== 解析命令行参数 =====
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      print_version();
      return 0;
    }
    // Unknown argument
    std::cerr << "Unknown option: " << arg << "\n";
    std::cerr << "Use --help for usage information.\n";
    return 1;
  }

  // ===== 加载配置（从环境变量和配置文件）=====
  Config config = Config::from_env();

  if (config.providers.empty()) {
    std::cerr << "Error: No API key configured.\n\n";
    std::cerr << "Please set one of the following environment variables:\n";
    std::cerr << "  • QWEN_OAUTH=1         — for Qwen Portal (OAuth, no API key needed)\n";
    std::cerr << "  • ANTHROPIC_API_KEY    — for Claude models\n";
    std::cerr << "  • OPENAI_API_KEY       — for OpenAI/compatible models\n";
    std::cerr << "  • OLLAMA_API_KEY=\"\"    — for Ollama local models (no API key needed)\n\n";
    std::cerr << "Run '" << argv[0] << " --help' for more information.\n";
    return 1;
  }

  // 检测是否为 Qwen OAuth 模式
  bool is_qwen_oauth = std::getenv("QWEN_OAUTH") != nullptr;

  // ===== 初始化框架 =====
  asio::io_context io_ctx;
  agent::init();

#ifdef AGENT_PLUGIN_QWEN
  // 注册 Qwen OAuth 插件（如果编译时启用）
  plugin::qwen::register_qwen_plugin();
#endif
  auto store = std::make_shared<JsonMessageStore>(config_paths::config_dir() / "sessions");
  auto session = Session::create(io_ctx, config, AgentType::Build, store);

  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  // ===== FTXUI 屏幕 =====
  auto screen = ScreenInteractive::Fullscreen();
  screen.TrackMouse(true);

  // ===== 状态与上下文 =====
  AppState state;

#ifdef AGENT_PLUGIN_QWEN
  // ===== Qwen OAuth 登录检测 =====
  bool needs_qwen_login = false;
  if (is_qwen_oauth) {
    auto& auth = plugin::qwen::qwen_portal_auth();
    auto token = auth.load_token();
    if (!token || token->is_expired()) {
      needs_qwen_login = true;
      state.login_state = LoginState::NeedLogin;
    }
  }

  // 登录认证的 future（用于异步等待）
  std::future<std::optional<plugin::qwen::OAuthToken>> login_future;
  bool login_started = false;
#endif
  state.agent_state.set_model(config.default_model);
  state.agent_state.set_session_id(session->id());
  state.agent_state.update_context(session->estimated_context_tokens(), session->context_window());

  // 加载历史记录
  auto history_file = config_paths::config_dir() / "input_history.json";
  state.load_history_from_file(history_file);

  AppContext ctx{io_ctx, config, store, session, [&screen]() {
                   screen.Post(Event::Custom);
                 }};

  setup_tui_callbacks(state, ctx);

  // ===== 输入组件 =====
  auto input_option = InputOption();
  input_option.multiline = false;
  input_option.cursor_position = &state.input_cursor_pos;
  input_option.transform = [](InputState s) {
    if (s.is_placeholder) {
      s.element |= dim | color(Color::GrayDark);
    }
    return s.element;
  };
  input_option.on_change = [&state] {
    if (!state.input_text.empty() && state.input_text[0] == '/') {
      auto matches = match_commands(state.input_text);
      state.show_cmd_menu = !matches.empty();
      state.cmd_menu_selected = 0;
      state.show_file_path_menu = false;  // 确保文件路径菜单关闭
    } else {
      state.show_cmd_menu = false;

      // 检查是否输入了 @ 符号以启用文件路径自动完成
      size_t at_pos = state.input_text.rfind('@');
      if (at_pos != std::string::npos) {
        std::string path_prefix = state.input_text.substr(at_pos + 1);
        state.file_path_matches = match_file_paths(path_prefix);
        state.show_file_path_menu = !state.file_path_matches.empty();
        state.file_path_menu_selected = 0;
      } else {
        state.show_file_path_menu = false;
        state.file_path_matches.clear();
      }
    }
  };
  input_option.on_enter = [&] {
    handle_submit(state, ctx, screen);
  };
  auto input_component = Input(&state.input_text, "输入您的消息或 @ 文件路径", input_option);

  auto input_with_prompt = Renderer(input_component, [&] {
    return hbox({
        text(" > ") | bold | color(Color::Cyan),
        input_component->Render() | flex,
    });
  });

  // ===== 主渲染器 =====
  auto final_renderer = Renderer(input_with_prompt, [&] {
    // 登录面板（最高优先级）
    if (state.login_state != LoginState::NotRequired && state.login_state != LoginState::Success) {
      auto login_panel = build_login_panel(state);
      return vbox({
          text(" agent_cli ") | bold | color(Color::White) | bgcolor(Color::Blue),
          separator() | dim,
          login_panel | flex,
      });
    }

    auto status_bar = build_status_bar(state);
    auto chat_view = build_chat_view(state);
    auto cmd_menu_element = build_cmd_menu(state);
    auto file_path_menu_element = build_file_path_menu(state);

    auto mode_str = to_string(state.agent_state.mode());
    auto input_area = vbox({
        cmd_menu_element,
        file_path_menu_element,
        separator() | dim,
        input_with_prompt->Render(),
        separator() | dim,
        hbox({text(" " + mode_str + " ") | dim, text("  tab to switch mode") | dim, filler()}),
    });

    // Question 面板（优先级最高）
    if (state.show_question_panel) {
      auto question_panel = build_question_panel(state);
      return vbox({
          status_bar,
          separator() | dim,
          question_panel | flex,
      });
    }

    if (state.show_sessions_panel) {
      auto sessions_panel = build_sessions_panel(state);
      return vbox({
          status_bar,
          separator() | dim,
          sessions_panel | flex,
          input_area,
      });
    }

    return vbox({
        status_bar,
        separator() | dim,
        chat_view | flex,
        input_area,
    });
  });

  // ===== 事件处理 =====
  auto component = CatchEvent(final_renderer, [&](Event event) {
    return handle_main_event(state, ctx, screen, event);
  });

  // ===== 欢迎消息 =====
  state.chat_log.push(
      {EntryKind::SystemInfo, std::string("agent_cli ") + AGENT_SDK_VERSION_STRING + " — Type a message to start. /help for commands.", ""});

  // ===== 使用 Loop 手动控制循环 =====
  Loop loop(&screen, component);

  // 在 FTXUI 初始化终端后，禁用 ISIG 让 Ctrl+C 作为字符输入而非信号
  {
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~ISIG;  // 禁用信号生成（SIGINT, SIGQUIT, SIGTSTP）
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
  }

  while (!loop.HasQuitted()) {
#ifdef AGENT_PLUGIN_QWEN
    // ===== 登录流程处理 =====
    if (needs_qwen_login && state.login_state == LoginState::NeedLogin && !login_started) {
      login_started = true;
      auto& auth = plugin::qwen::qwen_portal_auth();

      // 设置回调函数
      auth.set_status_callback([&state, &screen](const std::string& msg) {
        state.login_status_msg = msg;
        screen.Post(Event::Custom);
      });

      auth.set_user_code_callback([&state, &screen](const std::string& uri, const std::string& code, const std::string& uri_complete) {
        std::string auth_url = uri_complete.empty() ? uri : uri_complete;
        state.login_auth_url = auth_url;
        state.login_user_code = code;
        state.login_qr_code = plugin::QrCode::encode(auth_url);
        state.login_state = LoginState::WaitingAuth;
        screen.Post(Event::Custom);
      });

      // 启动异步认证
      login_future = auth.authenticate();
    }

    // 检查登录是否完成
    if (needs_qwen_login && login_started && state.login_state == LoginState::WaitingAuth) {
      if (login_future.valid() && login_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        auto token = login_future.get();
        if (token) {
          state.login_state = LoginState::Success;
          state.chat_log.push({EntryKind::SystemInfo, "✓ Qwen OAuth 登录成功！", ""});
          needs_qwen_login = false;
        } else {
          state.login_state = LoginState::Failed;
          state.login_error_msg = "认证失败，请重试";
        }
        screen.Post(Event::Custom);
      }
    }
#endif

    loop.RunOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // ===== 清理 =====
  // 保存历史记录
  state.save_history_to_file(history_file);

  ctx.session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();

  return 0;
}
