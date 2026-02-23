#pragma once

// tui_components.h — agent_cli 的可测试核心组件
// ChatLog、ToolPanel、命令解析等逻辑
// 独立于 FTXUI 渲染层，可以单独进行单元测试

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace agent_cli {

// ============================================================
// 聊天消息条目
// ============================================================

enum class EntryKind {
  UserMsg,
  AssistantText,
  Thinking,  // AI 思考过程（reasoning_content）
  ToolCall,
  ToolResult,
  SubtaskStart,
  SubtaskEnd,
  Error,
  SystemInfo,
};

std::string to_string(EntryKind kind);

struct ChatEntry {
  EntryKind kind;
  std::string text;    // 主要文本内容
  std::string detail;  // 额外详情 (如参数/结果)
  std::string tool_call_id;  // Tool call ID for matching subagent events
  std::vector<ChatEntry> nested_entries;  // Nested entries for subagent progress
};

// ============================================================
// 线程安全的聊天日志
// ============================================================

class ChatLog {
 public:
  void push(ChatEntry entry);
  void append_stream(const std::string& delta);
  std::vector<ChatEntry> snapshot() const;
  size_t size() const;
  void clear();
  ChatEntry last() const;
  std::vector<ChatEntry> filter(EntryKind kind) const;

  // Add nested entry to a tool call (for subagent progress)
  void add_nested_entry(const std::string& tool_call_id, ChatEntry nested_entry);

  // Update activity status for a tool call (for subagent progress)
  void update_tool_activity(const std::string& tool_call_id, const std::string& activity);

 private:
  mutable std::mutex mu_;
  std::vector<ChatEntry> entries_;
};

// ============================================================
// 工具活动记录
// ============================================================

struct ToolActivity {
  std::string tool_name;
  std::string status;  // "running", "done", "error"
  std::string summary;
};

class ToolPanel {
 public:
  void start_tool(const std::string& name, const std::string& args_summary);
  void finish_tool(const std::string& name, const std::string& result_summary, bool is_error);
  std::vector<ToolActivity> snapshot() const;
  size_t size() const;
  std::string tool_status(const std::string& name) const;
  void clear();

 private:
  mutable std::mutex mu_;
  std::vector<ToolActivity> activities_;
};

// ============================================================
// 命令解析
// ============================================================

enum class CommandType {
  None,      // 不是命令，是普通消息
  Quit,      // /q, /quit
  Clear,     // /clear
  Help,      // /h, /help
  Sessions,  // /s, /sessions
  Compact,   // /compact
  Expand,    // /expand — 展开所有工具调用
  Collapse,  // /collapse — 折叠所有工具调用
  Copy,      // /copy — 复制聊天内容到剪贴板
  Unknown,   // 无法识别的 / 命令
};

struct CommandDef {
  std::string name;
  std::string shortcut;
  std::string description;
  CommandType type;
};

const std::vector<CommandDef>& command_defs();
std::vector<CommandDef> match_commands(const std::string& prefix);

struct ParsedCommand {
  CommandType type = CommandType::None;
  std::string arg;
};

ParsedCommand parse_command(const std::string& input);

// ============================================================
// 文件路径自动完成
// ============================================================

struct FilePathMatch {
  std::string path;     // 相对路径（相对于当前工作目录）
  std::string display;  // 显示名称（文件名或目录名）
  bool is_directory;    // 是否为目录
};

std::vector<FilePathMatch> match_file_paths(const std::string& prefix);
std::string extract_file_path_prefix(const std::string& input);

// ============================================================
// 文本工具函数
// ============================================================

std::string truncate_text(const std::string& s, size_t max_len);
std::vector<std::string> split_lines(const std::string& text);
std::string format_time(const std::chrono::system_clock::time_point& ts);
std::string format_tokens(int64_t tokens);

// ============================================================
// Agent 模式
// ============================================================

enum class AgentMode {
  Build,
  Plan,
};

std::string to_string(AgentMode mode);

// ============================================================
// Agent 状态管理
// ============================================================

class AgentState {
 public:
  void set_running(bool running);
  bool is_running() const;

  void set_model(const std::string& model);
  std::string model() const;

  void set_session_id(const std::string& id);
  std::string session_id() const;

  void update_tokens(int64_t input, int64_t output);
  int64_t input_tokens() const;
  int64_t output_tokens() const;

  void update_context(int64_t used, int64_t limit);
  int64_t context_used() const;
  int64_t context_limit() const;
  float context_ratio() const;  // 返回 0.0~1.0 之间的占比

  void set_activity(const std::string& msg);
  std::string activity() const;

  void set_mode(AgentMode mode);
  AgentMode mode() const;
  void toggle_mode();

  std::string status_text() const;

 private:
  std::atomic<bool> running_{false};
  std::atomic<int64_t> input_tokens_{0};
  std::atomic<int64_t> output_tokens_{0};
  std::atomic<int64_t> context_used_{0};
  std::atomic<int64_t> context_limit_{128000};  // 默认 128k
  std::atomic<int> mode_{static_cast<int>(AgentMode::Build)};
  mutable std::mutex mu_;
  std::string model_;
  std::string session_id_;
  std::string activity_;
};

}  // namespace agent_cli
