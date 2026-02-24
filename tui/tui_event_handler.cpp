#include "tui_event_handler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ftxui/component/component.hpp>
#include <sstream>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "tui_callbacks.h"
#include "tui_components.h"
#include "tui_state.h"

namespace agent_cli {

using namespace ftxui;

// ============================================================
// 命令提交处理
// ============================================================

void handle_submit(AppState& state, AppContext& ctx, ScreenInteractive& screen) {
  // 命令菜单补全
  if (state.show_cmd_menu) {
    auto matches = match_commands(state.input_text);
    if (!matches.empty() && state.cmd_menu_selected < static_cast<int>(matches.size())) {
      state.input_text = matches[state.cmd_menu_selected].name;
      state.input_cursor_pos = static_cast<int>(state.input_text.size());
      state.show_cmd_menu = false;
      return;
    }
  }

  // 文件路径菜单补全
  if (state.show_file_path_menu) {
    int count = static_cast<int>(state.file_path_matches.size());
    if (count > 0 && state.file_path_menu_selected < count) {
      // 获取当前输入文本直到最后的 @ 符号
      size_t at_pos = state.input_text.rfind('@');
      if (at_pos != std::string::npos) {
        std::string before_at = state.input_text.substr(0, at_pos + 1);
        std::string path_to_insert = state.file_path_matches[state.file_path_menu_selected].path;

        // 如果是目录，添加斜杠
        if (state.file_path_matches[state.file_path_menu_selected].is_directory) {
          path_to_insert += "/";
        }

        // 添加一个空格到路径后面
        path_to_insert += " ";

        state.input_text = before_at + path_to_insert;
        state.input_cursor_pos = static_cast<int>(state.input_text.size());
        state.show_file_path_menu = false;
        state.file_path_matches.clear();
        return;  // 不继续处理提交
      }
    }
  }

  if (state.input_text.empty()) return;
  state.show_cmd_menu = false;
  state.show_file_path_menu = false;
  state.file_path_matches.clear();

  auto cmd = parse_command(state.input_text);
  switch (cmd.type) {
    case CommandType::Quit:
      screen.Exit();
      return;

    case CommandType::Clear:
      state.clear_all();
      state.input_text.clear();
      return;

    case CommandType::Help: {
      std::string h;
      h += "📋 Commands:\n\n";
      for (const auto& def : command_defs()) {
        std::string cmd_col = def.name;
        if (!def.shortcut.empty()) cmd_col += " (" + def.shortcut + ")";
        while (cmd_col.size() < 24) cmd_col += ' ';
        h += "  " + cmd_col + def.description + "\n";
      }
      h += "\n⌨️  Keybindings:\n\n";
      h += "  Esc                   Interrupt running agent\n";
      h += "  Ctrl+C                Press twice to exit\n";
      h += "  Tab                   Switch Build/Plan mode\n";
      h += "  PageUp / PageDown     Scroll chat history\n";
      h += "  ↑ / ↓                 Navigate command/file menu\n";
      h += "\n🖱️  Mouse:\n\n";
      h += "  Click on tool card    Expand/collapse tool details\n";
      h += "  Scroll wheel          Scroll chat history\n";
      h += "\n💡 Tips:\n\n";
      h += "  • Type @ to autocomplete file paths\n";
      h += "  • Type / to see available commands\n";
      h += "  • Use /sessions to manage multiple conversations\n";
      state.chat_log.push({EntryKind::SystemInfo, h, ""});
      state.input_text.clear();
      return;
    }

    case CommandType::Compact:
      state.chat_log.push({EntryKind::SystemInfo, "Context compaction triggered", ""});
      state.input_text.clear();
      return;

    case CommandType::Expand:
      for (auto& [k, v] : state.tool_expanded) v = true;
      for (size_t i = 0; i < state.chat_log.size(); ++i) state.tool_expanded[i] = true;
      state.chat_log.push({EntryKind::SystemInfo, "All tool calls expanded", ""});
      state.input_text.clear();
      return;

    case CommandType::Collapse:
      for (auto& [k, v] : state.tool_expanded) v = false;
      for (size_t i = 0; i < state.chat_log.size(); ++i) state.tool_expanded[i] = false;
      state.chat_log.push({EntryKind::SystemInfo, "All tool calls collapsed", ""});
      state.input_text.clear();
      return;

    case CommandType::Copy: {
      // 生成纯文本格式的聊天内容
      auto entries = state.chat_log.snapshot();
      std::ostringstream oss;

      for (const auto& e : entries) {
        switch (e.kind) {
          case EntryKind::UserMsg:
            oss << "User:\n" << e.text << "\n\n";
            break;
          case EntryKind::AssistantText:
            oss << "AI:\n" << e.text << "\n\n";
            break;
          case EntryKind::Thinking:
            oss << "AI Thinking:\n" << e.text << "\n\n";
            break;
          case EntryKind::ToolCall:
            oss << "Tool Call: " << e.text << "\n";
            if (!e.detail.empty()) {
              oss << "Arguments:\n" << e.detail << "\n";
            }
            break;
          case EntryKind::ToolResult:
            oss << "Tool Result:\n" << e.detail << "\n\n";
            break;
          case EntryKind::SubtaskStart:
            oss << "Subtask: " << e.text << "\n";
            break;
          case EntryKind::SubtaskEnd:
            oss << "Subtask Done: " << e.text << "\n";
            break;
          case EntryKind::Error:
            oss << "Error: " << e.text << "\n\n";
            break;
          case EntryKind::SystemInfo:
            oss << "System: " << e.text << "\n\n";
            break;
        }
      }

      std::string content = oss.str();

      // 使用系统命令复制到剪贴板
      // macOS: pbcopy, Linux: xclip or xsel, Windows: clip
      std::string copy_cmd;
#ifdef __APPLE__
      copy_cmd = "pbcopy";
#elif defined(_WIN32)
      copy_cmd = "clip";
#else
      // Linux: 尝试 xclip，如果不存在则尝试 xsel
      if (system("which xclip >/dev/null 2>&1") == 0) {
        copy_cmd = "xclip -selection clipboard";
      } else if (system("which xsel >/dev/null 2>&1") == 0) {
        copy_cmd = "xsel --clipboard --input";
      } else {
        state.chat_log.push({EntryKind::Error, "No clipboard utility found. Install xclip or xsel.", ""});
        state.input_text.clear();
        return;
      }
#endif

      FILE* pipe = popen(copy_cmd.c_str(), "w");
      if (pipe) {
        fwrite(content.c_str(), 1, content.size(), pipe);
        int result = pclose(pipe);
        if (result == 0) {
          state.chat_log.push({EntryKind::SystemInfo, "Chat content copied to clipboard (" + std::to_string(content.size()) + " bytes)", ""});
        } else {
          state.chat_log.push({EntryKind::Error, "Failed to copy to clipboard", ""});
        }
      } else {
        state.chat_log.push({EntryKind::Error, "Failed to open clipboard utility", ""});
      }
      state.input_text.clear();
      return;
    }

    case CommandType::Sessions:
      handle_sessions_command(state, ctx, cmd.arg);
      state.input_text.clear();
      return;

    case CommandType::Unknown:
      state.chat_log.push({EntryKind::Error, "Unknown command: " + cmd.arg, ""});
      state.input_text.clear();
      return;

    default:
      break;
  }

  // 普通消息
  std::string user_msg = state.input_text;

  if (state.agent_state.is_running()) {
    state.chat_log.push({EntryKind::SystemInfo, "Agent is busy, please wait...", ""});
    return;
  }

  // 将用户消息添加到历史记录（除非它已经是历史记录的一部分）
  if (!user_msg.empty()) {
    if (state.input_history.empty() || state.input_history.back() != user_msg) {
      state.input_history.push_back(user_msg);
    }
    // 重置历史记录索引，以便下次按向上箭头可以看到最新历史
    state.history_index = -1;
  }

  state.chat_log.push({EntryKind::UserMsg, user_msg, ""});
  state.input_text.clear();  // 清空输入框
  state.agent_state.set_running(true);
  
  // 启动或恢复会话计时器
  if (auto duration = state.agent_state.session_duration_ms(); !duration) {
    state.agent_state.start_session_timer();
  } else {
    state.agent_state.resume_session_timer();
  }
  
  state.auto_scroll = true;
  state.scroll_y = 1.0f;

  auto& session = ctx.session;
  auto refresh_fn = ctx.refresh_fn;
  std::thread([&session, &state, user_msg, refresh_fn]() {
    session->prompt(user_msg);
    auto usage = session->total_usage();
    state.agent_state.update_tokens(usage.input_tokens, usage.output_tokens);
    state.agent_state.update_context(session->estimated_context_tokens(), session->context_window());
    state.agent_state.set_running(false);
    // 不在这里暂停计时器，让它在 on_complete 回调中暂停
    refresh_fn();
  }).detach();
}

// ============================================================
// 会话命令处理
// ============================================================

void handle_sessions_command(AppState& state, AppContext& ctx, const std::string& arg) {
  auto sessions_list = ctx.store->list_sessions();

  if (arg.empty()) {
    // 打开会话列表面板
    state.sessions_cache = sessions_list;
    state.sessions_selected = 0;
    for (size_t si = 0; si < state.sessions_cache.size(); ++si) {
      if (state.sessions_cache[si].id == state.agent_state.session_id()) {
        state.sessions_selected = static_cast<int>(si);
        break;
      }
    }
    state.show_sessions_panel = true;
  } else if (arg == "d" || arg.substr(0, 2) == "d ") {
    // 删除会话
    std::string d_arg = (arg.size() > 2) ? arg.substr(2) : "";
    if (d_arg.empty() || !std::all_of(d_arg.begin(), d_arg.end(), ::isdigit)) {
      state.chat_log.push({EntryKind::Error, "Usage: /s d <N>", ""});
    } else {
      int d_idx = std::stoi(d_arg);
      if (d_idx < 1 || d_idx > static_cast<int>(sessions_list.size())) {
        state.chat_log.push({EntryKind::Error, "Invalid session number: " + d_arg, ""});
      } else {
        const auto& meta = sessions_list[d_idx - 1];
        bool was_current = (meta.id == state.agent_state.session_id());
        ctx.store->remove_session(meta.id);
        state.chat_log.push({EntryKind::SystemInfo, "Deleted session: " + (meta.title.empty() ? "(untitled)" : meta.title), ""});
        if (was_current) {
          ctx.session = agent::Session::create(ctx.io_ctx, ctx.config, agent::AgentType::Build, ctx.store);
          state.agent_state.set_session_id(ctx.session->id());
          setup_tui_callbacks(state, ctx);
          state.chat_log.push({EntryKind::SystemInfo, "Created new session", ""});
        }
      }
    }
  } else if (std::all_of(arg.begin(), arg.end(), ::isdigit)) {
    // 加载会话
    int s_idx = std::stoi(arg);
    if (s_idx < 1 || s_idx > static_cast<int>(sessions_list.size())) {
      state.chat_log.push({EntryKind::Error, "Invalid session number: " + arg, ""});
    } else {
      const auto& meta = sessions_list[s_idx - 1];
      ctx.session->cancel();
      auto resumed = agent::Session::resume(ctx.io_ctx, ctx.config, meta.id, ctx.store);
      if (resumed) {
        ctx.session = resumed;
        state.agent_state.set_session_id(ctx.session->id());
        setup_tui_callbacks(state, ctx);
        auto usage = ctx.session->total_usage();
        state.agent_state.update_tokens(usage.input_tokens, usage.output_tokens);
        state.agent_state.update_context(ctx.session->estimated_context_tokens(), ctx.session->context_window());
        state.clear_all();
        std::string title = meta.title.empty() ? "(untitled)" : meta.title;
        state.chat_log.push({EntryKind::SystemInfo, "Loaded session: " + title, ""});
        load_history_to_chat_log(state, ctx.session);
      } else {
        state.chat_log.push({EntryKind::Error, "Failed to load session", ""});
      }
    }
  } else {
    state.chat_log.push({EntryKind::Error, "Unknown sessions subcommand: " + arg, ""});
  }
}

// ============================================================
// 会话面板事件处理
// ============================================================

bool handle_sessions_panel_event(AppState& state, AppContext& ctx, Event event) {
  int count = static_cast<int>(state.sessions_cache.size());

  if (event == Event::Escape || event == Event::Character('q')) {
    state.show_sessions_panel = false;
    return true;
  }
  if (event == Event::ArrowUp || event == Event::Character('k')) {
    if (count > 0) state.sessions_selected = (state.sessions_selected - 1 + count) % count;
    return true;
  }
  if (event == Event::ArrowDown || event == Event::Character('j')) {
    if (count > 0) state.sessions_selected = (state.sessions_selected + 1) % count;
    return true;
  }

  if (event.is_mouse()) {
    auto& mouse = event.mouse();
    if (mouse.button == Mouse::WheelUp) {
      if (count > 0) state.sessions_selected = (state.sessions_selected - 1 + count) % count;
      return true;
    }
    if (mouse.button == Mouse::WheelDown) {
      if (count > 0) state.sessions_selected = (state.sessions_selected + 1) % count;
      return true;
    }
    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed && count > 0) {
      for (int bi = 0; bi < static_cast<int>(state.session_item_boxes.size()); ++bi) {
        if (state.session_item_boxes[bi].Contain(mouse.x, mouse.y)) {
          state.sessions_selected = bi;
          break;
        }
      }
      return true;
    }
    return true;
  }

