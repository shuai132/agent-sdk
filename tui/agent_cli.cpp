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

using namespace agent;
using namespace agent_cli;
using namespace ftxui;

int main(int argc, char* argv[]) {
  // ===== 加载配置 =====
  Config config = Config::load_default();

  const char* openai_key = std::getenv("OPENAI_API_KEY");
  const char* anthropic_key = std::getenv("ANTHROPIC_API_KEY");
  if (!anthropic_key) anthropic_key = std::getenv("ANTHROPIC_AUTH_TOKEN");

  if (anthropic_key) {
    const char* base_url = std::getenv("ANTHROPIC_BASE_URL");
    const char* model = std::getenv("ANTHROPIC_MODEL");
    config.providers["anthropic"] = ProviderConfig{"anthropic", anthropic_key, base_url ? base_url : "https://api.anthropic.com", std::nullopt, {}};
    if (model) config.default_model = model;
  }

  if (openai_key) {
    const char* base_url = std::getenv("OPENAI_BASE_URL");
    const char* model = std::getenv("OPENAI_MODEL");
    config.providers["openai"] = ProviderConfig{"openai", openai_key, base_url ? base_url : "https://api.openai.com", std::nullopt, {}};
    if (model) {
      config.default_model = model;
    } else if (!anthropic_key) {
      config.default_model = "gpt-4o";
    }
  }

  if (!anthropic_key && !openai_key) {
    std::cerr << "Error: No API key found. Set ANTHROPIC_API_KEY or OPENAI_API_KEY\n";
    return 1;
  }

  // ===== 初始化框架 =====
  asio::io_context io_ctx;
  agent::init();
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
    auto status_bar = build_status_bar(state);
    auto chat_view = build_chat_view(state);
    auto cmd_menu_element = build_cmd_menu(state);
    auto file_path_menu_element = build_file_path_menu(state);

    auto mode_str = to_string(state.agent_state.mode());
    auto input_area = vbox({
        cmd_menu_element,
        file_path_menu_element,
        separatorHeavy() | dim,
        input_with_prompt->Render(),
        separatorHeavy() | dim,
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
