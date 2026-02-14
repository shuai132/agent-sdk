// agent_cli.cpp — TUI Agent 终端应用（类似 Claude Code 风格）
// 基于 FTXUI 构建：
//  - 聊天区域支持鼠标滚轮滚动 + 右侧滚动条
//  - 工具调用可点击展开/折叠查看详情
//  - 输入 / 时弹出命令补全提示
//  - 状态栏显示模型/token/运行状态

#include <unistd.h>

#include <algorithm>
#include <asio.hpp>
#include <chrono>
#include <csignal>
#include <filesystem>
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
// 渲染 DOM 元素（纯显示用）
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
// 工具调用：折叠/展开的渲染
// ============================================================

struct ToolGroup {
  ChatEntry call;
  ChatEntry result;
  bool has_result = false;
};

static Element render_tool_group(const ToolGroup& group, bool expanded) {
  bool is_error = group.has_result && group.result.text.find("✗") != std::string::npos;
  bool is_running = !group.has_result;

  std::string status_icon = is_running ? "⏳" : (is_error ? "✗" : "✓");
  Color status_color = is_running ? Color::Yellow : (is_error ? Color::Red : Color::Green);
  std::string arrow = expanded ? "▼ " : "▶ ";

  // 生成摘要：从参数或结果中提取一行简短描述
  std::string summary;
  if (!expanded) {
    if (group.has_result) {
      // 优先用结果的第一行
      auto first_line = group.result.detail.substr(0, group.result.detail.find('\n'));
      summary = truncate_text(first_line, 80);
    } else if (!group.call.detail.empty()) {
      // 用参数的第一行
      auto first_line = group.call.detail.substr(0, group.call.detail.find('\n'));
      summary = truncate_text(first_line, 80);
    }
  }

  auto header = hbox({
      text("    "),
      text(arrow) | color(Color::Yellow),
      text(status_icon + " ") | color(status_color),
      text(group.call.text) | bold | color(Color::Yellow),
      text(is_running ? "  running..." : "") | dim,
      text(summary.empty() ? "" : "  " + summary) | dim,
  });

  if (!expanded) {
    return header;
  }

  // 展开内容
  Elements expanded_parts;
  expanded_parts.push_back(header);

  // 参数
  auto args_lines = split_lines(group.call.detail);
  Elements args_elems;
  for (size_t i = 0; i < args_lines.size() && i < 20; ++i) {
    args_elems.push_back(text(args_lines[i]));
  }
  if (args_lines.size() > 20) {
    args_elems.push_back(text("...(" + std::to_string(args_lines.size()) + " lines)") | dim);
  }
  expanded_parts.push_back(hbox({text("      "), vbox({text("Arguments:") | bold | dim, vbox(args_elems) | dim}) | flex | borderLight}) | flex);

  // 结果
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
        hbox({text("      "),
              vbox({text(is_error ? "Error:" : "Result:") | bold | dim | color(status_color), vbox(result_elems) | dim}) | flex | borderLight}) |
        flex);
  }

  return vbox(expanded_parts);
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

  // 滚动控制
  float scroll_y = 1.0f;          // 0.0=顶部, 1.0=底部
  bool auto_scroll = true;        // 新消息自动滚到底，用户上滚后暂停
  size_t last_snapshot_size = 0;  // 检测内容变化以触发自动滚动

  // Ctrl+C 两次退出
  bool ctrl_c_pending = false;
  std::chrono::steady_clock::time_point ctrl_c_time;

  // 工具调用展开状态 (key = ToolCall 在 snapshot 中的 index)
  // 鼠标追踪已禁用（支持终端文本选择），工具调用默认展开
  std::map<size_t, bool> tool_expanded;

  // ----- 输入组件 -----
  auto input_option = InputOption();
  input_option.multiline = false;
  input_option.transform = [](InputState state) {
    if (state.is_placeholder) {
      state.element |= dim | color(Color::GrayDark);
    }
    return state.element;
  };
  input_option.on_change = [&] {
    if (!input_text.empty() && input_text[0] == '/') {
      auto matches = match_commands(input_text);
      show_cmd_menu = !matches.empty();
      cmd_menu_selected = 0;
    } else {
      show_cmd_menu = false;
    }
  };

  // 提交处理函数：命令解析 + 消息发送
  auto handle_submit = [&] {
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
        tool_expanded.clear();
        scroll_y = 1.0f;
        auto_scroll = true;
        last_snapshot_size = 0;
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
        help_text += "\n  Ctrl+C — Press twice to exit";
        help_text += "\n\nScroll: PageUp/PageDown, Mouse wheel";
        help_text += "\nTool calls: Collapsed by default. Use /expand or /collapse to toggle.";
        g_chat_log.push({EntryKind::SystemInfo, help_text, ""});
        input_text.clear();
        return;
      }
      case CommandType::Compact:
        g_chat_log.push({EntryKind::SystemInfo, "Context compaction triggered", ""});
        input_text.clear();
        return;
      case CommandType::Expand:
        for (auto& [k, v] : tool_expanded) v = true;
        for (size_t i = 0; i < g_chat_log.size(); ++i) {
          tool_expanded[i] = true;
        }
        g_chat_log.push({EntryKind::SystemInfo, "All tool calls expanded", ""});
        input_text.clear();
        return;
      case CommandType::Collapse:
        for (auto& [k, v] : tool_expanded) v = false;
        for (size_t i = 0; i < g_chat_log.size(); ++i) {
          tool_expanded[i] = false;
        }
        g_chat_log.push({EntryKind::SystemInfo, "All tool calls collapsed", ""});
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
    auto_scroll = true;
    scroll_y = 1.0f;

    std::thread([&session, user_msg, refresh_fn]() {
      session->prompt(user_msg);
      auto usage = session->total_usage();
      g_agent_state.update_tokens(usage.input_tokens, usage.output_tokens);
      g_agent_state.set_running(false);
      refresh_fn();
    }).detach();
  };

  // on_enter 直接调用 handle_submit
  input_option.on_enter = handle_submit;
  auto input_component = Input(&input_text, "Message agent_cli...", input_option);

  // 包装输入组件：加上 prompt 前缀，保持光标位置正确
  auto input_with_prompt = Renderer(input_component, [&] {
    auto prompt_char = g_agent_state.is_running() ? "..." : "❯";
    return hbox({
        text(" " + std::string(prompt_char) + " ") | bold | color(Color::Cyan),
        input_component->Render() | flex,
    });
  });

  // ----- 主渲染器 -----
  // input_with_prompt 是唯一的可聚焦组件，负责接收所有键盘事件
  // chat 区域通过渲染函数生成，不参与焦点管理
  auto final_renderer = Renderer(input_with_prompt, [&] {
    auto entries = g_chat_log.snapshot();

    // 检测内容变化，自动滚动到底部
    size_t current_size = entries.size();
    // 简单检测：entry 数量变了，或者最后一条 entry 的文本长度变了（流式追加）
    bool content_changed = (current_size != last_snapshot_size);
    if (!content_changed && !entries.empty() && entries.back().kind == EntryKind::AssistantText) {
      // 流式追加时 entry 数量不变但内容变了——每帧都触发
      content_changed = true;
    }
    last_snapshot_size = current_size;

    if (auto_scroll && content_changed) {
      scroll_y = 1.0f;
    }

    // ========== 构建聊天内容 DOM ==========
    Elements chat_elements;
    chat_elements.push_back(text(""));

    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& e = entries[i];

      if (e.kind == EntryKind::ToolCall) {
        // 构建 ToolGroup
        ToolGroup group;
        group.call = e;
        if (i + 1 < entries.size() && entries[i + 1].kind == EntryKind::ToolResult) {
          group.result = entries[i + 1];
          group.has_result = true;
        }

        // 工具调用默认折叠，用 /expand 展开
        bool expanded = tool_expanded.count(i) && tool_expanded[i];
        chat_elements.push_back(render_tool_group(group, expanded));
        continue;
      }

      // 跳过已配对的 ToolResult
      if (e.kind == EntryKind::ToolResult && i > 0 && entries[i - 1].kind == EntryKind::ToolCall) {
        continue;
      }

      chat_elements.push_back(render_text_entry(e));
    }

    // spinner
    if (g_agent_state.is_running()) {
      chat_elements.push_back(hbox({
          text("    "),
          spinner(18, entries.size()) | color(Color::Cyan),
          text("  ") | dim,
      }));
    }

    chat_elements.push_back(text(""));

    auto chat_view = vbox(chat_elements)                     //
                     | focusPositionRelative(0.f, scroll_y)  //
                     | vscroll_indicator                     //
                     | yframe                                //
                     | flex;

    // ========== 状态栏 ==========
    auto status_bar = hbox({
        text(" " + std::filesystem::current_path().filename().string() + " ") | bold | color(Color::White) | bgcolor(Color::Blue),
        text(" "),
        text(g_agent_state.model()) | dim,
        filler(),
        text(format_tokens(g_agent_state.input_tokens()) + "↑ " + format_tokens(g_agent_state.output_tokens()) + "↓") | dim,
        text(" "),
        text(g_agent_state.is_running() ? " ● Running " : " ● Ready ") | color(Color::White) |
            bgcolor(g_agent_state.is_running() ? Color::Yellow : Color::Green),
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
    // 底部输入区域：固定高度，让输入框视觉上居中
    auto input_area = vbox({
                          cmd_menu_element,
                          text(""),
                          text(""),
                          separator() | dim,
                          text(""),
                          input_with_prompt->Render(),
                          text(""),
                      }) |
                      size(HEIGHT, EQUAL, 7);

    return vbox({
        status_bar,
        separator() | dim,
        chat_view | flex,
        input_area,
    });
  });

  // ----- 事件处理 -----
  auto component = CatchEvent(final_renderer, [&](Event event) {
    // 任何非 Ctrl+C 的按键都重置 ctrl_c_pending
    if (event != Event::Special("\x03")) {
      ctrl_c_pending = false;
    }

    // Esc: 终止当前任务，或关闭菜单
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
      return true;
    }

    // Ctrl+C: 第一次提示，2 秒内再按退出
    if (event == Event::Special("\x03")) {
      if (g_agent_state.is_running()) {
        session->cancel();
        g_agent_state.set_running(false);
        g_chat_log.push({EntryKind::SystemInfo, "Interrupted", ""});
        ctrl_c_pending = false;
        return true;
      }
      auto now = std::chrono::steady_clock::now();
      if (ctrl_c_pending && (now - ctrl_c_time) < std::chrono::seconds(2)) {
        screen.Exit();
        return true;
      }
      ctrl_c_pending = true;
      ctrl_c_time = now;
      g_chat_log.push({EntryKind::SystemInfo, "Press Ctrl+C again to exit", ""});
      return true;
    }

    // Enter: 在 CatchEvent 层直接处理提交，确保命令不会被 Input 吞掉
    if (event == Event::Return) {
      handle_submit();
      return true;
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

    // ===== 鼠标滚轮 =====
    if (event.is_mouse()) {
      auto& m = event.mouse();
      if (m.button == Mouse::WheelUp) {
        scroll_y = std::max(0.0f, scroll_y - 0.05f);
        auto_scroll = false;
        return true;
      }
      if (m.button == Mouse::WheelDown) {
        scroll_y = std::min(1.0f, scroll_y + 0.05f);
        if (scroll_y >= 0.95f) {
          scroll_y = 1.0f;
          auto_scroll = true;
        }
        return true;
      }
    }

    // ===== PageUp / PageDown =====
    if (event == Event::PageUp) {
      scroll_y = std::max(0.0f, scroll_y - 0.3f);
      auto_scroll = false;
      return true;
    }
    if (event == Event::PageDown) {
      scroll_y = std::min(1.0f, scroll_y + 0.3f);
      if (scroll_y >= 0.95f) {
        scroll_y = 1.0f;
        auto_scroll = true;
      }
      return true;
    }

    return false;
  });

  // ----- 欢迎消息 -----
  g_chat_log.push({EntryKind::SystemInfo, "agent_cli — Type a message to start. /help for commands. PageUp/Down to scroll.", ""});

  // ----- 运行 TUI -----
  screen.Loop(component);

  // ----- 清理 -----
  session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();

  return 0;
}
