#include "tui_render.h"

#include <filesystem>
#include <ftxui/screen/color.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace agent_cli {

using json = nlohmann::json;

using namespace ftxui;

// ============================================================
// 聊天条目渲染
// ============================================================

Element render_text_entry(const ChatEntry& entry) {
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

    case EntryKind::Thinking: {
      auto lines = split_lines(entry.text);
      Elements content;
      for (const auto& line : lines) {
        content.push_back(paragraph(line));
      }
      return vbox({
          hbox({text("  💭 ") | dim, text("Thinking") | dim | italic}),
          hbox({text("    "), vbox(content) | dim}) | flex,
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
// 工具调用卡片渲染
// ============================================================

// 解析 JSON 参数为 key: value 格式的行
static std::vector<std::pair<std::string, std::string>> parse_args_to_kv(const std::string& args_json) {
  std::vector<std::pair<std::string, std::string>> result;
  try {
    auto j = json::parse(args_json);
    if (j.is_object()) {
      for (auto it = j.begin(); it != j.end(); ++it) {
        std::string value;
        if (it.value().is_string()) {
          value = it.value().get<std::string>();
        } else {
          value = it.value().dump();
        }
        result.emplace_back(it.key(), value);
      }
    }
  } catch (...) {
    // JSON 解析失败，返回空
  }
  return result;
}

// Extract activity from detail string (stored as __ACTIVITY__:xxx)
static std::string extract_activity(const std::string& detail) {
  auto pos = detail.find("\n__ACTIVITY__:");
  if (pos != std::string::npos) {
    return detail.substr(pos + 14);  // Skip "\n__ACTIVITY__:"
  }
  return "";
}

// Extract clean detail without activity suffix
static std::string extract_clean_detail(const std::string& detail) {
  auto pos = detail.find("\n__ACTIVITY__:");
  if (pos != std::string::npos) {
    return detail.substr(0, pos);
  }
  return detail;
}

// Render a nested subagent entry
static Element render_nested_entry(const ChatEntry& entry) {
  switch (entry.kind) {
    case EntryKind::ToolCall:
      return hbox({
          text("   ◦ ") | color(Color::Cyan),
          text(entry.text) | bold,
          text(" ") | dim,
      });
    case EntryKind::ToolResult: {
      bool is_error = entry.text.find("✗") != std::string::npos;
      return hbox({
          text("     ") | dim,
          text(is_error ? "✗" : "✓") | color(is_error ? Color::Red : Color::Green),
          text(" done") | dim,
      });
    }
    case EntryKind::Thinking:
      return hbox({
          text("   💭 ") | dim,
          text(truncate_text(entry.text, 60)) | dim | italic,
      });
    case EntryKind::AssistantText:
      // Don't show stream text in collapsed view
      return text("");
    case EntryKind::Error:
      return hbox({
          text("   ✗ ") | color(Color::Red),
          text(truncate_text(entry.text, 80)) | color(Color::Red),
      });
    default:
      return text("");
  }
}

Element render_tool_group(const ToolGroup& group, bool expanded) {
  bool is_error = group.has_result && group.result.text.find("✗") != std::string::npos;
  bool is_running = !group.has_result;

  std::string status_icon = is_running ? "⏳" : (is_error ? "✗" : "✓");
  Color status_color = is_running ? Color::Yellow : (is_error ? Color::Red : Color::Green);

  // Extract activity from detail (for subagent progress)
  std::string activity = extract_activity(group.call.detail);
  std::string clean_detail = extract_clean_detail(group.call.detail);

  // 解析参数为 key-value 格式
  auto args_kv = parse_args_to_kv(clean_detail);

  // 构造卡片头部状态文本
  std::string status_text;
  if (is_running) {
    if (!activity.empty()) {
      status_text = activity;  // Show subagent activity instead of "running..."
    } else {
      status_text = "running...";
    }
  } else if (is_error) {
    status_text = "error";
  } else {
    status_text = "ok";
  }

  // 卡片头部行
  auto header_line = hbox({
      text(" " + status_icon + "  ") | color(status_color),
      text(group.call.text) | bold,
      text("  " + status_text) | dim,
  });

  // 构建卡片内容
  Elements card_content;
  card_content.push_back(header_line);

  // 始终显示参数（key: value 格式）
  for (const auto& [key, value] : args_kv) {
    // 处理多行值
    auto value_lines = split_lines(value);
    if (value_lines.size() <= 1) {
      card_content.push_back(hbox({text(" " + key + ": ") | dim, text(truncate_text(value, 100))}));
    } else {
      card_content.push_back(hbox({text(" " + key + ": ") | dim, text(truncate_text(value_lines[0], 80) + " ...")}));
    }
  }

  if (!expanded) {
    return hbox({text(" "), vbox(card_content) | borderRounded});
  }

  // 展开模式：显示完整参数和结果
  card_content.clear();
  card_content.push_back(header_line);
  card_content.push_back(text(""));

  // 完整参数区域
  for (const auto& [key, value] : args_kv) {
    auto value_lines = split_lines(value);
    if (value_lines.size() <= 1) {
      card_content.push_back(text("   " + key + ": " + value) | dim);
    } else {
      card_content.push_back(text("   " + key + ":") | dim);
      for (size_t i = 0; i < value_lines.size() && i < 20; ++i) {
        card_content.push_back(text("     " + value_lines[i]) | dim);
      }
      if (value_lines.size() > 20) {
        card_content.push_back(text("     ...(" + std::to_string(value_lines.size()) + " lines)") | dim);
      }
    }
  }

  // Subagent nested entries (for task tool)
  if (!group.call.nested_entries.empty()) {
    card_content.push_back(text(""));
    card_content.push_back(text("   Subagent execution:") | bold | dim | color(Color::Magenta));

    // Group stream text together, render tool calls separately
    std::string accumulated_stream;
    for (const auto& nested : group.call.nested_entries) {
      if (nested.kind == EntryKind::AssistantText) {
        accumulated_stream += nested.text;
      } else {
        // Flush accumulated stream if any
        if (!accumulated_stream.empty()) {
          auto stream_lines = split_lines(accumulated_stream);
          for (size_t si = 0; si < stream_lines.size() && si < 10; ++si) {
            card_content.push_back(text("   " + stream_lines[si]) | dim);
          }
          if (stream_lines.size() > 10) {
            card_content.push_back(text("   ...(" + std::to_string(stream_lines.size()) + " lines)") | dim);
          }
          accumulated_stream.clear();
        }
        // Render the non-stream entry
        card_content.push_back(render_nested_entry(nested));
      }
    }
    // Flush remaining stream
    if (!accumulated_stream.empty()) {
      auto stream_lines = split_lines(accumulated_stream);
      for (size_t si = 0; si < stream_lines.size() && si < 10; ++si) {
        card_content.push_back(text("   " + stream_lines[si]) | dim);
      }
      if (stream_lines.size() > 10) {
        card_content.push_back(text("   ...(" + std::to_string(stream_lines.size()) + " lines)") | dim);
      }
    }
  }

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

  return hbox({text(" "), vbox(card_content) | borderRounded});
}

// ============================================================
// 聊天视图构建
// ============================================================

Element build_chat_view(AppState& state) {
  auto entries = state.chat_log.snapshot();

  // 检测内容变化，自动滚动到底部
  size_t current_size = entries.size();
  bool content_changed = (current_size != state.last_snapshot_size);
  if (!content_changed && !entries.empty() && entries.back().kind == EntryKind::AssistantText) {
    content_changed = true;  // 流式追加
  }
  state.last_snapshot_size = current_size;

  if (state.auto_scroll && content_changed) {
    state.scroll_y = 1.0f;
  }

  // 先统计有多少个 ToolCall，预分配边界框
  size_t tool_count = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].kind == EntryKind::ToolCall) {
      tool_count++;
    }
  }
  state.tool_boxes.clear();
  state.tool_boxes.resize(tool_count);
  state.tool_entry_indices.clear();
  state.tool_entry_indices.reserve(tool_count);

  Elements chat_elements;
  chat_elements.push_back(text(""));

  size_t tool_box_idx = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];

    if (e.kind == EntryKind::ToolCall) {
      ToolGroup group;
      group.call = e;
      // Search forward for matching ToolResult (may not be immediately next due to Thinking entries)
      for (size_t j = i + 1; j < entries.size() && j < i + 5; ++j) {
        if (entries[j].kind == EntryKind::ToolResult) {
          // Check if tool names match (ToolResult.text contains tool name)
          // ToolCall.text is the tool name, ToolResult.text is "tool_name ✓" or "tool_name ✗"
          if (entries[j].text.find(e.text) == 0) {
            group.result = entries[j];
            group.has_result = true;
            break;
          }
        }
        // Stop searching if we hit another ToolCall (means no result for this one yet)
        if (entries[j].kind == EntryKind::ToolCall) break;
      }
      bool expanded = state.tool_expanded.count(i) && state.tool_expanded[i];

      // 使用 reflect 捕获边界框，同时存储原始索引
      if (tool_box_idx < state.tool_boxes.size()) {
        state.tool_entry_indices.push_back(i);  // 记录此工具框对应的 entry 索引
        chat_elements.push_back(render_tool_group(group, expanded) | reflect(state.tool_boxes[tool_box_idx]));
        tool_box_idx++;
      } else {
        chat_elements.push_back(render_tool_group(group, expanded));
      }
      continue;
    }

    if (e.kind == EntryKind::ToolResult) {
      // Check if this ToolResult was already paired with a ToolCall (search backward)
      bool already_paired = false;
      for (size_t j = (i > 5 ? i - 5 : 0); j < i; ++j) {
        if (entries[j].kind == EntryKind::ToolCall && e.text.find(entries[j].text) == 0) {
          already_paired = true;
          break;
        }
      }
      if (already_paired) continue;  // 已配对的 ToolResult
    }

    chat_elements.push_back(render_text_entry(e));
  }

  // 活动状态文字
  if (state.agent_state.is_running()) {
    auto activity = state.agent_state.activity();
    if (activity.empty()) activity = "Thinking...";
    chat_elements.push_back(hbox({text("    "), text(activity) | dim | color(Color::Cyan)}));
  }

  chat_elements.push_back(text(""));

  return vbox(chat_elements)                           //
         | focusPositionRelative(0.f, state.scroll_y)  //
         | vscroll_indicator                           //
         | yframe                                      //
         | flex;
}

