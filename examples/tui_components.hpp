#pragma once

// tui_components.hpp — agent_cli 的可测试核心组件
// ChatLog、ToolPanel、命令解析等逻辑
// 独立于 FTXUI 渲染层，可以单独进行单元测试

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace agent_cli {

using json = nlohmann::json;

// ============================================================
// 聊天消息条目
// ============================================================

enum class EntryKind {
  UserMsg,
  AssistantText,
  ToolCall,
  ToolResult,
  SubtaskStart,
  SubtaskEnd,
  Error,
  SystemInfo,
};

inline std::string to_string(EntryKind kind) {
  switch (kind) {
    case EntryKind::UserMsg:
      return "UserMsg";
    case EntryKind::AssistantText:
      return "AssistantText";
    case EntryKind::ToolCall:
      return "ToolCall";
    case EntryKind::ToolResult:
      return "ToolResult";
    case EntryKind::SubtaskStart:
      return "SubtaskStart";
    case EntryKind::SubtaskEnd:
      return "SubtaskEnd";
    case EntryKind::Error:
      return "Error";
    case EntryKind::SystemInfo:
      return "SystemInfo";
  }
  return "Unknown";
}

struct ChatEntry {
  EntryKind kind;
  std::string text;
  std::string detail;  // 可选的额外信息 (args, result 等)
};

// ============================================================
// 线程安全的聊天日志
// ============================================================

class ChatLog {
 public:
  void push(ChatEntry entry) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.push_back(std::move(entry));
  }

  // 追加文本到最后一个 AssistantText entry（用于流式输出）
  void append_stream(const std::string& delta) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!entries_.empty() && entries_.back().kind == EntryKind::AssistantText) {
      entries_.back().text += delta;
    } else {
      entries_.push_back({EntryKind::AssistantText, delta, ""});
    }
  }

  std::vector<ChatEntry> snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
  }

  // 获取最后一条 entry（用于测试）
  ChatEntry last() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (entries_.empty()) return {EntryKind::SystemInfo, "", ""};
    return entries_.back();
  }

  // 获取指定类型的所有 entries
  std::vector<ChatEntry> filter(EntryKind kind) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ChatEntry> result;
    for (const auto& e : entries_) {
      if (e.kind == kind) result.push_back(e);
    }
    return result;
  }

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
  void start_tool(const std::string& name, const std::string& args_summary) {
    std::lock_guard<std::mutex> lock(mu_);
    activities_.push_back({name, "running", args_summary});
  }

  void finish_tool(const std::string& name, const std::string& result_summary, bool is_error) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = activities_.rbegin(); it != activities_.rend(); ++it) {
      if (it->tool_name == name && it->status == "running") {
        it->status = is_error ? "error" : "done";
        it->summary = result_summary;
        break;
      }
    }
  }

  std::vector<ToolActivity> snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (activities_.size() <= 50) return activities_;
    return {activities_.end() - 50, activities_.end()};
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return activities_.size();
  }

  // 查找某个工具的最新状态
  std::string tool_status(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = activities_.rbegin(); it != activities_.rend(); ++it) {
      if (it->tool_name == name) return it->status;
    }
    return "";
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    activities_.clear();
  }

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
  Unknown,   // 无法识别的 / 命令
};

struct CommandDef {
  std::string name;         // "/quit"
  std::string shortcut;     // "/q"
  std::string description;  // "退出程序"
  CommandType type;
};

inline const std::vector<CommandDef>& command_defs() {
  static const std::vector<CommandDef> defs = {
      {"/quit", "/q", "退出程序", CommandType::Quit},
      {"/clear", "", "清空聊天记录", CommandType::Clear},
      {"/help", "/h", "显示帮助信息", CommandType::Help},
      {"/sessions", "/s", "管理会话", CommandType::Sessions},
      {"/compact", "", "压缩上下文", CommandType::Compact},
      {"/expand", "", "展开所有工具调用", CommandType::Expand},
      {"/collapse", "", "折叠所有工具调用", CommandType::Collapse},
  };
  return defs;
}

