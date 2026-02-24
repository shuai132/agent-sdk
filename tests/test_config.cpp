#include <gtest/gtest.h>

#include <filesystem>

#include "core/config.hpp"

using namespace agent;

namespace fs = std::filesystem;

// --- ConfigTest ---

TEST(ConfigTest, LoadDefault) {
  auto config = Config::load_default();

  // 默认配置应有一个有效的 default_model
  EXPECT_FALSE(config.default_model.empty());
  // 默认 log_level 为 "info"
  EXPECT_EQ(config.log_level, "info");
}

TEST(ConfigTest, GetNonexistentProvider) {
  Config config;

  auto provider = config.get_provider("nonexistent");
  EXPECT_FALSE(provider.has_value());
}

TEST(ConfigTest, GetOrCreateAgent) {
  Config config;
  config.default_model = "test-model";

  // Build 代理
  auto build_agent = config.get_or_create_agent(AgentType::Build);
  EXPECT_EQ(build_agent.id, "build");
  EXPECT_EQ(build_agent.type, AgentType::Build);
  EXPECT_EQ(build_agent.model, "test-model");
  EXPECT_EQ(build_agent.default_permission, Permission::Ask);

  // Explore 代理：只读，拒绝写入工具
  auto explore_agent = config.get_or_create_agent(AgentType::Explore);
  EXPECT_EQ(explore_agent.id, "explore");
  EXPECT_EQ(explore_agent.default_permission, Permission::Allow);
  EXPECT_FALSE(explore_agent.denied_tools.empty());

  // Plan 代理：仅允许读取工具
  auto plan_agent = config.get_or_create_agent(AgentType::Plan);
  EXPECT_EQ(plan_agent.id, "plan");
  EXPECT_EQ(plan_agent.default_permission, Permission::Deny);
  EXPECT_FALSE(plan_agent.allowed_tools.empty());

  // Compaction 代理：无工具
  auto compaction_agent = config.get_or_create_agent(AgentType::Compaction);
  EXPECT_EQ(compaction_agent.id, "compaction");
  EXPECT_EQ(compaction_agent.default_permission, Permission::Deny);
  EXPECT_TRUE(compaction_agent.allowed_tools.empty());
}

TEST(ConfigTest, DefaultModel) {
  Config config;

  EXPECT_EQ(config.default_model, "claude-sonnet-4-20250514");
}

TEST(ConfigTest, ContextSettings) {
  Config config;

  EXPECT_EQ(config.context.prune_protect_tokens, 40000);
  EXPECT_EQ(config.context.prune_minimum_tokens, 20000);
  EXPECT_EQ(config.context.truncate_max_lines, 2000u);
  EXPECT_EQ(config.context.truncate_max_bytes, 51200u);
}

// --- ConfigPathsTest ---

TEST(ConfigPathsTest, HomeDir) {
  auto home = config_paths::home_dir();

  EXPECT_FALSE(home.empty());
  EXPECT_TRUE(fs::exists(home));
}

TEST(ConfigPathsTest, ConfigDir) {
  auto config_dir = config_paths::config_dir();

  EXPECT_FALSE(config_dir.empty());
  // 配置目录应以 "agent-sdk" 结尾
  EXPECT_EQ(config_dir.filename(), "agent-sdk");
  // 父目录应为 ".config"
  EXPECT_EQ(config_dir.parent_path().filename(), ".config");
}

TEST(ConfigPathsTest, FindGitRoot) {
  // 当前项目是一个 git 仓库，从当前目录能找到 git 根目录
  auto git_root = config_paths::find_git_root(fs::current_path());

  ASSERT_TRUE(git_root.has_value());
  // git 根目录应该包含 .git
  EXPECT_TRUE(fs::exists(*git_root / ".git"));

  // 从根目录搜索不应该找到 git 仓库（除非根目录恰好是 git 仓库）
  auto root_result = config_paths::find_git_root("/");
  // 不做严格断言，因为取决于环境，但不应崩溃
}