  if (event == Event::Return) {
    if (count > 0 && state.sessions_selected < count) {
      const auto& meta = state.sessions_cache[state.sessions_selected];
      ctx.session->cancel();
      auto resumed = agent::Session::resume(ctx.io_ctx, ctx.config, meta.id, ctx.store);
      if (resumed) {
        ctx.session = resumed;
        state.agent_state.set_session_id(ctx.session->id());
        setup_tui_callbacks(state, ctx);
        auto usage = ctx.session->total_usage();
        state.agent_state.update_tokens(usage.input_tokens, usage.output_tokens);
        state.agent_state.update_context(ctx.session->estimated_context_tokens(), ctx.session->context_window());
        state.clear_all();
        std::string title = meta.title.empty() ? "(untitled)" : meta.title;
        state.chat_log.push({EntryKind::SystemInfo, "Loaded session: " + title, ""});
        load_history_to_chat_log(state, ctx.session);
      } else {
        state.chat_log.push({EntryKind::Error, "Failed to load session", ""});
      }
      state.show_sessions_panel = false;
    }
    return true;
  }

  if (event == Event::Character('d')) {
    if (count > 0 && state.sessions_selected < count) {
      const auto& meta = state.sessions_cache[state.sessions_selected];
      bool was_current = (meta.id == state.agent_state.session_id());
      ctx.store->remove_session(meta.id);
      state.chat_log.push({EntryKind::SystemInfo, "Deleted session: " + (meta.title.empty() ? "(untitled)" : meta.title), ""});
      if (was_current) {
        ctx.session = agent::Session::create(ctx.io_ctx, ctx.config, agent::AgentType::Build, ctx.store);
        state.agent_state.set_session_id(ctx.session->id());
        setup_tui_callbacks(state, ctx);
        state.chat_log.push({EntryKind::SystemInfo, "Created new session", ""});
      }
      state.sessions_cache = ctx.store->list_sessions();
      if (state.sessions_selected >= static_cast<int>(state.sessions_cache.size())) {
        state.sessions_selected = std::max(0, static_cast<int>(state.sessions_cache.size()) - 1);
      }
      if (state.sessions_cache.empty()) state.show_sessions_panel = false;
    }
    return true;
  }

