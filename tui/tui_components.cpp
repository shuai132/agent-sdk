#include "tui_components.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sstream>

namespace agent_cli {

// ============================================================
// EntryKind
// ============================================================

std::string to_string(EntryKind kind) {
  switch (kind) {
    case EntryKind::UserMsg:
      return "UserMsg";
    case EntryKind::AssistantText:
      return "AssistantText";
    case EntryKind::Thinking:
      return "Thinking";
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

// ============================================================
// ChatLog
// ============================================================

void ChatLog::push(ChatEntry entry) {
  std::lock_guard<std::mutex> lock(mu_);
  entries_.push_back(std::move(entry));
}

void ChatLog::append_stream(const std::string& delta) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!entries_.empty() && entries_.back().kind == EntryKind::AssistantText) {
    entries_.back().text += delta;
  } else {
    entries_.push_back({EntryKind::AssistantText, delta, ""});
  }
}

std::vector<ChatEntry> ChatLog::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_;
}

size_t ChatLog::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_.size();
}

void ChatLog::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  entries_.clear();
}

ChatEntry ChatLog::last() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (entries_.empty()) return {EntryKind::SystemInfo, "", ""};
  return entries_.back();
}

std::vector<ChatEntry> ChatLog::filter(EntryKind kind) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<ChatEntry> result;
  for (const auto& e : entries_) {
    if (e.kind == kind) result.push_back(e);
  }
  return result;
}

void ChatLog::add_nested_entry(const std::string& tool_name, ChatEntry nested_entry) {
  std::lock_guard<std::mutex> lock(mu_);
  // Find the most recent ToolCall entry with matching tool name
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->kind == EntryKind::ToolCall && it->text == tool_name) {
      it->nested_entries.push_back(std::move(nested_entry));
      return;
    }
  }
}

void ChatLog::update_tool_activity(const std::string& tool_name, const std::string& activity) {
  std::lock_guard<std::mutex> lock(mu_);
  // Find the most recent ToolCall entry with matching tool name and update its detail
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->kind == EntryKind::ToolCall && it->text == tool_name) {
      // Store activity in a special format that renderer can parse
      // We'll append it to detail with a separator
      auto pos = it->detail.find("\n__ACTIVITY__:");
      if (pos != std::string::npos) {
        it->detail = it->detail.substr(0, pos);
      }
      if (!activity.empty()) {
        it->detail += "\n__ACTIVITY__:" + activity;
      }
      return;
    }
  }
}

// ============================================================
// ToolPanel
// ============================================================

void ToolPanel::start_tool(const std::string& name, const std::string& args_summary) {
  std::lock_guard<std::mutex> lock(mu_);
  activities_.push_back({name, "running", args_summary});
}

void ToolPanel::finish_tool(const std::string& name, const std::string& result_summary, bool is_error) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = activities_.rbegin(); it != activities_.rend(); ++it) {
    if (it->tool_name == name && it->status == "running") {
      it->status = is_error ? "error" : "done";
      it->summary = result_summary;
      break;
    }
  }
}

std::vector<ToolActivity> ToolPanel::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (activities_.size() <= 50) return activities_;
  return {activities_.end() - 50, activities_.end()};
}

size_t ToolPanel::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return activities_.size();
}

std::string ToolPanel::tool_status(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = activities_.rbegin(); it != activities_.rend(); ++it) {
    if (it->tool_name == name) return it->status;
  }
  return "";
}

void ToolPanel::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  activities_.clear();
}

// ============================================================
// 命令解析
// ============================================================

const std::vector<CommandDef>& command_defs() {
  static const std::vector<CommandDef> defs = {
      {"/quit", "/q", "退出程序", CommandType::Quit},
      {"/clear", "", "清空聊天记录", CommandType::Clear},
      {"/help", "/h", "显示帮助信息", CommandType::Help},
      {"/sessions", "/s", "管理会话", CommandType::Sessions},
      {"/compact", "", "压缩上下文", CommandType::Compact},
      {"/expand", "", "展开所有工具调用", CommandType::Expand},
      {"/collapse", "", "折叠所有工具调用", CommandType::Collapse},
      {"/copy", "/c", "复制聊天内容到剪贴板", CommandType::Copy},
  };
  return defs;
}

std::vector<CommandDef> match_commands(const std::string& prefix) {
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

ParsedCommand parse_command(const std::string& input) {
  if (input.empty()) return {CommandType::None, ""};
  if (input[0] != '/') return {CommandType::None, ""};

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
  if (cmd == "/c" || cmd == "/copy") return {CommandType::Copy, arg};
  return {CommandType::Unknown, cmd};
}

// ============================================================
// 文本工具函数
// ============================================================

std::string truncate_text(const std::string& s, size_t max_len) {
  if (s.size() <= max_len) return s;
  return s.substr(0, max_len) + "...";
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    lines.push_back(line);
  }
  if (lines.empty()) lines.push_back("");
  return lines;
}

