#pragma once

// tui_render.h — FTXUI 渲染函数
// 聊天条目、工具卡片、状态栏、命令菜单、会话面板等 DOM 元素构建

#include <ftxui/dom/elements.hpp>

#include "tui_components.h"
#include "tui_state.h"

namespace agent_cli {

// 工具调用分组（ToolCall + 对应的 ToolResult）
struct ToolGroup {
  ChatEntry call;
  ChatEntry result;
  bool has_result = false;
};

// 渲染单条文本类聊天条目
ftxui::Element render_text_entry(const ChatEntry& entry);

// 渲染工具调用卡片（折叠/展开）
ftxui::Element render_tool_group(const ToolGroup& group, bool expanded);

// 构建聊天视图（含滚动逻辑）
ftxui::Element build_chat_view(AppState& state);

// 构建状态栏
ftxui::Element build_status_bar(const AppState& state);

// 构建命令提示菜单
ftxui::Element build_cmd_menu(const AppState& state);

// 构建文件路径提示菜单
ftxui::Element build_file_path_menu(const AppState& state);

// 构建会话列表面板
ftxui::Element build_sessions_panel(AppState& state);

// 构建 Question 面板（用于 question 工具交互）
ftxui::Element build_question_panel(AppState& state);

// 构建登录面板（用于 Qwen OAuth 认证）
ftxui::Element build_login_panel(const AppState& state);

}  // namespace agent_cli
