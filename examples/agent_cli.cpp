// agent_cli.cpp — TUI Agent 终端应用（类似 Claude Code 风格）
// 基于 FTXUI 构建：
//  - 聊天区域支持鼠标滚轮滚动 + 右侧滚动条
//  - 工具调用可点击展开/折叠查看详情
//  - 输入 / 时弹出命令补全提示
//  - 状态栏显示模型/token/运行状态

#include <unistd.h>

#include <asio.hpp>
#include <chrono>
#include <csignal>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "tui_components.hpp"

using namespace agent;
using namespace agent_cli;
using namespace ftxui;

// ============================================================
// 全局状态
// ============================================================

static ChatLog g_chat_log;
static ToolPanel g_tool_panel;
static AgentState g_agent_state;

// ============================================================
// Session 回调设置
// ============================================================

static void setup_tui_callbacks(std::shared_ptr<Session>& session, std::function<void()> refresh_fn) {
  session->on_stream([refresh_fn](const std::string& text) {
    g_chat_log.append_stream(text);
    refresh_fn();
  });

  session->on_tool_call([refresh_fn](const std::string& tool, const json& args) {
    std::string args_str = args.dump(2);
    g_tool_panel.start_tool(tool, args_str);
    g_chat_log.push({EntryKind::ToolCall, tool, args_str});
    refresh_fn();
  });

  session->on_tool_result([refresh_fn](const std::string& tool, const std::string& result, bool is_error) {
    std::string summary = result;
    if (summary.size() > 2000) summary = summary.substr(0, 2000) + "\n...(" + std::to_string(result.size()) + " chars total)";
    g_tool_panel.finish_tool(tool, summary, is_error);
    g_chat_log.push({EntryKind::ToolResult, tool + (is_error ? " ✗" : " ✓"), summary});
    refresh_fn();
  });

  session->on_complete([refresh_fn](FinishReason reason) {
    if (reason != FinishReason::Stop && reason != FinishReason::ToolCalls) {
      g_chat_log.push({EntryKind::SystemInfo, "Session ended: " + to_string(reason), ""});
    }
    refresh_fn();
  });

  session->on_error([refresh_fn](const std::string& error) {
    g_chat_log.push({EntryKind::Error, error, ""});
    refresh_fn();
  });

  session->set_permission_handler([refresh_fn](const std::string& permission, const std::string& description) {
    g_chat_log.push({EntryKind::SystemInfo, "Auto-allowed: " + permission, description});
    refresh_fn();
    std::promise<bool> p;
    p.set_value(true);
    return p.get_future();
  });
}

// ============================================================
// 渲染 DOM 元素（纯显示用，不可交互）
// ============================================================

static Element render_text_entry(const ChatEntry& entry) {
  switch (entry.kind) {
    case EntryKind::UserMsg:
      return vbox({
          hbox({text("  ❯ ") | color(Color::Green), text("You") | bold | color(Color::Green)}),
          hbox({text("    "), paragraph(entry.text)}),
          text(""),
      });

    case EntryKind::AssistantText: {
      auto lines = split_lines(entry.text);
      Elements content;
      for (const auto& line : lines) {
        content.push_back(paragraph(line));
      }
      return vbox({
          hbox({text("  ✦ ") | color(Color::Cyan), text("AI") | bold | color(Color::Cyan)}),
          hbox({text("    "), vbox(content) | flex}) | flex,
          text(""),
      });
    }

    case EntryKind::SubtaskStart:
      return hbox({
          text("    ◈ Subtask: ") | color(Color::Magenta) | bold,
          text(entry.text) | color(Color::Magenta),
      });

    case EntryKind::SubtaskEnd:
      return hbox({
          text("    ◈ Done: ") | color(Color::Magenta),
          text(truncate_text(entry.text, 100)) | dim,
      });

    case EntryKind::Error:
      return hbox({
          text("  ✗ ") | color(Color::Red) | bold,
          paragraph(entry.text) | color(Color::Red),
      });

    case EntryKind::SystemInfo:
      return hbox({text("  "), text(entry.text) | dim});

    default:
      return text("");
  }
}

// ============================================================
// 可点击展开/折叠的工具调用组件
// ============================================================