std::string format_time(const std::chrono::system_clock::time_point& ts) {
  auto time_t = std::chrono::system_clock::to_time_t(ts);
  std::tm tm{};
  localtime_r(&time_t, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

std::string format_tokens(int64_t tokens) {
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
// AgentMode
// ============================================================

std::string to_string(AgentMode mode) {
  switch (mode) {
    case AgentMode::Build:
      return "build";
    case AgentMode::Plan:
      return "plan";
  }
  return "build";
}

// ============================================================
// AgentState
// ============================================================

void AgentState::set_running(bool running) {
  running_.store(running);
}

bool AgentState::is_running() const {
  return running_.load();
}

void AgentState::set_model(const std::string& model) {
  std::lock_guard<std::mutex> lock(mu_);
  model_ = model;
}

std::string AgentState::model() const {
  std::lock_guard<std::mutex> lock(mu_);
  return model_;
}

void AgentState::set_session_id(const std::string& id) {
  std::lock_guard<std::mutex> lock(mu_);
  session_id_ = id;
}

std::string AgentState::session_id() const {
  std::lock_guard<std::mutex> lock(mu_);
  return session_id_;
}

void AgentState::update_tokens(int64_t input, int64_t output) {
  input_tokens_.store(input);
  output_tokens_.store(output);
}

int64_t AgentState::input_tokens() const {
  return input_tokens_.load();
}

int64_t AgentState::output_tokens() const {
  return output_tokens_.load();
}

void AgentState::update_context(int64_t used, int64_t limit) {
  context_used_.store(used);
  if (limit > 0) {
    context_limit_.store(limit);
  }
}

int64_t AgentState::context_used() const {
  return context_used_.load();
}

int64_t AgentState::context_limit() const {
  return context_limit_.load();
}

float AgentState::context_ratio() const {
  int64_t limit = context_limit_.load();
  if (limit <= 0) return 0.0f;
  return static_cast<float>(context_used_.load()) / static_cast<float>(limit);
}

void AgentState::set_activity(const std::string& msg) {
  std::lock_guard<std::mutex> lock(mu_);
  activity_ = msg;
}

std::string AgentState::activity() const {
  std::lock_guard<std::mutex> lock(mu_);
  return activity_;
}

void AgentState::set_mode(AgentMode mode) {
  mode_.store(static_cast<int>(mode));
}

AgentMode AgentState::mode() const {
  return static_cast<AgentMode>(mode_.load());
}

void AgentState::toggle_mode() {
  auto current = mode();
  set_mode(current == AgentMode::Build ? AgentMode::Plan : AgentMode::Build);
}

std::string AgentState::status_text() const {
  std::string s = "Model: " + model();
  s += " | Tokens: " + format_tokens(input_tokens()) + "in/" + format_tokens(output_tokens()) + "out";
  s += is_running() ? " | [Running...]" : " | [Ready]";
  return s;
}

// ============================================================
// 文件路径自动完成
// ============================================================

std::string extract_file_path_prefix(const std::string& input) {
  // 查找最后一个 @ 符号
  size_t at_pos = input.rfind('@');
  if (at_pos == std::string::npos) {
    return "";
  }

  // 提取 @ 符号之后的部分
  std::string path_part = input.substr(at_pos + 1);

  // 如果路径部分包含空格（且不是在引号内），则只取到第一个空格前的部分
  // 这里简化处理，直接返回路径部分
  return path_part;
}

std::vector<FilePathMatch> match_file_paths(const std::string& prefix) {
  std::vector<FilePathMatch> result;

  if (prefix.empty()) {
    // 如果没有前缀，列出当前目录的内容
    try {
      for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path())) {
        std::string filename = entry.path().filename().string();
        std::string relative_path = filename;  // 对于当前目录的文件，相对路径就是文件名
        if (entry.is_directory()) {
          result.push_back({relative_path, filename + "/", true});
        } else {
          result.push_back({relative_path, filename, false});
        }
      }
    } catch (...) {
      // 忽略错误
    }
    return result;
  }

  // 分离目录路径和文件名前缀
  std::filesystem::path prefix_path(prefix);
  std::filesystem::path dir_path;
  std::string file_prefix;

  // 如果前缀以 / 结尾或者是目录，则将其视为目录路径
  if (prefix.back() == '/' || std::filesystem::is_directory(prefix_path)) {
    dir_path = prefix_path;
    file_prefix = "";
  } else {
    // 否则分离目录和文件名
    dir_path = prefix_path.parent_path();
    if (dir_path.string().empty()) {
      dir_path = std::filesystem::current_path();
    }
    file_prefix = prefix_path.filename().string();
  }

  // 确保目录存在
  if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
    return result;
  }

  try {
    // 计算相对于当前工作目录的路径前缀
    std::filesystem::path current_dir = std::filesystem::current_path();
    std::string path_prefix = std::filesystem::relative(dir_path, current_dir).string();
    if (path_prefix == ".") path_prefix = "";  // 如果是当前目录，移除点

    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
      std::string filename = entry.path().filename().string();

      // 检查文件名是否匹配前缀（不区分大小写）
      std::string lower_filename = filename;
      std::string lower_prefix = file_prefix;
      std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(), ::tolower);
      std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(), ::tolower);

      if (file_prefix.empty() ||
          lower_filename.length() >= lower_prefix.length() && lower_filename.substr(0, lower_prefix.length()) == lower_prefix) {
        std::string display_name = filename;
        std::string relative_path = path_prefix.empty() ? filename : path_prefix + "/" + filename;
        if (entry.is_directory()) {
          display_name += "/";
          relative_path += "/";
        }

        result.push_back({relative_path, display_name, entry.is_directory()});
      }
    }

    // 按照名称排序，目录优先
    std::sort(result.begin(), result.end(), [](const FilePathMatch& a, const FilePathMatch& b) {
      if (a.is_directory && !b.is_directory) return true;
      if (!a.is_directory && b.is_directory) return false;
      return a.display < b.display;
    });
  } catch (...) {
    // 忽略错误
  }

  return result;
}

}  // namespace agent_cli
