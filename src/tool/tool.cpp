#include "tool/tool.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/types.hpp"

namespace agent {

namespace fs = std::filesystem;

// Parameter schema to JSON
json ParameterSchema::to_json_schema() const {
  json schema;
  schema["type"] = type;
  schema["description"] = description;

  if (default_value) {
    schema["default"] = *default_value;
  }

  if (enum_values && !enum_values->empty()) {
    schema["enum"] = *enum_values;
  }

  return schema;
}

// Tool to JSON schema
json Tool::to_json_schema() const {
  json schema;
  schema["name"] = id();
  schema["description"] = description();

  json properties;
  json required_props = json::array();

  for (const auto &param : parameters()) {
    properties[param.name] = param.to_json_schema();
    if (param.required) {
      required_props.push_back(param.name);
    }
  }

  schema["input_schema"] = {{"type", "object"}, {"properties", properties}, {"required", required_props}};

  return schema;
}

Result<json> Tool::validate_args(const json &args) const {
  auto params = parameters();

  for (const auto &param : params) {
    if (param.required && !args.contains(param.name)) {
      return Result<json>::failure("Missing required parameter: " + param.name);
    }
  }

  return Result<json>::success(args);
}

// SimpleTool implementation
SimpleTool::SimpleTool(std::string id, std::string description) : id_(std::move(id)), description_(std::move(description)) {}

// Tool Registry
ToolRegistry &ToolRegistry::instance() {
  static ToolRegistry instance;
  return instance;
}

void ToolRegistry::register_tool(std::shared_ptr<Tool> tool) {
  std::lock_guard lock(mutex_);
  tools_[tool->id()] = std::move(tool);
}

void ToolRegistry::unregister_tool(const std::string &id) {
  std::lock_guard lock(mutex_);
  tools_.erase(id);
}

std::shared_ptr<Tool> ToolRegistry::get(const std::string &id) const {
  std::lock_guard lock(mutex_);
  auto it = tools_.find(id);
  if (it != tools_.end()) {
    return it->second;
  }
  return nullptr;
}

std::vector<std::shared_ptr<Tool>> ToolRegistry::all() const {
  std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<Tool>> result;
  result.reserve(tools_.size());
  for (const auto &[id, tool] : tools_) {
    result.push_back(tool);
  }
  return result;
}

std::vector<std::shared_ptr<Tool>> ToolRegistry::for_agent(const AgentConfig &agent) const {
  auto all_tools = all();
  std::vector<std::shared_ptr<Tool>> result;

  for (const auto &tool : all_tools) {
    bool allowed = true;

    // Check denied list
    for (const auto &denied : agent.denied_tools) {
      if (tool->id() == denied) {
        allowed = false;
        break;
      }
    }

    // Check allowed list (if not empty, only allow listed tools)
    if (!agent.allowed_tools.empty()) {
      allowed = false;
      for (const auto &allowed_tool : agent.allowed_tools) {
        if (tool->id() == allowed_tool) {
          allowed = true;
          break;
        }
      }
    }

    if (allowed) {
      result.push_back(tool);
    }
  }

  return result;
}

// Truncation helpers
namespace Truncate {

TruncateResult output(const std::string &text, size_t max_lines, size_t max_bytes) {
  TruncateResult result;
  result.truncated = false;

  // Sanitize input to ensure valid UTF-8 (prevents nlohmann::json type_error.316)
  std::string safe_text = sanitize_utf8(text);

  // Check byte limit
  if (safe_text.size() > max_bytes) {
    result.truncated = true;
    result.content = safe_text.substr(0, max_bytes);
    result.content += "\n... [Output truncated. " + std::to_string(safe_text.size() - max_bytes) + " bytes omitted]";
    return result;
  }

  // Check line limit
  size_t line_count = 0;
  size_t pos = 0;
  size_t last_newline = 0;

  while ((pos = safe_text.find('\n', pos)) != std::string::npos) {
    line_count++;
    last_newline = pos;
    pos++;

    if (line_count >= max_lines) {
      result.truncated = true;
      result.content = safe_text.substr(0, last_newline);

      // Count remaining lines
      size_t remaining = 0;
      while ((pos = safe_text.find('\n', pos)) != std::string::npos) {
        remaining++;
        pos++;
      }

      result.content += "\n... [" + std::to_string(remaining) + " lines truncated]";
      return result;
    }
  }

  result.content = safe_text;
  return result;
}

TruncateResult save_and_truncate(const std::string &text, const std::string &tool_name, size_t max_lines, size_t max_bytes) {
  auto truncated = output(text, max_lines, max_bytes);

  if (truncated.truncated) {
    // Save full output to temp file
    auto temp_dir = fs::temp_directory_path() / "agent-cpp" / "tool_outputs";
    fs::create_directories(temp_dir);

    auto filename = tool_name + "_" + UUID::short_id() + ".txt";
    auto path = temp_dir / filename;

    std::ofstream file(path);
    if (file.is_open()) {
      file << text;
      file.close();
      truncated.full_output_path = path.string();
      truncated.content += "\nFull output saved to: " + path.string();
    }
  }

  return truncated;
}

}  // namespace Truncate

}  // namespace agent