// ============================================================
// 状态栏
// ============================================================

Element build_status_bar(const AppState& state) {
  // 计算 context 使用占比
  float ratio = state.agent_state.context_ratio();
  int percent = static_cast<int>(ratio * 100);
  std::string context_str = std::to_string(percent) + "%";

  // 根据占比选择颜色：<50% 绿色，50-80% 黄色，>80% 红色
  Color context_color = Color::Green;
  if (ratio >= 0.8f) {
    context_color = Color::Red;
  } else if (ratio >= 0.5f) {
    context_color = Color::Yellow;
  }

  return hbox({
      text(" " + std::filesystem::current_path().filename().string() + " ") | bold | color(Color::White) | bgcolor(Color::Blue),
      text(" "),
      text(state.agent_state.model()) | dim,
      filler(),
      text(format_tokens(state.agent_state.input_tokens()) + "↑ " + format_tokens(state.agent_state.output_tokens()) + "↓") | dim,
      text("  "),
      text("ctx:" + context_str) | color(context_color),
      text(" "),
      text(state.agent_state.is_running() ? " ● Running " : " ● Ready ") | color(Color::White) |
          bgcolor(state.agent_state.is_running() ? Color::Yellow : Color::Green),
  });
}

// ============================================================
// 命令提示菜单
// ============================================================

