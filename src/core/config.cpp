#include "config.hpp"

#include <cstdlib>
#include <fstream>

namespace agent {

namespace fs = std::filesystem;

Config Config::load(const fs::path& path) {
  Config config;

  if (!fs::exists(path)) {
    return config;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    return config;
  }

  try {
    json j = json::parse(file);

    // Load providers
    if (j.contains("providers")) {
      for (auto& [name, provider_json] : j["providers"].items()) {
        ProviderConfig provider;
        provider.name = name;
        provider.api_key = provider_json.value("api_key", "");
        provider.base_url = provider_json.value("base_url", "");
        if (provider_json.contains("organization")) {
          provider.organization = provider_json["organization"];
        }
        if (provider_json.contains("headers")) {
          for (auto& [k, v] : provider_json["headers"].items()) {
            provider.headers[k] = v;
          }
        }
        config.providers[name] = provider;
      }
    }

    // Load default model
    config.default_model = j.value("default_model", "claude-sonnet-4-20250514");

    // Load MCP servers
    if (j.contains("mcp_servers")) {
      for (const auto& server_json : j["mcp_servers"]) {
        McpServerConfig server;
        server.name = server_json.value("name", "");
        server.type = server_json.value("type", "local");
        server.command = server_json.value("command", "");
        server.url = server_json.value("url", "");
        server.enabled = server_json.value("enabled", true);

        if (server_json.contains("args")) {
          for (const auto& arg : server_json["args"]) {
            server.args.push_back(arg);
          }
        }
        if (server_json.contains("env")) {
          for (auto& [k, v] : server_json["env"].items()) {
            server.env[k] = v;
          }
        }
        if (server_json.contains("headers")) {
          for (auto& [k, v] : server_json["headers"].items()) {
            server.headers[k] = v;
          }
        }

        config.mcp_servers.push_back(server);
      }
    }

    // Load agents
    if (j.contains("agents")) {
      for (auto& [id, agent_json] : j["agents"].items()) {
        AgentConfig agent;
        agent.id = id;
        agent.type = agent_type_from_string(agent_json.value("type", "build"));
        agent.model = agent_json.value("model", "");
        agent.system_prompt = agent_json.value("system_prompt", "");
        agent.max_tokens = agent_json.value("max_tokens", 100000);
        agent.default_permission = permission_from_string(agent_json.value("default_permission", "ask"));

        if (agent_json.contains("allowed_tools")) {
          for (const auto& tool : agent_json["allowed_tools"]) {
            agent.allowed_tools.push_back(tool);
          }
        }
        if (agent_json.contains("denied_tools")) {
          for (const auto& tool : agent_json["denied_tools"]) {
            agent.denied_tools.push_back(tool);
          }
        }
        if (agent_json.contains("permissions")) {
          for (auto& [tool_id, perm_str] : agent_json["permissions"].items()) {
            agent.permissions[tool_id] = permission_from_string(perm_str);
          }
        }

        config.agents[id] = agent;
      }
    }

    // Load context settings
    if (j.contains("context")) {
      const auto& ctx = j["context"];
      config.context.prune_protect_tokens = ctx.value("prune_protect_tokens", 40000);
      config.context.prune_minimum_tokens = ctx.value("prune_minimum_tokens", 20000);
      config.context.truncate_max_lines = ctx.value("truncate_max_lines", 2000);
      config.context.truncate_max_bytes = ctx.value("truncate_max_bytes", 51200);
    }

    // Load instructions
    if (j.contains("instructions")) {
      for (const auto& instr : j["instructions"]) {
        config.instructions.push_back(instr);
      }
    }

    // Load skill paths
    if (j.contains("skill_paths")) {
      for (const auto& path : j["skill_paths"]) {
        config.skill_paths.push_back(path.get<std::string>());
      }
    }

    config.log_level = j.value("log_level", "info");
    if (j.contains("log_file")) {
      config.log_file = j["log_file"].get<std::string>();
    }

  } catch (const std::exception& e) {
    // Log error and return default config
  }

  return config;
}

Config Config::load_default() {
  // Try to load from project config first, then global
  auto project_config = config_paths::project_config_file();
  if (fs::exists(project_config)) {
    return load(project_config);
  }

  auto global_config = config_paths::default_config_file();
  if (fs::exists(global_config)) {
    return load(global_config);
  }

  return Config{};
}

Config Config::from_env() {
  Config config = load_default();

  // Read Anthropic provider from environment
  const char* anthropic_key = std::getenv("ANTHROPIC_API_KEY");
  if (!anthropic_key) {
    anthropic_key = std::getenv("ANTHROPIC_AUTH_TOKEN");
  }

  if (anthropic_key) {
    const char* base_url = std::getenv("ANTHROPIC_BASE_URL");
    const char* model = std::getenv("ANTHROPIC_MODEL");

    ProviderConfig provider;
    provider.name = "anthropic";
    provider.api_key = anthropic_key;
    provider.base_url = base_url ? base_url : "https://api.anthropic.com";

    config.providers["anthropic"] = provider;

    if (model) {
      config.default_model = model;
    }
  }

  // Read Qwen OAuth or OpenAI provider from environment
  // Qwen OAuth takes precedence over OpenAI if both are set
  const char* qwen_oauth = std::getenv("QWEN_OAUTH");
  bool is_qwen_oauth = qwen_oauth && (std::string(qwen_oauth) == "true" || std::string(qwen_oauth) == "1" || std::string(qwen_oauth) == "yes");

  if (is_qwen_oauth) {
    // Qwen OAuth mode: uses OpenAI-compatible API at portal.qwen.ai
    const char* base_url = std::getenv("QWEN_BASE_URL");
    const char* model = std::getenv("QWEN_MODEL");

    ProviderConfig provider;
    provider.name = "openai";         // Reuse OpenAI provider implementation
    provider.api_key = "qwen-oauth";  // Special marker for OAuth flow
    provider.base_url = base_url ? base_url : "https://portal.qwen.ai";

    config.providers["openai"] = provider;

    if (model) {
      config.default_model = model;
    } else if (!anthropic_key) {
      config.default_model = "coder-model";
    }
  } else {
    // Standard OpenAI provider
    const char* openai_key = std::getenv("OPENAI_API_KEY");

    if (openai_key) {
      const char* base_url = std::getenv("OPENAI_BASE_URL");
      const char* model = std::getenv("OPENAI_MODEL");

      ProviderConfig provider;
      provider.name = "openai";
      provider.api_key = openai_key;
      provider.base_url = base_url ? base_url : "https://api.openai.com";

      config.providers["openai"] = provider;

      if (model) {
        config.default_model = model;
      } else if (!anthropic_key) {
        config.default_model = "gpt-4o";
      }
    }
  }

  return config;
}

void Config::save(const fs::path& path) const {
  json j;

  // Save providers
  json providers_json;
  for (const auto& [name, provider] : providers) {
    json p;
    p["api_key"] = provider.api_key;
    p["base_url"] = provider.base_url;
    if (provider.organization) {
      p["organization"] = *provider.organization;
    }
    if (!provider.headers.empty()) {
      p["headers"] = provider.headers;
    }
    providers_json[name] = p;
  }
  j["providers"] = providers_json;

  j["default_model"] = default_model;

  // Save MCP servers
  json servers_json = json::array();
  for (const auto& server : mcp_servers) {
    json s;
    s["name"] = server.name;
    s["type"] = server.type;
    s["command"] = server.command;
    s["url"] = server.url;
    s["enabled"] = server.enabled;
    s["args"] = server.args;
    s["env"] = server.env;
    s["headers"] = server.headers;
    servers_json.push_back(s);
  }
  j["mcp_servers"] = servers_json;

  // Save agents
  json agents_json;
  for (const auto& [id, agent] : agents) {
    json a;
    a["type"] = to_string(agent.type);
    a["model"] = agent.model;
    a["system_prompt"] = agent.system_prompt;
    a["max_tokens"] = agent.max_tokens;
    a["default_permission"] = to_string(agent.default_permission);
    a["allowed_tools"] = agent.allowed_tools;
    a["denied_tools"] = agent.denied_tools;

    if (!agent.permissions.empty()) {
      json perms;
      for (const auto& [tool_id, perm] : agent.permissions) {
        perms[tool_id] = to_string(perm);
      }
      a["permissions"] = perms;
    }

    agents_json[id] = a;
  }
  j["agents"] = agents_json;

  // Save context settings
  j["context"] = {{"prune_protect_tokens", context.prune_protect_tokens},
                  {"prune_minimum_tokens", context.prune_minimum_tokens},
                  {"truncate_max_lines", context.truncate_max_lines},
                  {"truncate_max_bytes", context.truncate_max_bytes}};

  j["instructions"] = instructions;

  json skill_paths_json = json::array();
  for (const auto& p : skill_paths) {
    skill_paths_json.push_back(p.string());
  }
  j["skill_paths"] = skill_paths_json;

  j["log_level"] = log_level;
  if (log_file) {
    j["log_file"] = log_file->string();
  }

  // Write to file
  std::ofstream file(path);
  if (file.is_open()) {
    file << j.dump(2);
  }
}

std::optional<ProviderConfig> Config::get_provider(const std::string& name) const {
  auto it = providers.find(name);
  if (it != providers.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<AgentConfig> Config::get_agent(const AgentId& id) const {
  auto it = agents.find(id);
  if (it != agents.end()) {
    return it->second;
  }
  return std::nullopt;
}

AgentConfig Config::get_or_create_agent(AgentType type) const {
  std::string type_str = to_string(type);
  auto it = agents.find(type_str);
  if (it != agents.end()) {
    return it->second;
  }

  // Create default agent config
  AgentConfig config;
  config.id = type_str;
  config.type = type;
  config.model = default_model;

  switch (type) {
    case AgentType::Build:
      config.default_permission = Permission::Ask;
      break;
    case AgentType::Explore:
      config.default_permission = Permission::Allow;
      config.denied_tools = {"write", "edit", "bash"};
      break;
    case AgentType::General:
      config.default_permission = Permission::Ask;
      break;
    case AgentType::Plan:
      config.default_permission = Permission::Deny;
      config.allowed_tools = {"read", "glob", "grep"};
      break;
    case AgentType::Compaction:
      config.default_permission = Permission::Deny;
      config.allowed_tools = {};  // No tools
      break;
  }

  return config;
}

namespace config_paths {

fs::path home_dir() {
  const char* home = std::getenv("HOME");
  if (home) {
    return fs::path(home);
  }
#ifdef _WIN32
  const char* userprofile = std::getenv("USERPROFILE");
  if (userprofile) {
    return fs::path(userprofile);
  }
#endif
  return fs::current_path();
}

fs::path config_dir() {
  return home_dir() / ".config" / "agent-sdk";
}

fs::path default_config_file() {
  return config_dir() / "config.json";
}

fs::path project_config_file() {
  return fs::current_path() / ".agent-sdk" / "config.json";
}

std::optional<fs::path> find_git_root(const fs::path& start_dir) {
  fs::path current = start_dir;
  while (true) {
    if (fs::exists(current / ".git")) {
      return current;
    }
    auto parent = current.parent_path();
    if (parent == current) break;
    current = parent;
  }
  return std::nullopt;
}

std::vector<fs::path> find_agent_instructions(const fs::path& start_dir) {
  std::vector<fs::path> result;

  // Determine the boundary: stop at git worktree root or filesystem root
  auto git_root = find_git_root(start_dir);

  fs::path current = start_dir;
  while (true) {
    // Per-directory candidates in priority order.
    // AGENTS.md variants take precedence over CLAUDE.md variants.
    // Within each convention, root-level file takes precedence over nested dir file.
    struct {
      const char* path;
    } candidates[] = {
        {"AGENTS.md"}, {".agent-sdk/AGENTS.md"}, {".agents/AGENTS.md"}, {".opencode/AGENTS.md"}, {"CLAUDE.md"}, {".claude/CLAUDE.md"},
    };

    for (const auto& c : candidates) {
      auto candidate = current / c.path;
      if (fs::exists(candidate)) {
        result.push_back(candidate);
      }
    }

    // Stop if we've reached the git root
    if (git_root && current == *git_root) break;

    auto parent = current.parent_path();
    if (parent == current) break;  // Filesystem root
    current = parent;
  }

  // Reverse so parent (more general) instructions come first
  std::reverse(result.begin(), result.end());

  // Add global instructions (highest generality, prepended)
  // Search order: agent-sdk own config > cross-tool shared > Claude compat > OpenCode compat
  auto home = home_dir();
  std::vector<fs::path> global_candidates = {
      config_dir() / "AGENTS.md",                   // ~/.config/agent-sdk/AGENTS.md
      home / ".agents" / "AGENTS.md",               // ~/.agents/AGENTS.md
      home / ".claude" / "CLAUDE.md",               // ~/.claude/CLAUDE.md
      home / ".config" / "opencode" / "AGENTS.md",  // ~/.config/opencode/AGENTS.md
  };

  // Insert global instructions in reverse order so the first found ends up first
  for (auto it = global_candidates.rbegin(); it != global_candidates.rend(); ++it) {
    if (fs::exists(*it)) {
      result.insert(result.begin(), *it);
    }
  }

  return result;
}

}  // namespace config_paths

}  // namespace agent
