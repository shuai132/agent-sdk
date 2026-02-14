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
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "core/version.hpp"
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
    g_agent_state.set_activity("Generating...");
    refresh_fn();
  });

  session->on_tool_call([refresh_fn](const std::string& tool, const json& args) {
    std::string args_str = args.dump(2);
    g_tool_panel.start_tool(tool, args_str);
    g_chat_log.push({EntryKind::ToolCall, tool, args_str});
    g_agent_state.set_activity("Running " + tool + "...");
    refresh_fn();
  });

  session->on_tool_result([refresh_fn](const std::string& tool, const std::string& result, bool is_error) {
    std::string summary = result;
    if (summary.size() > 2000) summary = summary.substr(0, 2000) + "\n...(" + std::to_string(result.size()) + " chars total)";
    g_tool_panel.finish_tool(tool, summary, is_error);
    g_chat_log.push({EntryKind::ToolResult, tool + (is_error ? " ✗" : " ✓"), summary});
    g_agent_state.set_activity("Thinking...");
    refresh_fn();
  });

  session->on_complete([refresh_fn](FinishReason reason) {
    if (reason != FinishReason::Stop && reason != FinishReason::ToolCalls) {
      g_chat_log.push({EntryKind::SystemInfo, "Session ended: " + to_string(reason), ""});
    }
    g_agent_state.set_activity("");
    refresh_fn();
  });

  session->on_error([refresh_fn](const std::string& error) {
    g_chat_log.push({EntryKind::Error, error, ""});
    g_agent_state.set_activity("");
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

// 将 session 历史消息回填到 ChatLog
static void load_history_to_chat_log(const std::shared_ptr<Session>& session) {
  const auto& msgs = session->messages();
  if (msgs.empty()) return;

  // 查找最近的摘要消息
  int start_index = 0;
  for (int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i) {
    if (msgs[i].is_summary() && msgs[i].is_finished()) {
      start_index = i;
      break;
    }
  }

  if (start_index > 0) {
    g_chat_log.push({EntryKind::SystemInfo, "[" + std::to_string(start_index) + " earlier messages compacted]", ""});
  }

  for (size_t i = static_cast<size_t>(start_index); i < msgs.size(); ++i) {
    const auto& msg = msgs[i];

    if (msg.role() == Role::System) continue;

    if (msg.is_summary()) {
      g_chat_log.push({EntryKind::SystemInfo, "[Summary] " + msg.text(), ""});
      continue;
    }

    if (msg.role() == Role::User) {
      auto tool_results = msg.tool_results();
      // 跳过纯工具结果载体消息
      if (!tool_results.empty() && msg.text().empty()) continue;
      if (!msg.text().empty()) {
        g_chat_log.push({EntryKind::UserMsg, msg.text(), ""});
      }
    }

    if (msg.role() == Role::Assistant) {
      if (!msg.text().empty()) {
        g_chat_log.push({EntryKind::AssistantText, msg.text(), ""});
      }
      auto tool_calls = msg.tool_calls();
      for (const auto* tc : tool_calls) {
        g_chat_log.push({EntryKind::ToolCall, tc->name, tc->arguments.dump(2)});
        // 找到对应的 tool result
        // 遍历后续消息查找 result
        for (size_t j = i + 1; j < msgs.size(); ++j) {
          auto results = msgs[j].tool_results();
          for (const auto* tr : results) {
            if (tr->tool_call_id == tc->id) {
              std::string summary = tr->output;
              if (summary.size() > 2000) summary = summary.substr(0, 2000) + "...";
              g_chat_log.push({EntryKind::ToolResult, tc->name + (tr->is_error ? " ✗" : " ✓"), summary});
              goto found_result;
            }
          }
        }
      // 没找到对应 result（可能还在运行中）
      found_result:;
      }
    }
  }
}

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

    case EntryKind::SystemInfo: {
      auto lines = split_lines(entry.text);
      Elements elems;
      for (const auto& line : lines) {
        elems.push_back(hbox({text("  "), text(line) | dim}));
      }
      return vbox(elems);
    }

    default:
      return text("");
  }
}

