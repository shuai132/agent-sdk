#pragma once

// tui_state.h — TUI 应用的全局状态和上下文
// 包含所有 UI 状态变量，由 main 持有，各模块通过引用访问

#include <asio.hpp>
#include <chrono>
#include <ftxui/screen/box.hpp>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "tui_components.h"

#ifdef AGENT_PLUGIN_QWEN
#include "plugin/qwen/qwen_oauth.hpp"
#endif

namespace agent_cli {

// 登录状态枚举
enum class LoginState {
  NotRequired,  // 不需要登录（已有 token 或使用其他认证方式）
  NeedLogin,    // 需要登录
  WaitingAuth,  // 等待用户扫码授权
  Success,      // 登录成功
  Failed,       // 登录失败
};

// TUI 应用的全部可变状态，集中管理
struct AppState {
  // ----- 核心组件 -----
  ChatLog chat_log;
  ToolPanel tool_panel;
  AgentState agent_state;

  // ----- 输入 -----
  std::string input_text;
  int input_cursor_pos = 0;
  std::vector<std::string> input_history;  // 输入历史记录
  int history_index = -1;                  // 当前浏览的历史记录索引 (-1 表示当前输入)

  // ----- 命令菜单 -----
  int cmd_menu_selected = 0;
  bool show_cmd_menu = false;

  // ----- 文件路径自动完成菜单 -----
  int file_path_menu_selected = 0;
  bool show_file_path_menu = false;
  std::vector<FilePathMatch> file_path_matches;

  // ----- 滚动控制 -----
  float scroll_y = 1.0f;          // 0.0=顶部, 1.0=底部
  bool auto_scroll = true;        // 新消息自动滚到底，用户上滚后暂停
  size_t last_snapshot_size = 0;  // 检测内容变化以触发自动滚动

  // ----- Ctrl+C 两次退出 -----
  bool ctrl_c_pending = false;
  std::chrono::steady_clock::time_point ctrl_c_time;

  // ----- 工具调用展开状态 -----
  std::map<size_t, bool> tool_expanded;    // key = ToolCall 在 snapshot 中的 index
  std::vector<ftxui::Box> tool_boxes;      // 工具框的屏幕坐标（用于鼠标点击检测）
  std::vector<size_t> tool_entry_indices;  // 工具框对应的 entry 索引（tool_boxes[i] 对应 entries[tool_entry_indices[i]]）

  // ----- 会话列表面板 -----
  bool show_sessions_panel = false;
  int sessions_selected = 0;
  std::vector<agent::SessionMeta> sessions_cache;
  std::vector<ftxui::Box> session_item_boxes;

  // ----- Question 面板（用于 question 工具交互） -----
  bool show_question_panel = false;
  std::vector<std::string> question_list;                                   // 当前要问的问题列表
  std::vector<std::string> question_answers;                                // 用户输入的答案
  int question_current_index = 0;                                           // 当前正在回答的问题索引
  std::string question_input_text;                                          // 当前答案输入框的文字
  std::shared_ptr<std::promise<agent::QuestionResponse>> question_promise;  // 用于返回答案的 promise

  // ----- 登录面板（Qwen OAuth）-----
  LoginState login_state = LoginState::NotRequired;
  std::string login_qr_code;     // QR 码字符串（已渲染为 Unicode）
  std::string login_auth_url;    // 授权链接
  std::string login_user_code;   // 用户码
  std::string login_status_msg;  // 状态消息
  std::string login_error_msg;   // 错误消息

  // ----- 便捷方法 -----
  void reset_view();
  void clear_all();
  void reset_question_panel();  // 重置 question 面板状态

  // ----- 历史记录持久化 -----
  void save_history_to_file(const std::filesystem::path& filepath);
  void load_history_from_file(const std::filesystem::path& filepath);
};

// TUI 应用的外部依赖/上下文（生命周期由 main 管理）
struct AppContext {
  asio::io_context& io_ctx;
  agent::Config& config;
  std::shared_ptr<agent::JsonMessageStore> store;
  std::shared_ptr<agent::Session> session;
  std::function<void()> refresh_fn;
};

}  // namespace agent_cli
