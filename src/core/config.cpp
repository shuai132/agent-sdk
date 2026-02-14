#include "agent/core/config.hpp"

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
    
    // Save context settings
    j["context"] = {
        {"prune_protect_tokens", context.prune_protect_tokens},
        {"prune_minimum_tokens", context.prune_minimum_tokens},
        {"truncate_max_lines", context.truncate_max_lines},
        {"truncate_max_bytes", context.truncate_max_bytes}
    };
    
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
    return home_dir() / ".config" / "agent-cpp";
}

fs::path default_config_file() {
    return config_dir() / "config.json";
}

fs::path project_config_file() {
    return fs::current_path() / ".agent-cpp" / "config.json";
}

std::vector<fs::path> find_agent_instructions(const fs::path& start_dir) {
    std::vector<fs::path> result;
    
    fs::path current = start_dir;
    while (current.has_parent_path()) {
        // Check for AGENTS.md
        auto agents_md = current / "AGENTS.md";
        if (fs::exists(agents_md)) {
            result.push_back(agents_md);
        }
        
        // Check in .agent-cpp directory
        auto agent_dir_md = current / ".agent-cpp" / "AGENTS.md";
        if (fs::exists(agent_dir_md)) {
            result.push_back(agent_dir_md);
        }
        
        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    
    // Reverse so parent instructions come first
    std::reverse(result.begin(), result.end());
    
    // Add global instructions
    auto global_md = config_dir() / "AGENTS.md";
    if (fs::exists(global_md)) {
        result.insert(result.begin(), global_md);
    }
    
    return result;
}

}  // namespace config_paths

}  // namespace agent