// 根据前缀匹配命令（用于补全提示）
inline std::vector<CommandDef> match_commands(const std::string& prefix) {
  std::vector<CommandDef> result;
  if (prefix.empty() || prefix[0] != '/') return result;
  std::string lower_prefix = prefix;
  for (auto& c : lower_prefix) c = static_cast<char>(std::tolower(c));
  for (const auto& def : command_defs()) {
    if (def.name.substr(0, lower_prefix.size()) == lower_prefix ||
        (!def.shortcut.empty() && def.shortcut.substr(0, lower_prefix.size()) == lower_prefix)) {
      result.push_back(def);
    }
  }
  return result;
}

struct ParsedCommand {
  CommandType type = CommandType::None;
  std::string arg;  // 可选参数
};

inline ParsedCommand parse_command(const std::string& input) {
  if (input.empty()) return {CommandType::None, ""};
  if (input[0] != '/') return {CommandType::None, ""};

  // 分割命令和参数
  auto space_pos = input.find(' ');
  std::string cmd = (space_pos != std::string::npos) ? input.substr(0, space_pos) : input;
  std::string arg = (space_pos != std::string::npos) ? input.substr(space_pos + 1) : "";

  if (cmd == "/q" || cmd == "/quit") return {CommandType::Quit, arg};
  if (cmd == "/clear") return {CommandType::Clear, arg};
  if (cmd == "/h" || cmd == "/help") return {CommandType::Help, arg};
  if (cmd == "/s" || cmd == "/sessions") return {CommandType::Sessions, arg};
  if (cmd == "/compact") return {CommandType::Compact, arg};
  if (cmd == "/expand") return {CommandType::Expand, arg};
  if (cmd == "/collapse") return {CommandType::Collapse, arg};

  return {CommandType::Unknown, cmd};
}

// ============================================================
// 文本工具函数
// ============================================================

inline std::string truncate_text(const std::string& s, size_t max_len) {
  if (s.size() <= max_len) return s;
  return s.substr(0, max_len) + "...";
}

inline std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    lines.push_back(line);
  }
  if (lines.empty()) lines.push_back("");
  return lines;
}

// 格式化 token 数量为可读字符串
inline std::string format_tokens(int64_t tokens) {
  if (tokens < 1000) return std::to_string(tokens);
  if (tokens < 1000000) {
    double k = static_cast<double>(tokens) / 1000.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fK", k);
    return buf;
  }
  double m = static_cast<double>(tokens) / 1000000.0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fM", m);
  return buf;
}

// ============================================================
// Agent 状态管理
// ============================================================

class AgentState {
 public:
  void set_running(bool running) {
    running_.store(running);
  }
  bool is_running() const {
    return running_.load();
  }

  void set_model(const std::string& model) {
    std::lock_guard<std::mutex> lock(mu_);
    model_ = model;
  }
  std::string model() const {
    std::lock_guard<std::mutex> lock(mu_);
    return model_;
  }

  void set_session_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    session_id_ = id;
  }
  std::string session_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return session_id_;
  }

  void update_tokens(int64_t input, int64_t output) {
    input_tokens_.store(input);
    output_tokens_.store(output);
  }
  int64_t input_tokens() const {
    return input_tokens_.load();
  }
  int64_t output_tokens() const {
    return output_tokens_.load();
  }

  std::string status_text() const {
    std::string s = "Model: " + model();
    s += " | Tokens: " + format_tokens(input_tokens()) + "in/" + format_tokens(output_tokens()) + "out";
    s += is_running() ? " | [Running...]" : " | [Ready]";
    return s;
  }

 private:
  std::atomic<bool> running_{false};
  std::atomic<int64_t> input_tokens_{0};
  std::atomic<int64_t> output_tokens_{0};
  mutable std::mutex mu_;
  std::string model_;
  std::string session_id_;
};

}  // namespace agent_cli