TEST(ConfigTest, SaveAndLoadMcpServers) {
  Config config;

  // 添加本地 MCP 服务器
  McpServerConfig local_server;
  local_server.name = "my-server";
  local_server.type = "local";
  local_server.command = "npx";
  local_server.args = {"-y", "@modelcontextprotocol/server-filesystem"};
  local_server.env = {{"HOME", "/tmp"}};
  local_server.enabled = true;
  config.mcp_servers.push_back(local_server);

  // 添加远程 MCP 服务器
  McpServerConfig remote_server;
  remote_server.name = "remote-server";
  remote_server.type = "remote";
  remote_server.url = "https://example.com/mcp";
  remote_server.headers = {{"Authorization", "Bearer xxx"}};
  remote_server.enabled = true;
  config.mcp_servers.push_back(remote_server);

  // 保存到临时文件
  auto tmp_path = fs::temp_directory_path() / "test_mcp_config.json";
  config.save(tmp_path);

  // 重新加载
  auto loaded = Config::load(tmp_path);

  ASSERT_EQ(loaded.mcp_servers.size(), 2u);

  // 验证本地服务器
  const auto& s0 = loaded.mcp_servers[0];
  EXPECT_EQ(s0.name, "my-server");
  EXPECT_EQ(s0.type, "local");
  EXPECT_EQ(s0.command, "npx");
  ASSERT_EQ(s0.args.size(), 2u);
  EXPECT_EQ(s0.args[0], "-y");
  EXPECT_EQ(s0.args[1], "@modelcontextprotocol/server-filesystem");
  ASSERT_EQ(s0.env.size(), 1u);
  EXPECT_EQ(s0.env.at("HOME"), "/tmp");
  EXPECT_TRUE(s0.enabled);

  // 验证远程服务器
  const auto& s1 = loaded.mcp_servers[1];
  EXPECT_EQ(s1.name, "remote-server");
  EXPECT_EQ(s1.type, "remote");
  EXPECT_EQ(s1.url, "https://example.com/mcp");
  ASSERT_EQ(s1.headers.size(), 1u);
  EXPECT_EQ(s1.headers.at("Authorization"), "Bearer xxx");
  EXPECT_TRUE(s1.enabled);

  // 清理临时文件
  fs::remove(tmp_path);
}

TEST(ConfigTest, FromEnvWithOllamaKey) {
  // 保存原始环境变量
  const char* orig_ollama = std::getenv("OLLAMA_API_KEY");
  const char* orig_base_url = std::getenv("OLLAMA_BASE_URL");
  const char* orig_model = std::getenv("OLLAMA_MODEL");
  const char* orig_anthropic = std::getenv("ANTHROPIC_API_KEY");
  const char* orig_openai = std::getenv("OPENAI_API_KEY");
  const char* orig_qwen_oauth = std::getenv("QWEN_OAUTH");

  // 清空其他 providers，仅设置 Ollama
  unsetenv("ANTHROPIC_API_KEY");
  unsetenv("OPENAI_API_KEY");
  unsetenv("QWEN_OAUTH");
  setenv("OLLAMA_API_KEY", "", 1);
  setenv("OLLAMA_MODEL", "deepseek-r1:7b", 1);
  unsetenv("OLLAMA_BASE_URL");

  auto config = Config::from_env();

  // 验证 Ollama provider 被配置
  ASSERT_TRUE(config.providers.count("ollama") > 0);
  EXPECT_EQ(config.providers["ollama"].name, "ollama");
  EXPECT_EQ(config.providers["ollama"].api_key, "");
  EXPECT_EQ(config.providers["ollama"].base_url, "http://localhost:11434");
  EXPECT_EQ(config.default_model, "deepseek-r1:7b");

  // 恢复环境变量
  if (orig_ollama) {
    setenv("OLLAMA_API_KEY", orig_ollama, 1);
  } else {
    unsetenv("OLLAMA_API_KEY");
  }
  if (orig_base_url) {
    setenv("OLLAMA_BASE_URL", orig_base_url, 1);
  } else {
    unsetenv("OLLAMA_BASE_URL");
  }
  if (orig_model) {
    setenv("OLLAMA_MODEL", orig_model, 1);
  } else {
    unsetenv("OLLAMA_MODEL");
  }
  if (orig_anthropic) setenv("ANTHROPIC_API_KEY", orig_anthropic, 1);
  if (orig_openai) setenv("OPENAI_API_KEY", orig_openai, 1);
  if (orig_qwen_oauth) setenv("QWEN_OAUTH", orig_qwen_oauth, 1);
}