// ============================================================
// 工具调用：圆角边框卡片渲染
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

  // 卡片头部：✓/✗/⏳ + 工具名 + 摘要
  std::string header_text = group.call.text;
  if (!expanded && group.has_result) {
    auto first_line = group.result.detail.substr(0, group.result.detail.find('\n'));
    auto summary = truncate_text(first_line, 80);
    if (!summary.empty()) header_text += "  " + summary;
  }
  if (is_running) header_text += "  running...";

  auto header_line = hbox({
      text(" " + status_icon + "  ") | color(status_color),
      text(header_text) | (is_running ? dim : bold),
  });

  if (!expanded) {
    // 折叠模式：单行圆角边框卡片，不使用 flex
    return hbox({
        text(" "),
        vbox({header_line}) | borderRounded,
    });
  }

  // 展开模式：圆角边框卡片包含参数和结果
  Elements card_content;
  card_content.push_back(header_line);
  card_content.push_back(text(""));

  // 参数区域
  auto args_lines = split_lines(group.call.detail);
  Elements args_elems;
  for (size_t i = 0; i < args_lines.size() && i < 20; ++i) {
    args_elems.push_back(text("   " + args_lines[i]) | dim);
  }
  if (args_lines.size() > 20) {
    args_elems.push_back(text("   ...(" + std::to_string(args_lines.size()) + " lines)") | dim);
  }
  card_content.push_back(text("   Arguments:") | bold | dim);
  for (auto& elem : args_elems) card_content.push_back(elem);

  // 结果区域
  if (group.has_result) {
    card_content.push_back(text(""));
    card_content.push_back(text(is_error ? "   Error:" : "   Result:") | bold | dim | color(status_color));
    auto result_lines = split_lines(group.result.detail);
    for (size_t i = 0; i < result_lines.size() && i < 30; ++i) {
      card_content.push_back(text("   " + result_lines[i]) | dim);
    }
    if (result_lines.size() > 30) {
      card_content.push_back(text("   ...(" + std::to_string(result_lines.size()) + " lines total)") | dim);
    }
  }

  return hbox({
      text(" "),
      vbox(card_content) | borderRounded,
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

  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  // ----- FTXUI 屏幕 -----
  auto screen = ScreenInteractive::Fullscreen();
  screen.TrackMouse(true);  // 启用鼠标追踪，支持滚轮滚动（按住 Shift 仍可选择文字）
  auto refresh_fn = [&screen]() {
    screen.Post(Event::Custom);
  };

  setup_tui_callbacks(session, refresh_fn);

  // ----- 状态 -----
  std::string input_text;
  int input_cursor_pos = 0;
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
  std::map<size_t, bool> tool_expanded;

  // 会话列表面板状态
  bool show_sessions_panel = false;
  int sessions_selected = 0;
  std::vector<SessionMeta> sessions_cache;
  std::vector<Box> session_item_boxes;  // 每个会话条目的屏幕坐标（reflect 捕获）

  // ----- 输入组件 -----
  auto input_option = InputOption();
  input_option.multiline = false;
  input_option.cursor_position = &input_cursor_pos;
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
        input_cursor_pos = static_cast<int>(input_text.size());
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
        std::string h;
        h += "Commands:\n";
        h += "\n";
        for (const auto& def : command_defs()) {
          std::string cmd_col = def.name;
          if (!def.shortcut.empty()) cmd_col += " (" + def.shortcut + ")";
          // 右对齐到 24 字符宽度
          while (cmd_col.size() < 24) cmd_col += ' ';
          h += "  " + cmd_col + def.description + "\n";
        }
        h += "\n";
        h += "Keybindings:\n";
        h += "\n";
        h += "  Esc                   Interrupt running agent\n";
        h += "  Ctrl+C                Press twice to exit\n";
        h += "  Tab                   Switch build/plan mode\n";
        h += "  PageUp / PageDown     Scroll chat history\n";
        g_chat_log.push({EntryKind::SystemInfo, h, ""});
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
      case CommandType::Sessions: {
        auto sessions_list = store->list_sessions();

        if (cmd.arg.empty()) {
          // /sessions — 打开会话列表面板
          sessions_cache = sessions_list;
          sessions_selected = 0;
          // 定位到当前会话
          for (size_t si = 0; si < sessions_cache.size(); ++si) {
            if (sessions_cache[si].id == g_agent_state.session_id()) {
              sessions_selected = static_cast<int>(si);
              break;
            }
          }
          show_sessions_panel = true;
        } else if (cmd.arg == "d" || cmd.arg.substr(0, 2) == "d ") {
          // /s d [N] — 删除会话
          std::string d_arg = (cmd.arg.size() > 2) ? cmd.arg.substr(2) : "";
          if (d_arg.empty() || !std::all_of(d_arg.begin(), d_arg.end(), ::isdigit)) {
            g_chat_log.push({EntryKind::Error, "Usage: /s d <N>", ""});
          } else {
            int d_idx = std::stoi(d_arg);
            if (d_idx < 1 || d_idx > static_cast<int>(sessions_list.size())) {
              g_chat_log.push({EntryKind::Error, "Invalid session number: " + d_arg, ""});
            } else {
              const auto& meta = sessions_list[d_idx - 1];
              bool was_current = (meta.id == g_agent_state.session_id());
              store->remove_session(meta.id);
              g_chat_log.push({EntryKind::SystemInfo, "Deleted session: " + (meta.title.empty() ? "(untitled)" : meta.title), ""});
              if (was_current) {
                session = Session::create(io_ctx, config, AgentType::Build, store);
                g_agent_state.set_session_id(session->id());
                setup_tui_callbacks(session, refresh_fn);
                g_chat_log.push({EntryKind::SystemInfo, "Created new session", ""});
              }
            }
          }
        } else if (std::all_of(cmd.arg.begin(), cmd.arg.end(), ::isdigit)) {
          // /s <N> — 加载会话
          int s_idx = std::stoi(cmd.arg);
          if (s_idx < 1 || s_idx > static_cast<int>(sessions_list.size())) {
            g_chat_log.push({EntryKind::Error, "Invalid session number: " + cmd.arg, ""});
          } else {
            const auto& meta = sessions_list[s_idx - 1];
            session->cancel();
            auto resumed = Session::resume(io_ctx, config, meta.id, store);
            if (resumed) {
              session = resumed;
              g_agent_state.set_session_id(session->id());
              setup_tui_callbacks(session, refresh_fn);
              auto usage = session->total_usage();
              g_agent_state.update_tokens(usage.input_tokens, usage.output_tokens);
              g_chat_log.clear();
              g_tool_panel.clear();
              tool_expanded.clear();
              std::string title = meta.title.empty() ? "(untitled)" : meta.title;
              g_chat_log.push({EntryKind::SystemInfo, "Loaded session: " + title, ""});
              load_history_to_chat_log(session);
            } else {
              g_chat_log.push({EntryKind::Error, "Failed to load session", ""});
            }
          }
        } else {
          g_chat_log.push({EntryKind::Error, "Unknown sessions subcommand: " + cmd.arg, ""});
        }

        input_text.clear();
        return;
      }
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
  auto input_component = Input(&input_text, "输入您的消息或 @ 文件路径", input_option);

  // 包装输入组件：加上 > 提示符
  auto input_with_prompt = Renderer(input_component, [&] {
    return hbox({
        text(" > ") | bold | color(Color::Cyan),
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

    // 活动状态文字（替代 spinner 进度条）
    if (g_agent_state.is_running()) {
      auto activity = g_agent_state.activity();
      if (activity.empty()) activity = "Thinking...";
      chat_elements.push_back(hbox({
          text("    "),
          text(activity) | dim | color(Color::Cyan),
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

    // ========== 输入区域：上下边框线 + > 提示符 + 左下角模式显示 ==========
    auto mode_str = to_string(g_agent_state.mode());
    auto mode_label = text(" " + mode_str + " ") | dim;

    auto input_area = vbox({
        cmd_menu_element,
        separatorHeavy() | dim,
        input_with_prompt->Render(),
        separatorHeavy() | dim,
        hbox({
            mode_label,
            text("  tab to switch mode") | dim,
            filler(),
        }),
    });

    Element main_area;

    if (show_sessions_panel) {
      // ========== 会话列表面板 ==========
      Elements session_items;
      if (sessions_cache.empty()) {
        session_items.push_back(text("  No saved sessions") | dim);
      } else {
        for (int si = 0; si < static_cast<int>(sessions_cache.size()); ++si) {
          const auto& meta = sessions_cache[si];
          bool is_current = (meta.id == g_agent_state.session_id());
          bool is_selected = (si == sessions_selected);
          std::string title = meta.title.empty() ? "(untitled)" : meta.title;
          std::string marker = is_current ? " ●" : "  ";
          std::string detail =
              format_time(meta.updated_at) + "  " + to_string(meta.agent_type) + "  tokens: " + format_tokens(meta.total_usage.total());

          auto row = vbox({
              hbox({
                  text(marker) | color(Color::Green),
                  text(" " + std::to_string(si + 1) + ". ") | dim,
                  text(title) | bold,
              }),
              hbox({
                  text("      "),
                  text(detail) | dim,
              }),
          });

          if (is_selected) {
            row = row | bgcolor(Color::GrayDark) | color(Color::White);
          }
          session_items.push_back(row);
          session_items.push_back(text(""));
        }
      }

      // 重置并填充 session_item_boxes
      session_item_boxes.resize(sessions_cache.size());

      // 将每个会话条目与其间隔行合并为一组，整组加 reflect 捕获屏幕坐标
      // 选中的条目加 focus，让 yframe 仅在超出可见区域时才滚动
      Elements reflected_items;
      for (size_t ri = 0; ri + 1 < session_items.size(); ri += 2) {
        int idx = static_cast<int>(ri / 2);
        if (idx < static_cast<int>(session_item_boxes.size())) {
          auto item = vbox({session_items[ri], session_items[ri + 1]}) | reflect(session_item_boxes[idx]);
          if (idx == sessions_selected) {
            item = item | focus;
          }
          reflected_items.push_back(item);
        }
      }
      if (session_items.size() % 2 == 1) {
        reflected_items.push_back(session_items.back());
      }

      auto session_list = vbox(reflected_items)  //
                          | vscroll_indicator    //
                          | yframe               //
                          | flex;

      auto panel_header = hbox({
          text(" Sessions ") | bold,
          filler(),
          text(" ↑↓ navigate  Enter load  d delete  n new  Esc close ") | dim,
      });

      main_area = vbox({
          status_bar,
          separator() | dim,
          panel_header,
          separator() | dim,
          session_list | flex,
          input_area,
      });
    } else {
      main_area = vbox({
          status_bar,
          separator() | dim,
          chat_view | flex,
          input_area,
      });
    }

    return main_area;
  });

  // ----- 事件处理 -----
  auto component = CatchEvent(final_renderer, [&](Event event) {
    // ===== 会话列表面板专属事件 =====
    if (show_sessions_panel) {
      int count = static_cast<int>(sessions_cache.size());

      if (event == Event::Escape || event == Event::Character('q')) {
        show_sessions_panel = false;
        return true;
      }
      if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (count > 0) sessions_selected = (sessions_selected - 1 + count) % count;
        return true;
      }
      if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (count > 0) sessions_selected = (sessions_selected + 1) % count;
        return true;
      }
      // 鼠标事件
      if (event.is_mouse()) {
        auto& mouse = event.mouse();
        if (mouse.button == Mouse::WheelUp) {
          if (count > 0) sessions_selected = (sessions_selected - 1 + count) % count;
          return true;
        }
        if (mouse.button == Mouse::WheelDown) {
          if (count > 0) sessions_selected = (sessions_selected + 1) % count;
          return true;
        }
        // 鼠标左键点击：通过 reflect 捕获的 Box 精确定位
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed && count > 0) {
          for (int bi = 0; bi < static_cast<int>(session_item_boxes.size()); ++bi) {
            if (session_item_boxes[bi].Contain(mouse.x, mouse.y)) {
              sessions_selected = bi;
              break;
            }
          }
          return true;
        }
        return true;  // 拦截其他鼠标事件
      }
      if (event == Event::Return) {
        // 加载选中的会话
        if (count > 0 && sessions_selected < count) {
          const auto& meta = sessions_cache[sessions_selected];
          session->cancel();
          auto resumed = Session::resume(io_ctx, config, meta.id, store);
          if (resumed) {
            session = resumed;
            g_agent_state.set_session_id(session->id());
            setup_tui_callbacks(session, refresh_fn);
            auto usage = session->total_usage();
            g_agent_state.update_tokens(usage.input_tokens, usage.output_tokens);
            g_chat_log.clear();
            g_tool_panel.clear();
            tool_expanded.clear();
            std::string title = meta.title.empty() ? "(untitled)" : meta.title;
            g_chat_log.push({EntryKind::SystemInfo, "Loaded session: " + title, ""});
            load_history_to_chat_log(session);
          } else {
            g_chat_log.push({EntryKind::Error, "Failed to load session", ""});
          }
          show_sessions_panel = false;
        }
        return true;
      }
      if (event == Event::Character('d')) {
        // 删除选中的会话
        if (count > 0 && sessions_selected < count) {
          const auto& meta = sessions_cache[sessions_selected];
          bool was_current = (meta.id == g_agent_state.session_id());
          store->remove_session(meta.id);
          g_chat_log.push({EntryKind::SystemInfo, "Deleted session: " + (meta.title.empty() ? "(untitled)" : meta.title), ""});
          if (was_current) {
            session = Session::create(io_ctx, config, AgentType::Build, store);
            g_agent_state.set_session_id(session->id());
            setup_tui_callbacks(session, refresh_fn);
            g_chat_log.push({EntryKind::SystemInfo, "Created new session", ""});
          }
          // 刷新列表
          sessions_cache = store->list_sessions();
          if (sessions_selected >= static_cast<int>(sessions_cache.size())) {
            sessions_selected = std::max(0, static_cast<int>(sessions_cache.size()) - 1);
          }
          if (sessions_cache.empty()) show_sessions_panel = false;
        }
        return true;
      }
      if (event == Event::Character('n')) {
        // 新建会话
        session = Session::create(io_ctx, config, AgentType::Build, store);
        g_agent_state.set_session_id(session->id());
        setup_tui_callbacks(session, refresh_fn);
        g_agent_state.update_tokens(0, 0);
        g_chat_log.clear();
        g_tool_panel.clear();
        tool_expanded.clear();
        g_chat_log.push({EntryKind::SystemInfo, "New session created", ""});
        show_sessions_panel = false;
        return true;
      }
      // 拦截所有其他按键，不传递给输入框
      return true;
    }

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
            input_cursor_pos = static_cast<int>(input_text.size());
            show_cmd_menu = false;
          }
          return true;
        }
      }
    }

    // Tab: 切换 Build/Plan 模式（命令菜单不显示时）
    if (event == Event::Tab && !show_cmd_menu) {
      g_agent_state.toggle_mode();
      return true;
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

    // ===== 鼠标事件：滚轮滚动聊天区域 =====
    if (event.is_mouse()) {
      auto& mouse = event.mouse();
      if (mouse.button == Mouse::WheelUp) {
        scroll_y = std::max(0.0f, scroll_y - 0.05f);
        auto_scroll = false;
        return true;
      }
      if (mouse.button == Mouse::WheelDown) {
        scroll_y = std::min(1.0f, scroll_y + 0.05f);
        if (scroll_y >= 0.95f) {
          scroll_y = 1.0f;
          auto_scroll = true;
        }
        return true;
      }
      // 拦截所有鼠标事件，防止终端 scrollback 滚动
      return true;
    }

    return false;
  });

  // ----- 欢迎消息 -----
  g_chat_log.push(
      {EntryKind::SystemInfo, std::string("agent_cli ") + AGENT_SDK_VERSION_STRING + " — Type a message to start. /help for commands.", ""});

  // ----- 运行 TUI -----
  screen.Loop(component);

  // ----- 清理 -----
  session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();

  return 0;
}
