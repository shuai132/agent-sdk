// Agent initialization
#include "agent.hpp"

#include <filesystem>

#include "core/version.hpp"
#include "llm/anthropic.hpp"
#include "log/log.h"
#include "mcp/client.hpp"
#include "plugin/qwen/qwen_oauth.hpp"
#include "skill/skill.hpp"
#include "tool/builtin/builtins.hpp"

namespace agent {

// Force inclusion of Anthropic provider registration
namespace {
// This function exists to force the linker to include the anthropic.cpp
// translation unit which contains the static provider registration
void force_provider_registration() {
  // Just reference the type to ensure the translation unit is linked
  (void)sizeof(llm::AnthropicProvider);
}
}  // namespace

void init() {
  // 初始化日志系统
  init_log();

  force_provider_registration();
  tools::register_builtins();

  // Register Qwen OAuth plugin for portal.qwen.ai authentication
  plugin::qwen::register_qwen_plugin();

  // Discover skills from current working directory and standard locations
  auto cwd = std::filesystem::current_path();
  auto config = Config::load_default();
  skill::SkillRegistry::instance().discover(cwd, config.skill_paths);

  // Initialize MCP servers from config
  if (!config.mcp_servers.empty()) {
    auto& mcp_mgr = mcp::McpManager::instance();
    mcp_mgr.initialize(config.mcp_servers);
    mcp_mgr.connect_all();
    mcp_mgr.register_tools();
  }
}

void shutdown() {
  mcp::McpManager::instance().disconnect_all();
}

std::string version() {
  return AGENT_SDK_VERSION_STRING;
}

}  // namespace agent