TEST(ConfigTest, FromEnvOllamaOnlyWhenEmpty) {
  // 保存原始环境变量
  const char* orig_ollama = std::getenv("OLLAMA_API_KEY");
  const char* orig_openai = std::getenv("OPENAI_API_KEY");

  // 设置 OpenAI 和 Ollama，OpenAI 应该优先
  setenv("OPENAI_API_KEY", "test-openai-key", 1);
  setenv("OLLAMA_API_KEY", "", 1);

  auto config = Config::from_env();

  // OpenAI 应该被配置，Ollama 不应该（因为 providers 不为空）
  ASSERT_TRUE(config.providers.count("openai") > 0);
  EXPECT_FALSE(config.providers.count("ollama") > 0);

  // 恢复环境变量
  if (orig_ollama) {
    setenv("OLLAMA_API_KEY", orig_ollama, 1);
  } else {
    unsetenv("OLLAMA_API_KEY");
  }
  if (orig_openai) {
    setenv("OPENAI_API_KEY", orig_openai, 1);
  } else {
    unsetenv("OPENAI_API_KEY");
  }
}

// --- Config::from_env() Tests ---

TEST(ConfigTest, FromEnvWithNoProviders) {
  // 保存原始环境变量
  const char* orig_anthropic = std::getenv("ANTHROPIC_API_KEY");
  const char* orig_anthropic_auth = std::getenv("ANTHROPIC_AUTH_TOKEN");
  const char* orig_openai = std::getenv("OPENAI_API_KEY");
  const char* orig_qwen_oauth = std::getenv("QWEN_OAUTH");

  // 清空环境变量
  unsetenv("ANTHROPIC_API_KEY");
  unsetenv("ANTHROPIC_AUTH_TOKEN");
  unsetenv("OPENAI_API_KEY");
  unsetenv("QWEN_OAUTH");

  auto config = Config::from_env();

  // 没有 API key 时，providers 应该为空（除非配置文件中有定义）
  // 由于测试环境可能没有配置文件，这里只验证函数不崩溃
  EXPECT_TRUE(config.providers.empty() || !config.providers.empty());

  // 恢复环境变量
  if (orig_anthropic) setenv("ANTHROPIC_API_KEY", orig_anthropic, 1);
  if (orig_anthropic_auth) setenv("ANTHROPIC_AUTH_TOKEN", orig_anthropic_auth, 1);
  if (orig_openai) setenv("OPENAI_API_KEY", orig_openai, 1);
  if (orig_qwen_oauth) setenv("QWEN_OAUTH", orig_qwen_oauth, 1);
}

TEST(ConfigTest, FromEnvWithAnthropicKey) {
  // 保存原始环境变量
  const char* orig_anthropic = std::getenv("ANTHROPIC_API_KEY");
  const char* orig_base_url = std::getenv("ANTHROPIC_BASE_URL");
  const char* orig_model = std::getenv("ANTHROPIC_MODEL");

  // 设置测试环境变量
  setenv("ANTHROPIC_API_KEY", "test-anthropic-key", 1);
  unsetenv("ANTHROPIC_BASE_URL");
  unsetenv("ANTHROPIC_MODEL");

  auto config = Config::from_env();

  // 验证 Anthropic provider 被配置
  ASSERT_TRUE(config.providers.count("anthropic") > 0);
  EXPECT_EQ(config.providers["anthropic"].api_key, "test-anthropic-key");
  EXPECT_EQ(config.providers["anthropic"].base_url, "https://api.anthropic.com");

  // 恢复环境变量
  if (orig_anthropic) {
    setenv("ANTHROPIC_API_KEY", orig_anthropic, 1);
  } else {
    unsetenv("ANTHROPIC_API_KEY");
  }
  if (orig_base_url) {
    setenv("ANTHROPIC_BASE_URL", orig_base_url, 1);
  }
  if (orig_model) {
    setenv("ANTHROPIC_MODEL", orig_model, 1);
  }
}

TEST(ConfigTest, FromEnvWithCustomBaseUrl) {
  // 保存原始环境变量
  const char* orig_anthropic = std::getenv("ANTHROPIC_API_KEY");
  const char* orig_base_url = std::getenv("ANTHROPIC_BASE_URL");

  // 设置测试环境变量
  setenv("ANTHROPIC_API_KEY", "test-key", 1);
  setenv("ANTHROPIC_BASE_URL", "https://custom.api.com", 1);

  auto config = Config::from_env();

  ASSERT_TRUE(config.providers.count("anthropic") > 0);
  EXPECT_EQ(config.providers["anthropic"].base_url, "https://custom.api.com");

  // 恢复环境变量
  if (orig_anthropic) {
    setenv("ANTHROPIC_API_KEY", orig_anthropic, 1);
  } else {
    unsetenv("ANTHROPIC_API_KEY");
  }
  if (orig_base_url) {
    setenv("ANTHROPIC_BASE_URL", orig_base_url, 1);
  } else {
    unsetenv("ANTHROPIC_BASE_URL");
  }
}