  if (event == Event::Character('n')) {
    ctx.session = agent::Session::create(ctx.io_ctx, ctx.config, agent::AgentType::Build, ctx.store);
    state.agent_state.set_session_id(ctx.session->id());
    setup_tui_callbacks(state, ctx);
    state.agent_state.update_tokens(0, 0);
    state.clear_all();
    state.chat_log.push({EntryKind::SystemInfo, "New session created", ""});
    state.show_sessions_panel = false;
    return true;
  }

  return true;  // 拦截所有其他按键
}

// ============================================================
// Question 面板事件处理
// ============================================================

bool handle_question_panel_event(AppState& state, AppContext& ctx, Event event) {
  // Esc: 取消问答
  if (event == Event::Escape) {
    if (state.question_promise) {
      agent::QuestionResponse response;
      response.cancelled = true;
      state.question_promise->set_value(response);
    }
    state.reset_question_panel();
    state.agent_state.set_activity("Thinking...");
    ctx.refresh_fn();
    return true;
  }

  // Enter: 提交当前答案
  if (event == Event::Return) {
    // 保存当前答案
    if (state.question_current_index < static_cast<int>(state.question_answers.size())) {
      state.question_answers[state.question_current_index] = state.question_input_text;
    }

    // 移动到下一个问题
    state.question_current_index++;
    state.question_input_text.clear();

    // 检查是否所有问题都已回答
    if (state.question_current_index >= static_cast<int>(state.question_list.size())) {
      // 所有问题都已回答，提交结果
      if (state.question_promise) {
        agent::QuestionResponse response;
        response.answers = state.question_answers;
        response.cancelled = false;
        state.question_promise->set_value(response);
      }
      state.reset_question_panel();
      state.agent_state.set_activity("Thinking...");
    }
    ctx.refresh_fn();
    return true;
  }

  // Tab: 跳到下一个问题（不提交当前答案）
  if (event == Event::Tab) {
    // 保存当前输入
    if (state.question_current_index < static_cast<int>(state.question_answers.size())) {
      state.question_answers[state.question_current_index] = state.question_input_text;
    }
    // 循环切换问题
    state.question_current_index = (state.question_current_index + 1) % static_cast<int>(state.question_list.size());
    // 加载之前的答案（如果有）
    state.question_input_text = state.question_answers[state.question_current_index];
    ctx.refresh_fn();
    return true;
  }

  // 处理文本输入
  if (event.is_character()) {
    state.question_input_text += event.character();
    ctx.refresh_fn();
    return true;
  }

  // Backspace
  if (event == Event::Backspace && !state.question_input_text.empty()) {
    state.question_input_text.pop_back();
    ctx.refresh_fn();
    return true;
  }

  // ArrowUp/ArrowDown: 切换问题
  if (event == Event::ArrowUp) {
    if (state.question_current_index < static_cast<int>(state.question_answers.size())) {
      state.question_answers[state.question_current_index] = state.question_input_text;
    }
    state.question_current_index =
        (state.question_current_index - 1 + static_cast<int>(state.question_list.size())) % static_cast<int>(state.question_list.size());
    state.question_input_text = state.question_answers[state.question_current_index];
    ctx.refresh_fn();
    return true;
  }
  if (event == Event::ArrowDown) {
    if (state.question_current_index < static_cast<int>(state.question_answers.size())) {
      state.question_answers[state.question_current_index] = state.question_input_text;
    }
    state.question_current_index = (state.question_current_index + 1) % static_cast<int>(state.question_list.size());
    state.question_input_text = state.question_answers[state.question_current_index];
    ctx.refresh_fn();
    return true;
  }

  return true;  // 拦截所有其他按键
}