// 一组 ToolCall + ToolResult 配对
struct ToolGroup {
  ChatEntry call;
  ChatEntry result;  // kind == SystemInfo 如果还没有结果
  bool has_result = false;
};

static Component make_tool_component(ToolGroup group, std::shared_ptr<bool> expanded) {
  return Renderer([group, expanded](bool focused) {
           bool is_expanded = *expanded;
           bool is_error = group.has_result && group.result.text.find("✗") != std::string::npos;
           bool is_running = !group.has_result;

           // 状态图标
           std::string status_icon = is_running ? "⏳" : (is_error ? "✗" : "✓");
           Color status_color = is_running ? Color::Yellow : (is_error ? Color::Red : Color::Green);
           std::string arrow = is_expanded ? "▼ " : "▶ ";

           // 头部行
           auto header = hbox({
               text("    "),
               text(arrow) | color(Color::Yellow),
               text(status_icon + " ") | color(status_color),
               text(group.call.text) | bold | color(Color::Yellow),
               text(is_running ? "  running..." : "") | dim,
           });

           if (focused) {
             header = header | bgcolor(Color::GrayDark);
           }

           if (!is_expanded) {
             return header;
           }

           // 展开内容：参数
           auto args_lines = split_lines(group.call.detail);
           Elements args_elems;
           for (size_t i = 0; i < args_lines.size() && i < 20; ++i) {
             args_elems.push_back(text(args_lines[i]));
           }
           if (args_lines.size() > 20) {
             args_elems.push_back(text("...(" + std::to_string(args_lines.size()) + " lines)") | dim);
           }

           Elements expanded_parts;
           expanded_parts.push_back(header);

           // 参数区域
           expanded_parts.push_back(hbox({text("      "), vbox({text("Arguments:") | bold | dim, vbox(args_elems) | dim}) | flex | borderLight}) |
                                    flex);

           // 结果区域
           if (group.has_result) {
             auto result_lines = split_lines(group.result.detail);
             Elements result_elems;
             for (size_t i = 0; i < result_lines.size() && i < 30; ++i) {
               result_elems.push_back(text(result_lines[i]));
             }
             if (result_lines.size() > 30) {
               result_elems.push_back(text("...(" + std::to_string(result_lines.size()) + " lines total)") | dim);
             }
             expanded_parts.push_back(
                 hbox({text("      "), vbox({text(is_error ? "Error:" : "Result:") | bold | dim | color(status_color), vbox(result_elems) | dim}) |
                                           flex | borderLight}) |
                 flex);
           }

           return vbox(expanded_parts);
         }) |
         CatchEvent([expanded](Event event) {
           if (event == Event::Return || (event.is_mouse() && event.mouse().button == Mouse::Left && event.mouse().motion == Mouse::Pressed)) {
             *expanded = !*expanded;
             return true;
           }
           return false;
         });
}

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[]) {
  // ----- 加载配置 -----
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

  g_agent_state.set_model(config.default_model);

  // ----- 初始化框架 -----
  asio::io_context io_ctx;
  agent::init();
  auto store = std::make_shared<JsonMessageStore>(config_paths::config_dir() / "sessions");
  auto session = Session::create(io_ctx, config, AgentType::Build, store);
  g_agent_state.set_session_id(session->id());

  // IO 线程
  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  // ----- FTXUI 屏幕 -----
  auto screen = ScreenInteractive::Fullscreen();
  auto refresh_fn = [&screen]() {
    screen.Post(Event::Custom);
  };

  setup_tui_callbacks(session, refresh_fn);

  // ----- 状态 -----
  std::string input_text;
  int cmd_menu_selected = 0;
  bool show_cmd_menu = false;

  // 聊天区域的组件列表和展开状态追踪
  // 由于 entries 在后台线程中更新，我们在渲染时动态重建组件树
  struct ToolExpandState {
    std::shared_ptr<bool> expanded;
  };
  std::map<size_t, ToolExpandState> tool_expand_states;  // key = ToolCall entry index

  // ----- 输入组件 -----
  auto input_option = InputOption();
  input_option.multiline = false;
  input_option.on_change = [&] {
    if (!input_text.empty() && input_text[0] == '/') {
      auto matches = match_commands(input_text);
      show_cmd_menu = !matches.empty();
      cmd_menu_selected = 0;
    } else {
      show_cmd_menu = false;
    }
  };
  input_option.on_enter = [&] {
    if (show_cmd_menu) {
      auto matches = match_commands(input_text);
      if (!matches.empty() && cmd_menu_selected < static_cast<int>(matches.size())) {
        input_text = matches[cmd_menu_selected].name;
        show_cmd_menu = false;
        return;
      }
    }

    if (input_text.empty()) return;
    show_cmd_menu = false;

    auto cmd = parse_command(input_text);
    switch (cmd.type) {
      case CommandType::Quit:
        screen.Exit();
        return;
      case CommandType::Clear:
        g_chat_log.clear();
        g_tool_panel.clear();
        tool_expand_states.clear();
        input_text.clear();
        return;
      case CommandType::Help: {
        std::string help_text = "Available commands:";
        for (const auto& def : command_defs()) {
          help_text += "\n  " + def.name;
          if (!def.shortcut.empty()) help_text += " (" + def.shortcut + ")";
          help_text += " — " + def.description;
        }
        help_text += "\n  Esc — Interrupt running agent";
        g_chat_log.push({EntryKind::SystemInfo, help_text, ""});
        input_text.clear();
        return;
      }
      case CommandType::Compact:
        g_chat_log.push({EntryKind::SystemInfo, "Context compaction triggered", ""});
        input_text.clear();
        return;
      case CommandType::Unknown:
        g_chat_log.push({EntryKind::Error, "Unknown command: " + cmd.arg, ""});
        input_text.clear();
        return;
      default:
        break;
    }

    std::string user_msg = input_text;
    input_text.clear();

    if (g_agent_state.is_running()) {
      g_chat_log.push({EntryKind::SystemInfo, "Agent is busy, please wait...", ""});
      return;
    }

    g_chat_log.push({EntryKind::UserMsg, user_msg, ""});
    g_agent_state.set_running(true);

    std::thread([&session, user_msg, refresh_fn]() {
      session->prompt(user_msg);
      auto usage = session->total_usage();
      g_agent_state.update_tokens(usage.input_tokens, usage.output_tokens);
      g_agent_state.set_running(false);
      refresh_fn();
    }).detach();
  };
  auto input_component = Input(&input_text, "Message agent_cli...", input_option);

  // ----- 主渲染器 -----
  // 用 Container::Vertical 来承载所有可交互的条目组件，
  // 这样滚轮和焦点跟随都由 FTXUI 自动处理
  auto chat_container = Container::Vertical({});

  // 追踪上一次的 entry 数量，增量更新组件
  size_t last_entry_count = 0;

  auto main_container = Container::Vertical({
      chat_container,
      input_component,
  });

  auto renderer = Renderer(main_container, [&] {
    auto entries = g_chat_log.snapshot();

    // ========== 增量构建聊天组件 ==========
    if (entries.size() != last_entry_count) {
      // 重建组件树
      chat_container->DetachAllChildren();

      for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];

        if (e.kind == EntryKind::ToolCall) {
          // 查找配对的 ToolResult
          ToolGroup group;
          group.call = e;
          if (i + 1 < entries.size() && entries[i + 1].kind == EntryKind::ToolResult) {
            group.result = entries[i + 1];
            group.has_result = true;
          }

          // 获取或创建展开状态
          if (tool_expand_states.find(i) == tool_expand_states.end()) {
            tool_expand_states[i] = {std::make_shared<bool>(false)};
          }

          auto comp = make_tool_component(group, tool_expand_states[i].expanded);
          chat_container->Add(comp);
          continue;
        }

        // 跳过已配对的 ToolResult（已合并到 ToolCall 组件中）
        if (e.kind == EntryKind::ToolResult && i > 0 && entries[i - 1].kind == EntryKind::ToolCall) {
          continue;
        }

        // 普通条目：包装为 Renderer 组件
        auto entry_copy = e;
        chat_container->Add(Renderer([entry_copy] {
          return render_text_entry(entry_copy);
        }));
      }

      // 新消息时滚动到底部：将焦点设到最后一个子组件
      if (!chat_container->ChildCount()) {
        // 空
      }

      last_entry_count = entries.size();
    }

    // Agent 运行动画
    Element spinner_elem = text("");
    if (g_agent_state.is_running()) {
      spinner_elem = hbox({
          text("    "),
          spinner(18, entries.size()) | color(Color::Cyan),
          text("  ") | dim,
      });
    }

    // 聊天视图
    auto chat_view = vbox({
                         chat_container->Render(),
                         spinner_elem,
                         text(""),
                     }) |
                     vscroll_indicator | yframe | flex;

    // ========== 状态栏 ==========
    auto status_bar = hbox({
        text(" agent_cli ") | bold | color(Color::White) | bgcolor(Color::Blue),
        text(" "),
        text(g_agent_state.model()) | dim,
        filler(),
        text(format_tokens(g_agent_state.input_tokens()) + "↑ " + format_tokens(g_agent_state.output_tokens()) + "↓") | dim,
        text(" "),
        text(g_agent_state.is_running() ? " ● Running " : " ● Ready ") | color(Color::White) |
            bgcolor(g_agent_state.is_running() ? Color::Yellow : Color::Green),
    });

    // ========== 输入区域 ==========
    auto prompt_char = g_agent_state.is_running() ? "..." : "❯";
    auto input_area = hbox({
        text(" " + std::string(prompt_char) + " ") | bold | color(Color::Cyan),
        input_component->Render() | flex,
    });

    // ========== 命令提示菜单 ==========
    Element cmd_menu_element = text("");
    if (show_cmd_menu && !input_text.empty()) {
      auto matches = match_commands(input_text);
      if (!matches.empty()) {
        Elements menu_items;
        for (int j = 0; j < static_cast<int>(matches.size()); ++j) {
          auto& def = matches[j];
          bool selected = (j == cmd_menu_selected);
          auto item = hbox({
              text("  "),
              text(def.name) | bold,
              text(def.shortcut.empty() ? "" : " (" + def.shortcut + ")") | dim,
              text("  "),
              text(def.description) | dim,
          });
          if (selected) {
            item = item | bgcolor(Color::GrayDark) | color(Color::White);
          }
          menu_items.push_back(item);
        }
        cmd_menu_element = vbox(menu_items) | borderRounded | color(Color::GrayLight);
      }
    }

    // ========== 最终布局 ==========
    return vbox({
        status_bar,
        separator() | dim,
        chat_view | flex,
        separator() | dim,
        cmd_menu_element,
        input_area,
    });
  });

  // ----- 事件处理 -----
  auto component = CatchEvent(renderer, [&](Event event) {
    // Esc: 中断 agent 或关闭菜单
    if (event == Event::Escape) {
      if (g_agent_state.is_running()) {
        session->cancel();
        g_agent_state.set_running(false);
        g_chat_log.push({EntryKind::SystemInfo, "Interrupted", ""});
        return true;
      }
      if (show_cmd_menu) {
        show_cmd_menu = false;
        return true;
      }
      return false;
    }

    // Ctrl+C
    if (event == Event::Special("\x03")) {
      if (g_agent_state.is_running()) {
        session->cancel();
        g_agent_state.set_running(false);
        g_chat_log.push({EntryKind::SystemInfo, "Interrupted", ""});
        return true;
      }
      return false;
    }

    // 命令菜单导航
    if (show_cmd_menu) {
      auto matches = match_commands(input_text);
      int count = static_cast<int>(matches.size());
      if (count > 0) {
        if (event == Event::ArrowUp) {
          cmd_menu_selected = (cmd_menu_selected - 1 + count) % count;
          return true;
        }
        if (event == Event::ArrowDown) {
          cmd_menu_selected = (cmd_menu_selected + 1) % count;
          return true;
        }
        if (event == Event::Tab) {
          if (cmd_menu_selected < count) {
            input_text = matches[cmd_menu_selected].name;
            show_cmd_menu = false;
          }
          return true;
        }
      }
    }

    // PageUp / PageDown / Home / End 由 Container::Vertical 自动处理
    return false;
  });

  // ----- 欢迎消息 -----
  g_chat_log.push({EntryKind::SystemInfo, "agent_cli — Type a message to start. /help for commands.", ""});

  // ----- 运行 TUI -----
  screen.Loop(component);

  // ----- 清理 -----
  session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();

  return 0;
}