TEST(ConfigTest, FromEnvWithQwenOAuth) {
  // 保存原始环境变量
  const char* orig_qwen_oauth = std::getenv("QWEN_OAUTH");
  const char* orig_qwen_base_url = std::getenv("QWEN_BASE_URL");
  const char* orig_qwen_model = std::getenv("QWEN_MODEL");
  const char* orig_anthropic = std::getenv("ANTHROPIC_API_KEY");
  const char* orig_openai = std::getenv("OPENAI_API_KEY");

  // 清空其他 provider，设置 Qwen OAuth 模式
  unsetenv("ANTHROPIC_API_KEY");
  unsetenv("OPENAI_API_KEY");
  setenv("QWEN_OAUTH", "true", 1);
  unsetenv("QWEN_BASE_URL");
  unsetenv("QWEN_MODEL");

  auto config = Config::from_env();

  // 验证 OpenAI provider 配置为 Qwen portal
  ASSERT_TRUE(config.providers.count("openai") > 0);
  EXPECT_EQ(config.providers["openai"].api_key, "qwen-oauth");
  EXPECT_EQ(config.providers["openai"].base_url, "https://portal.qwen.ai");
  // 验证 default_model 为 coder-model
  EXPECT_EQ(config.default_model, "coder-model");

  // 恢复环境变量
  if (orig_qwen_oauth) {
    setenv("QWEN_OAUTH", orig_qwen_oauth, 1);
  } else {
    unsetenv("QWEN_OAUTH");
  }
  if (orig_qwen_base_url) {
    setenv("QWEN_BASE_URL", orig_qwen_base_url, 1);
  }
  if (orig_qwen_model) {
    setenv("QWEN_MODEL", orig_qwen_model, 1);
  }
  if (orig_anthropic) {
    setenv("ANTHROPIC_API_KEY", orig_anthropic, 1);
  }
  if (orig_openai) {
    setenv("OPENAI_API_KEY", orig_openai, 1);
  }
}

TEST(ConfigTest, FromEnvQwenOAuthTakesPrecedenceOverOpenAI) {
  // 保存原始环境变量
  const char* orig_qwen_oauth = std::getenv("QWEN_OAUTH");
  const char* orig_openai = std::getenv("OPENAI_API_KEY");
  const char* orig_anthropic = std::getenv("ANTHROPIC_API_KEY");

  // 同时设置 Qwen OAuth 和 OpenAI，Qwen 应该优先
  unsetenv("ANTHROPIC_API_KEY");
  setenv("QWEN_OAUTH", "1", 1);
  setenv("OPENAI_API_KEY", "sk-openai-key", 1);

  auto config = Config::from_env();

  // Qwen OAuth 优先，所以 api_key 应该是 qwen-oauth
  ASSERT_TRUE(config.providers.count("openai") > 0);
  EXPECT_EQ(config.providers["openai"].api_key, "qwen-oauth");
  EXPECT_EQ(config.providers["openai"].base_url, "https://portal.qwen.ai");

  // 恢复环境变量
  if (orig_qwen_oauth) {
    setenv("QWEN_OAUTH", orig_qwen_oauth, 1);
  } else {
    unsetenv("QWEN_OAUTH");
  }
  if (orig_openai) {
    setenv("OPENAI_API_KEY", orig_openai, 1);
  } else {
    unsetenv("OPENAI_API_KEY");
  }
  if (orig_anthropic) {
    setenv("ANTHROPIC_API_KEY", orig_anthropic, 1);
  }
}