// ============================================================
// 主事件处理
// ============================================================

bool handle_main_event(AppState& state, AppContext& ctx, ScreenInteractive& screen, Event event) {
  // Question 面板专属事件（优先处理）
  if (state.show_question_panel) {
    return handle_question_panel_event(state, ctx, event);
  }

  // 会话列表面板专属事件
  if (state.show_sessions_panel) {
    return handle_sessions_panel_event(state, ctx, event);
  }

  // 非 Ctrl+C 重置
  if (event != Event::Special("\x03")) {
    state.ctrl_c_pending = false;
  }

  // Esc: 终止当前任务或关闭菜单
  if (event == Event::Escape) {
    if (state.agent_state.is_running()) {
      ctx.session->cancel();
      state.agent_state.set_running(false);
      state.chat_log.push({EntryKind::SystemInfo, "Interrupted", ""});
      return true;
    }
    if (state.show_cmd_menu) {
      state.show_cmd_menu = false;
      return true;
    }
    return true;
  }

  // Ctrl+C: 清空输入框 / 1秒内两次退出
  if (event == Event::Special("\x03")) {
    // 如果 agent 正在运行，先中断
    if (state.agent_state.is_running()) {
      ctx.session->cancel();
      state.agent_state.set_running(false);
      state.chat_log.push({EntryKind::SystemInfo, "Interrupted", ""});
      state.ctrl_c_pending = false;
      return true;
    }

    // 如果输入框非空，清空输入框并重置 ctrl_c 状态
    if (!state.input_text.empty()) {
      state.input_text.clear();
      state.input_cursor_pos = 0;
      state.show_cmd_menu = false;
      state.show_file_path_menu = false;
      state.file_path_matches.clear();
      state.ctrl_c_pending = false;
      return true;
    }

    // 输入框为空，检查是否 1 秒内再次按 Ctrl+C
    auto now = std::chrono::steady_clock::now();
    if (state.ctrl_c_pending && (now - state.ctrl_c_time) < std::chrono::seconds(1)) {
      screen.Exit();
      return true;
    }
    state.ctrl_c_pending = true;
    state.ctrl_c_time = now;
    state.chat_log.push({EntryKind::SystemInfo, "Press Ctrl+C again to exit", ""});
    return true;
  }

  // Enter
  if (event == Event::Return) {
    handle_submit(state, ctx, screen);
    return true;
  }

  // 命令菜单导航
  if (state.show_cmd_menu) {
    auto matches = match_commands(state.input_text);
    int count = static_cast<int>(matches.size());
    if (count > 0) {
      if (event == Event::ArrowUp) {
        state.cmd_menu_selected = (state.cmd_menu_selected - 1 + count) % count;
        return true;
      }
      if (event == Event::ArrowDown) {
        state.cmd_menu_selected = (state.cmd_menu_selected + 1) % count;
        return true;
      }
      if (event == Event::Tab) {
        if (state.cmd_menu_selected < count) {
          state.input_text = matches[state.cmd_menu_selected].name;
          state.input_cursor_pos = static_cast<int>(state.input_text.size());
          state.show_cmd_menu = false;
        }
        return true;
      }
    }
  }

  // 文件路径菜单导航
  if (state.show_file_path_menu && !state.show_cmd_menu) {  // 只在命令菜单未显示时处理
    int count = static_cast<int>(state.file_path_matches.size());
    if (count > 0) {
      if (event == Event::ArrowUp) {
        state.file_path_menu_selected = (state.file_path_menu_selected - 1 + count) % count;
        return true;
      }
      if (event == Event::ArrowDown) {
        state.file_path_menu_selected = (state.file_path_menu_selected + 1) % count;
        return true;
      }
      if (event == Event::Tab || event == Event::Return) {
        if (state.file_path_menu_selected < count) {
          // 获取当前输入文本直到最后的 @ 符号
          size_t at_pos = state.input_text.rfind('@');
          if (at_pos != std::string::npos) {
            std::string before_at = state.input_text.substr(0, at_pos + 1);
            std::string path_to_insert = state.file_path_matches[state.file_path_menu_selected].path;

            // 如果是目录，添加斜杠
            if (state.file_path_matches[state.file_path_menu_selected].is_directory) {
              path_to_insert += "/";
            }

            // 添加一个空格到路径后面
            path_to_insert += " ";

            state.input_text = before_at + path_to_insert;
            state.input_cursor_pos = static_cast<int>(state.input_text.size());
            state.show_file_path_menu = false;
            state.file_path_matches.clear();
          }
        }
        return true;
      }
      if (event == Event::Escape) {
        state.show_file_path_menu = false;
        state.file_path_matches.clear();
        return true;
      }
    }
  }

  // 向上箭头：浏览历史记录（显示更早的记录）
  if (event == Event::ArrowUp && !state.show_cmd_menu && !state.show_file_path_menu) {
    if (state.input_history.empty()) {
      return true;  // 没有历史记录，直接返回
    }

    // 如果当前是新输入（不在历史记录中），但不是空输入，则保存当前输入
    if (state.history_index == -1 && !state.input_text.empty()) {
      if (state.input_history.empty() || state.input_history.back() != state.input_text) {
        state.input_history.push_back(state.input_text);
      }
      // 立即移动到刚添加的记录，然后跳到前一条
      state.history_index = 0;  // 指向最新添加的记录
      // 如果有至少2条记录，显示前一条
      if (state.input_history.size() > 1) {
        int array_index = static_cast<int>(state.input_history.size()) - 2;  // 倒数第二条
        state.input_text = state.input_history[array_index];
        state.input_cursor_pos = static_cast<int>(state.input_text.size());
        state.history_index = 1;  // 指向倒数第二条
      }
    } else if (state.history_index < static_cast<int>(state.input_history.size()) - 1) {
      // 移动到上一条历史记录（显示更早的记录）
      state.history_index++;
      // 显示对应的历史记录（history_index=0对应最新，即数组索引size()-1）
      int array_index = static_cast<int>(state.input_history.size()) - 1 - state.history_index;
      state.input_text = state.input_history[array_index];
      state.input_cursor_pos = static_cast<int>(state.input_text.size());
    }
    return true;
  }

  // 向下箭头：浏览历史记录（显示更新的记录或回到当前输入）
  if (event == Event::ArrowDown && !state.show_cmd_menu && !state.show_file_path_menu) {
    // 如果已经在当前输入，直接返回
    if (state.history_index <= -1) {
      return true;
    }

    // 移动到下一条历史记录或回到当前输入
    state.history_index--;
    if (state.history_index < 0) {
      // 回到当前输入，不改变 input_text，保持用户可能已编辑的内容
      state.history_index = -1;
    } else {
      // 显示更新的历史记录（较小的history_index对应较新的记录）
      int array_index = static_cast<int>(state.input_history.size()) - 1 - state.history_index;
      state.input_text = state.input_history[array_index];
      state.input_cursor_pos = static_cast<int>(state.input_text.size());
    }
    return true;
  }

  // Tab: 切换模式
  if (event == Event::Tab && !state.show_cmd_menu && !state.show_file_path_menu) {
    state.agent_state.toggle_mode();
    return true;
  }

  // PageUp / PageDown
  if (event == Event::PageUp) {
    state.scroll_y = std::max(0.0f, state.scroll_y - 0.3f);
    state.auto_scroll = false;
    return true;
  }
  if (event == Event::PageDown) {
    state.scroll_y = std::min(1.0f, state.scroll_y + 0.3f);
    if (state.scroll_y >= 0.95f) {
      state.scroll_y = 1.0f;
      state.auto_scroll = true;
    }
    return true;
  }

  // 鼠标滚轮
  if (event.is_mouse()) {
    auto& mouse = event.mouse();

    // 处理工具框点击（切换展开/折叠）
    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
      // 检查是否点击了某个工具框
      for (size_t box_idx = 0; box_idx < state.tool_boxes.size(); ++box_idx) {
        if (state.tool_boxes[box_idx].Contain(mouse.x, mouse.y)) {
          // 找到对应的 entry 索引
          if (box_idx < state.tool_entry_indices.size()) {
            size_t entry_idx = state.tool_entry_indices[box_idx];
            // 切换展开状态
            state.tool_expanded[entry_idx] = !state.tool_expanded[entry_idx];
            return true;
          }
        }
      }
      return false;  // 点击了其他地方，让默认行为处理
    }

    if (mouse.button == Mouse::WheelUp) {
      state.scroll_y = std::max(0.0f, state.scroll_y - 0.05f);
      state.auto_scroll = false;
      return true;
    }
    if (mouse.button == Mouse::WheelDown) {
      state.scroll_y = std::min(1.0f, state.scroll_y + 0.05f);
      if (state.scroll_y >= 0.95f) {
        state.scroll_y = 1.0f;
        state.auto_scroll = true;
      }
      return true;
    }
    return true;  // 拦截所有鼠标事件
  }

  return false;
}

}  // namespace agent_cli
