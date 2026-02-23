#pragma once

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/message.hpp"
#include "core/types.hpp"

namespace agent {

// Forward declaration
class Session;

// Question info for question_handler
struct QuestionInfo {
  std::vector<std::string> questions;
};

// Question response from user
struct QuestionResponse {
  std::vector<std::string> answers;
  bool cancelled = false;
};

// Subagent event types for progress tracking
struct SubagentEvent {
  enum class Type { Stream, Thinking, ToolCall, ToolResult, Complete, Error };
  Type type;
  std::string text;
  std::string detail;  // For ToolCall: tool name, for ToolResult: result, etc.
  bool is_error = false;
};

// Tool execution context
struct ToolContext {
  SessionId session_id;
  MessageId message_id;
  std::string working_dir;

  // Abort signal
  std::shared_ptr<std::atomic<bool>> abort_signal;

  // Permission checker callback
  std::function<std::future<bool>(const std::string& permission, const std::string& description)> ask_permission;

  // Progress callback
  std::function<void(const std::string& status)> on_progress;

  // Subagent event callback (for Task tool to report child session progress)
  std::function<void(const SubagentEvent& event)> on_subagent_event;

  // Create child session callback (for Task tool)
  std::function<std::shared_ptr<Session>(AgentType)> create_child_session;

  // Question handler callback (for Question tool)
  std::function<std::future<QuestionResponse>(const QuestionInfo& info)> question_handler;
};

// Tool execution result
struct ToolResult {
  std::string output;
  std::optional<std::string> title;
  json metadata;
  bool is_error = false;

  // Factory methods
  static ToolResult success(const std::string& output) {
    return ToolResult{output, std::nullopt, json::object(), false};
  }

  static ToolResult error(const std::string& message) {
    return ToolResult{message, std::nullopt, json::object(), true};
  }

  static ToolResult with_title(const std::string& output, const std::string& title) {
    return ToolResult{output, title, json::object(), false};
  }
};

// Parameter schema (simplified JSON Schema)
struct ParameterSchema {
  std::string name;
  std::string type;  // "string", "number", "boolean", "object", "array"
  std::string description;
  bool required = true;
  std::optional<json> default_value;
  std::optional<std::vector<std::string>> enum_values;

  json to_json_schema() const;
};

// Tool definition
class Tool {
 public:
  virtual ~Tool() = default;

  // Tool identification
  virtual std::string id() const = 0;

  virtual std::string description() const = 0;

  // Parameter schema
  virtual std::vector<ParameterSchema> parameters() const = 0;

  // Execution
  virtual std::future<ToolResult> execute(const json& args, const ToolContext& ctx) = 0;

  // Generate JSON Schema for tool
  json to_json_schema() const;

  // Validate arguments
  Result<json> validate_args(const json& args) const;
};

// Base class for simpler tool implementation
class SimpleTool : public Tool {
 public:
  SimpleTool(std::string id, std::string description);

  std::string id() const override {
    return id_;
  }

  std::string description() const override {
    return description_;
  }

 protected:
  std::string id_;
  std::string description_;
};

// Tool registry
class ToolRegistry {
 public:
  static ToolRegistry& instance();

  // Register a tool
  void register_tool(std::shared_ptr<Tool> tool);

  // Unregister a tool
  void unregister_tool(const std::string& id);

  // Get a tool by ID
  std::shared_ptr<Tool> get(const std::string& id) const;

  // Get all tools
  std::vector<std::shared_ptr<Tool>> all() const;

  // Get tools filtered by agent config
  std::vector<std::shared_ptr<Tool>> for_agent(const AgentConfig& agent) const;

  // Initialize builtin tools
  void init_builtins();

 private:
  ToolRegistry() = default;

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Tool>> tools_;
};

// Truncation helper
namespace Truncate {
struct TruncateResult {
  std::string content;
  bool truncated;
  std::optional<std::string> full_output_path;
};

// Truncate output if too large
TruncateResult output(const std::string& text, size_t max_lines = 2000, size_t max_bytes = 51200);

// Save full output to file and return truncated version
TruncateResult save_and_truncate(const std::string& text, const std::string& tool_name, size_t max_lines = 2000, size_t max_bytes = 51200);
}  // namespace Truncate

}  // namespace agent