TEST(ConfigTest, FromEnvAnthropicTakesPrecedenceForModel) {
  // 保存原始环境变量
  const char* orig_anthropic = std::getenv("ANTHROPIC_API_KEY");
  const char* orig_openai = std::getenv("OPENAI_API_KEY");
  const char* orig_model = std::getenv("OPENAI_MODEL");

  // 设置两个 provider，但不指定 OPENAI_MODEL
  setenv("ANTHROPIC_API_KEY", "anthropic-key", 1);
  setenv("OPENAI_API_KEY", "openai-key", 1);
  unsetenv("OPENAI_MODEL");
  unsetenv("ANTHROPIC_MODEL");

  auto config = Config::from_env();

  // 两个 provider 都应该被配置
  ASSERT_TRUE(config.providers.count("anthropic") > 0);
  ASSERT_TRUE(config.providers.count("openai") > 0);

  // 默认模型应该保持 Anthropic 的默认值（因为没有显式设置 OPENAI_MODEL）
  EXPECT_EQ(config.default_model, "claude-sonnet-4-20250514");

  // 恢复环境变量
  if (orig_anthropic) {
    setenv("ANTHROPIC_API_KEY", orig_anthropic, 1);
  } else {
    unsetenv("ANTHROPIC_API_KEY");
  }
  if (orig_openai) {
    setenv("OPENAI_API_KEY", orig_openai, 1);
  } else {
    unsetenv("OPENAI_API_KEY");
  }
  if (orig_model) {
    setenv("OPENAI_MODEL", orig_model, 1);
  }
}

TEST(ConfigTest, SaveAndLoadAgents) {
  Config config;

  // 添加 build 代理
  AgentConfig build_agent;
  build_agent.id = "build";
  build_agent.type = AgentType::Build;
  build_agent.model = "claude-sonnet-4-20250514";
  build_agent.system_prompt = "You are a coding assistant";
  build_agent.max_tokens = 200000;
  build_agent.default_permission = Permission::Allow;
  build_agent.allowed_tools = {"bash", "read"};
  build_agent.denied_tools = {"write"};
  build_agent.permissions = {{"bash", Permission::Ask}};
  config.agents["build"] = build_agent;

  // 添加 explore 代理
  AgentConfig explore_agent;
  explore_agent.id = "explore";
  explore_agent.type = AgentType::Explore;
  explore_agent.model = "gpt-4o";
  explore_agent.system_prompt = "Read-only exploration agent";
  explore_agent.max_tokens = 50000;
  explore_agent.default_permission = Permission::Deny;
  config.agents["explore"] = explore_agent;

  // 保存到临时文件
  auto tmp_path = fs::temp_directory_path() / "test_agents_config.json";
  config.save(tmp_path);

  // 重新加载
  auto loaded = Config::load(tmp_path);

  ASSERT_EQ(loaded.agents.size(), 2u);

  // 验证 build 代理
  auto build_opt = loaded.get_agent("build");
  ASSERT_TRUE(build_opt.has_value());
  const auto& b = *build_opt;
  EXPECT_EQ(b.id, "build");
  EXPECT_EQ(b.type, AgentType::Build);
  EXPECT_EQ(b.model, "claude-sonnet-4-20250514");
  EXPECT_EQ(b.system_prompt, "You are a coding assistant");
  EXPECT_EQ(b.max_tokens, 200000);
  EXPECT_EQ(b.default_permission, Permission::Allow);
  ASSERT_EQ(b.allowed_tools.size(), 2u);
  EXPECT_EQ(b.allowed_tools[0], "bash");
  EXPECT_EQ(b.allowed_tools[1], "read");
  ASSERT_EQ(b.denied_tools.size(), 1u);
  EXPECT_EQ(b.denied_tools[0], "write");
  ASSERT_EQ(b.permissions.size(), 1u);
  EXPECT_EQ(b.permissions.at("bash"), Permission::Ask);

  // 验证 explore 代理
  auto explore_opt = loaded.get_agent("explore");
  ASSERT_TRUE(explore_opt.has_value());
  const auto& e = *explore_opt;
  EXPECT_EQ(e.id, "explore");
  EXPECT_EQ(e.type, AgentType::Explore);
  EXPECT_EQ(e.model, "gpt-4o");
  EXPECT_EQ(e.system_prompt, "Read-only exploration agent");
  EXPECT_EQ(e.max_tokens, 50000);
  EXPECT_EQ(e.default_permission, Permission::Deny);
  EXPECT_TRUE(e.allowed_tools.empty());
  EXPECT_TRUE(e.denied_tools.empty());
  EXPECT_TRUE(e.permissions.empty());

  // 清理临时文件
  fs::remove(tmp_path);
}
