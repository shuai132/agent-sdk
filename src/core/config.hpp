#pragma once

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "types.hpp"

namespace agent {

// Agent configuration
struct AgentConfig {
  AgentId id;
  AgentType type = AgentType::Build;
  std::string model;
  std::string system_prompt;

  // Tool permissions: tool_id -> Permission
  std::map<std::string, Permission> permissions;

  // Default permission for unlisted tools
  Permission default_permission = Permission::Ask;

  // Context limits
  int64_t max_tokens = 100000;

  // Allowed tools (empty = all)
  std::vector<std::string> allowed_tools;
  std::vector<std::string> denied_tools;
};

// MCP server configuration
struct McpServerConfig {
  std::string name;
  std::string type;  // "local", "remote", or "qwen-portal"

  // For local (stdio) servers
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;

  // For remote servers
  std::string url;
  std::map<std::string, std::string> headers;

  // For OAuth-based servers (e.g., qwen-portal)
  std::string auth_type;  // "oauth" for OAuth-based auth, empty for API key

  bool enabled = true;
};

// Application configuration
struct Config {
  // Provider configs
  std::map<std::string, ProviderConfig> providers;

  // Default model
  std::string default_model = "claude-sonnet-4-20250514";

  // Agent configurations
  std::map<AgentId, AgentConfig> agents;

  // MCP servers
  std::vector<McpServerConfig> mcp_servers;

  // Working directory
  std::filesystem::path working_dir = std::filesystem::current_path();

  // Instructions files to load
  std::vector<std::string> instructions;

  // Skill paths
  std::vector<std::filesystem::path> skill_paths;

  // Context management settings
  struct ContextSettings {
    int64_t prune_protect_tokens = 40000;
    int64_t prune_minimum_tokens = 20000;
    size_t truncate_max_lines = 2000;
    size_t truncate_max_bytes = 51200;
  } context;

  // Logging
  std::string log_level = "info";
  std::optional<std::filesystem::path> log_file;

  // Load from file
  static Config load(const std::filesystem::path& path);

  // Load default config from project/global config files
  static Config load_default();

  // Load config from environment variables, with file config as base
  // Reads: ANTHROPIC_API_KEY/AUTH_TOKEN, ANTHROPIC_BASE_URL, ANTHROPIC_MODEL
  //        OPENAI_API_KEY, OPENAI_BASE_URL, OPENAI_MODEL
  //        QWEN_OAUTH (true/1/yes), QWEN_BASE_URL, QWEN_MODEL
  // Note: QWEN_OAUTH takes precedence over OPENAI_API_KEY
  static Config from_env();

  // Save to file
  void save(const std::filesystem::path& path) const;

  // Get provider config
  std::optional<ProviderConfig> get_provider(const std::string& name) const;

  // Get agent config
  std::optional<AgentConfig> get_agent(const AgentId& id) const;

  AgentConfig get_or_create_agent(AgentType type) const;
};

// Configuration paths
namespace config_paths {
std::filesystem::path home_dir();

std::filesystem::path config_dir();

std::filesystem::path default_config_file();

std::filesystem::path project_config_file();

// Find the git worktree root from a starting directory (returns nullopt if not in a git repo)
std::optional<std::filesystem::path> find_git_root(const std::filesystem::path& start_dir);

// Find AGENTS.md / CLAUDE.md files (hierarchical, multi-convention compatible)
// Searches: AGENTS.md, .agent-sdk/AGENTS.md, .agents/AGENTS.md,
//           CLAUDE.md, .claude/CLAUDE.md, .opencode/AGENTS.md
// Stops at git worktree root (or filesystem root)
std::vector<std::filesystem::path> find_agent_instructions(const std::filesystem::path& start_dir);
}  // namespace config_paths

}  // namespace agent
