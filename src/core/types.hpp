#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace agent {

using json = nlohmann::json;

// Forward declarations
class Session;

class Tool;

class Message;

// Type aliases
using SessionId = std::string;
using MessageId = std::string;
using ToolId = std::string;
using AgentId = std::string;

using Timestamp = std::chrono::system_clock::time_point;

// Result type for operations that can fail
template <typename T>
struct Result {
  std::optional<T> value;
  std::optional<std::string> error;

  bool ok() const {
    return value.has_value();
  }

  bool failed() const {
    return error.has_value();
  }

  static Result success(T val) {
    return Result{std::move(val), std::nullopt};
  }

  static Result failure(std::string err) {
    return Result{std::nullopt, std::move(err)};
  }
};

// Token usage tracking
struct TokenUsage {
  int64_t input_tokens = 0;
  int64_t output_tokens = 0;
  int64_t cache_read_tokens = 0;
  int64_t cache_write_tokens = 0;

  int64_t total() const {
    return input_tokens + output_tokens;
  }

  TokenUsage &operator+=(const TokenUsage &other) {
    input_tokens += other.input_tokens;
    output_tokens += other.output_tokens;
    cache_read_tokens += other.cache_read_tokens;
    cache_write_tokens += other.cache_write_tokens;
    return *this;
  }
};

// Finish reason for LLM responses
enum class FinishReason {
  Stop,       // Natural completion
  ToolCalls,  // Needs tool execution
  Length,     // Token limit reached
  Error,      // Error occurred
  Cancelled   // User cancelled
};

std::string to_string(FinishReason reason);

FinishReason finish_reason_from_string(const std::string &str);

// Permission levels
enum class Permission {
  Allow,
  Deny,
  Ask  // Prompt user
};

// Agent types
enum class AgentType {
  Build,      // Main coding agent
  Explore,    // Read-only exploration
  General,    // General purpose subagent
  Plan,       // Planning agent
  Compaction  // Context compression agent
};

std::string to_string(AgentType type);

AgentType agent_type_from_string(const std::string &str);

// Model info
struct ModelInfo {
  std::string id;
  std::string provider;  // "anthropic", "openai", etc.
  int64_t context_window = 128000;
  int64_t max_output_tokens = 8192;
  bool supports_vision = false;
  bool supports_tools = true;
};

// Provider configuration
struct ProviderConfig {
  std::string name;
  std::string api_key;
  std::string base_url;
  std::optional<std::string> organization;
  std::map<std::string, std::string> headers;
};

}  // namespace agent