Element build_cmd_menu(const AppState& state) {
  if (!state.show_cmd_menu || state.input_text.empty()) return text("");

  auto matches = match_commands(state.input_text);
  if (matches.empty()) return text("");

  Elements menu_items;
  for (int j = 0; j < static_cast<int>(matches.size()); ++j) {
    auto& def = matches[j];
    bool selected = (j == state.cmd_menu_selected);
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
  return vbox(menu_items) | borderRounded | color(Color::GrayLight);
}

// ============================================================
// 文件路径菜单
// ============================================================

Element build_file_path_menu(const AppState& state) {
  if (!state.show_file_path_menu || state.file_path_matches.empty()) return text("");

  // 限制最大显示项目数，防止菜单过长
  const int MAX_VISIBLE_ITEMS = 10;
  int total_items = static_cast<int>(state.file_path_matches.size());
  int start_idx = 0;
  int end_idx = total_items;

  // 计算可视范围，确保选中的项目在可视区域内
  if (total_items > MAX_VISIBLE_ITEMS) {
    // 选中的项目应该在中间附近，但不能超过边界
    int half_visible = MAX_VISIBLE_ITEMS / 2;
    start_idx = state.file_path_menu_selected - half_visible;
    start_idx = std::max(0, std::min(start_idx, total_items - MAX_VISIBLE_ITEMS));
    end_idx = std::min(start_idx + MAX_VISIBLE_ITEMS, total_items);
  }

  Elements menu_items;
  for (int j = start_idx; j < end_idx; ++j) {
    const auto& match = state.file_path_matches[j];
    bool selected = (j == state.file_path_menu_selected);
    auto item = hbox({
        text("  "),
        text(match.display) | (match.is_directory ? color(Color::Blue) : color(Color::White)),
        text("  "),
    });
    if (selected) {
      item = item | bgcolor(Color::GrayDark) | color(Color::White);
    }
    menu_items.push_back(item);
  }

  auto menu = vbox(menu_items);

  // 如果超过最大显示数量，添加滚动指示
  if (total_items > MAX_VISIBLE_ITEMS) {
    std::string indicator = "(" + std::to_string(start_idx + 1) + "-" + std::to_string(end_idx) + "/" + std::to_string(total_items) + ")";
    menu = vbox({menu, hbox({text("  "), text(indicator) | dim, filler()})});
  }

  return menu | borderRounded | color(Color::GrayLight) | yflex;
}

// ============================================================
// 会话列表面板
// ============================================================

Element build_sessions_panel(AppState& state) {
  Elements session_items;
  if (state.sessions_cache.empty()) {
    session_items.push_back(text("  No saved sessions") | dim);
  } else {
    for (int si = 0; si < static_cast<int>(state.sessions_cache.size()); ++si) {
      const auto& meta = state.sessions_cache[si];
      bool is_current = (meta.id == state.agent_state.session_id());
      bool is_selected = (si == state.sessions_selected);
      std::string title = meta.title.empty() ? "(untitled)" : meta.title;
      std::string marker = is_current ? " ●" : "  ";
      std::string detail =
          format_time(meta.updated_at) + "  " + agent::to_string(meta.agent_type) + "  tokens: " + format_tokens(meta.total_usage.total());

      auto row = vbox({
          hbox({
              text(marker) | color(Color::Green),
              text(" " + std::to_string(si + 1) + ". ") | dim,
              text(title) | bold,
          }),
          hbox({text("      "), text(detail) | dim}),
      });

      if (is_selected) {
        row = row | bgcolor(Color::GrayDark) | color(Color::White);
      }
      session_items.push_back(row);
      session_items.push_back(text(""));
    }
  }

  // reflect 捕获屏幕坐标
  state.session_item_boxes.resize(state.sessions_cache.size());
  Elements reflected_items;
  for (size_t ri = 0; ri + 1 < session_items.size(); ri += 2) {
    int idx = static_cast<int>(ri / 2);
    if (idx < static_cast<int>(state.session_item_boxes.size())) {
      auto item = vbox({session_items[ri], session_items[ri + 1]}) | reflect(state.session_item_boxes[idx]);
      if (idx == state.sessions_selected) {
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

  return vbox({panel_header, separator() | dim, session_list | flex});
}

// ============================================================
// Question 面板
// ============================================================

Element build_question_panel(AppState& state) {
  Elements question_items;

  if (state.question_list.empty()) {
    question_items.push_back(text("  No questions") | dim);
  } else {
    for (int qi = 0; qi < static_cast<int>(state.question_list.size()); ++qi) {
      bool is_current = (qi == state.question_current_index);
      bool is_answered = !state.question_answers[qi].empty();

      // 问题标题
      std::string q_prefix = is_current ? " ▶ " : "   ";
      std::string q_status = is_answered ? " ✓" : "";

      auto q_line = hbox({
          text(q_prefix) | color(is_current ? Color::Cyan : Color::White),
          text("Q" + std::to_string(qi + 1) + ": ") | bold | color(Color::Yellow),
          paragraph(state.question_list[qi]),
          text(q_status) | color(Color::Green),
      });

      question_items.push_back(q_line);

      // 显示答案（如果有）或输入框（如果是当前问题）
      if (is_current) {
        // 当前问题的输入框
        std::string input_display = state.question_input_text;
        if (input_display.empty()) {
          input_display = "Type your answer here...";
        }
        auto input_line = hbox({
            text("      A: ") | dim, text(input_display) | (state.question_input_text.empty() ? dim : nothing) | underlined,
            text("▌") | blink | color(Color::Cyan),  // 光标
        });
        question_items.push_back(input_line);
      } else if (is_answered) {
        // 已回答的问题显示答案
        auto answer_line = hbox({
            text("      A: ") | dim,
            text(state.question_answers[qi]) | color(Color::GrayLight),
        });
        question_items.push_back(answer_line);
      }

      question_items.push_back(text(""));
    }
  }

  auto question_list = vbox(question_items)  //
                       | vscroll_indicator   //
                       | yframe              //
                       | flex;

  auto panel_header = hbox({
      text(" ❓ AI Questions ") | bold | color(Color::Yellow),
      filler(),
      text(" ↑↓ switch  Enter next/submit  Tab skip  Esc cancel ") | dim,
  });

  auto progress_text =
      text(" Question " + std::to_string(state.question_current_index + 1) + "/" + std::to_string(state.question_list.size()) + " ") |
      color(Color::Cyan);

  return vbox({
      panel_header,
      separator() | dim,
      question_list | flex,
      separator() | dim,
      hbox({progress_text, filler()}),
  });
}

// ============================================================
// 登录面板（Qwen OAuth）
// ============================================================

Element build_login_panel(const AppState& state) {
  Elements content;

  // 标题
  auto header = hbox({
      text(" 🔐 Qwen OAuth 登录 ") | bold | color(Color::Cyan),
      filler(),
  });
  content.push_back(header);
  content.push_back(separator() | dim);
  content.push_back(text(""));

  switch (state.login_state) {
    case LoginState::NeedLogin:
      content.push_back(text("  正在初始化认证...") | color(Color::Yellow));
      break;

    case LoginState::WaitingAuth: {
      // QR 码显示
      if (!state.login_qr_code.empty()) {
        auto qr_lines = split_lines(state.login_qr_code);
        content.push_back(text("  请扫描二维码或访问以下链接进行授权：") | dim);
        content.push_back(text(""));

        // 渲染 QR 码
        for (const auto& line : qr_lines) {
          content.push_back(text("  " + line));
        }
        content.push_back(text(""));
      }

      // 授权链接和验证码
      if (!state.login_auth_url.empty()) {
        content.push_back(hbox({text("  链接: ") | dim, text(state.login_auth_url) | color(Color::Blue) | underlined}));
      }
      if (!state.login_user_code.empty()) {
        content.push_back(hbox({text("  验证码: ") | dim, text(state.login_user_code) | bold | color(Color::Green)}));
      }
      content.push_back(text(""));

      // 状态消息
      if (!state.login_status_msg.empty()) {
        content.push_back(text("  " + state.login_status_msg) | color(Color::Yellow));
      } else {
        content.push_back(text("  等待授权中...") | color(Color::Yellow) | blink);
      }
      break;
    }

    case LoginState::Success:
      content.push_back(text("  ✓ 登录成功！") | bold | color(Color::Green));
      content.push_back(text(""));
      content.push_back(text("  正在进入应用...") | dim);
      break;

    case LoginState::Failed:
      content.push_back(text("  ✗ 登录失败") | bold | color(Color::Red));
      if (!state.login_error_msg.empty()) {
        content.push_back(text(""));
        content.push_back(text("  错误: " + state.login_error_msg) | color(Color::Red));
      }
      content.push_back(text(""));
      content.push_back(text("  按 Enter 重试，按 Esc 退出") | dim);
      break;

    default:
      break;
  }

  content.push_back(text(""));
  content.push_back(filler());

  return vbox(content) | flex | borderRounded;
}

}  // namespace agent_cli
