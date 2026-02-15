#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/config.hpp"
#include "mcp/client.hpp"
#include "mcp/transport.hpp"

#ifdef AGENT_PLUGIN_QWEN
#include "plugin/qwen/qwen_oauth.hpp"
#endif

using namespace agent;
using namespace agent::mcp;

#ifdef AGENT_PLUGIN_QWEN
using namespace agent::plugin::qwen;
#endif

// ============================================================
// JsonRpcTest — JSON-RPC 消息序列化
// ============================================================

TEST(JsonRpcTest, RequestSerialization) {
  JsonRpcRequest req;
  req.method = "initialize";
  req.id = 42;
  req.params = json{{"protocolVersion", "2024-11-05"}};

  auto j = req.to_json();

  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["method"], "initialize");
  EXPECT_EQ(j["id"], 42);
  EXPECT_TRUE(j.contains("params"));
  EXPECT_EQ(j["params"]["protocolVersion"], "2024-11-05");
}

TEST(JsonRpcTest, RequestSerializationEmptyParams) {
  JsonRpcRequest req;
  req.method = "tools/list";
  req.id = 1;
  // params 默认为空 object

  auto j = req.to_json();

  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["method"], "tools/list");
  EXPECT_EQ(j["id"], 1);
  // 空 params 不应被序列化
  EXPECT_FALSE(j.contains("params"));
}

TEST(JsonRpcTest, ResponseFromJson) {
  json j = {
      {"jsonrpc", "2.0"},
      {"id", 10},
      {"result", {{"capabilities", {{"tools", json::object()}}}}},
  };

  auto resp = JsonRpcResponse::from_json(j);

  EXPECT_EQ(resp.id, 10);
  EXPECT_TRUE(resp.ok());
  EXPECT_TRUE(resp.result.has_value());
  EXPECT_FALSE(resp.error.has_value());
  EXPECT_TRUE(resp.result->contains("capabilities"));
}

TEST(JsonRpcTest, ResponseFromJsonNullId) {
  json j = {
      {"jsonrpc", "2.0"},
      {"id", nullptr},
      {"result", "ok"},
  };

  auto resp = JsonRpcResponse::from_json(j);

  // null id 应保持默认值 0
  EXPECT_EQ(resp.id, 0);
  EXPECT_TRUE(resp.ok());
}

TEST(JsonRpcTest, ResponseErrorMessage) {
  // 带有 message 字段的错误
  json j = {
      {"jsonrpc", "2.0"},
      {"id", 5},
      {"error", {{"code", -32601}, {"message", "Method not found"}}},
  };

  auto resp = JsonRpcResponse::from_json(j);

  EXPECT_FALSE(resp.ok());
  EXPECT_TRUE(resp.error.has_value());
  EXPECT_EQ(resp.error_message(), "Method not found");
}

TEST(JsonRpcTest, ResponseErrorMessageWithoutMessageField) {
  // 没有 message 字段的错误 — 应 dump 整个 error 对象
  json j = {
      {"jsonrpc", "2.0"},
      {"id", 6},
      {"error", {{"code", -32000}}},
  };

  auto resp = JsonRpcResponse::from_json(j);

  EXPECT_FALSE(resp.ok());
  std::string msg = resp.error_message();
  EXPECT_FALSE(msg.empty());
  // 应包含序列化后的 JSON
  EXPECT_NE(msg.find("-32000"), std::string::npos);
}

TEST(JsonRpcTest, ResponseErrorMessageWhenNoError) {
  json j = {
      {"jsonrpc", "2.0"},
      {"id", 7},
      {"result", json::object()},
  };

  auto resp = JsonRpcResponse::from_json(j);

  EXPECT_TRUE(resp.ok());
  EXPECT_EQ(resp.error_message(), "");
}

TEST(JsonRpcTest, NotificationSerialization) {
  JsonRpcNotification notif;
  notif.method = "notifications/initialized";

  auto j = notif.to_json();

  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["method"], "notifications/initialized");
  // 通知消息不应包含 id 字段
  EXPECT_FALSE(j.contains("id"));
  // 空 params 不应被序列化
  EXPECT_FALSE(j.contains("params"));
}

TEST(JsonRpcTest, NotificationSerializationWithParams) {
  JsonRpcNotification notif;
  notif.method = "notifications/tools/list_changed";
  notif.params = json{{"reason", "updated"}};

  auto j = notif.to_json();

  EXPECT_EQ(j["jsonrpc"], "2.0");
  EXPECT_EQ(j["method"], "notifications/tools/list_changed");
  EXPECT_FALSE(j.contains("id"));
  EXPECT_TRUE(j.contains("params"));
  EXPECT_EQ(j["params"]["reason"], "updated");
}

// ============================================================
// TransportStateTest — 传输状态
// ============================================================

TEST(TransportStateTest, ToString) {
  EXPECT_EQ(to_string(TransportState::Disconnected), "Disconnected");
  EXPECT_EQ(to_string(TransportState::Connecting), "Connecting");
  EXPECT_EQ(to_string(TransportState::Connected), "Connected");
  EXPECT_EQ(to_string(TransportState::Failed), "Failed");
}

// ============================================================
// ClientStateTest — 客户端状态
// ============================================================

TEST(ClientStateTest, ToString) {
  EXPECT_EQ(to_string(ClientState::Disconnected), "Disconnected");
  EXPECT_EQ(to_string(ClientState::Connecting), "Connecting");
  EXPECT_EQ(to_string(ClientState::Initializing), "Initializing");
  EXPECT_EQ(to_string(ClientState::Ready), "Ready");
  EXPECT_EQ(to_string(ClientState::Failed), "Failed");
}

// ============================================================
// McpToolBridgeTest — 工具桥接
// ============================================================

TEST(McpToolBridgeTest, ParameterConversion) {
  // 构造一个 McpClient（type 未知会导致 state_ = Failed，但不影响桥接测试）
  McpServerConfig config;
  config.name = "test-server";
  config.type = "local";
  config.command = "/nonexistent";

  auto client = std::make_shared<McpClient>(config);

  // 构造 McpToolInfo，模拟典型的 JSON Schema
  McpToolInfo tool_info;
  tool_info.name = "read_file";
  tool_info.description = "Read a file from disk";
  tool_info.input_schema = json{
      {"type", "object"},
      {"properties",
       {{"path", {{"type", "string"}, {"description", "File path to read"}}},
        {"encoding",
         {{"type", "string"}, {"description", "File encoding"}, {"default", "utf-8"}, {"enum", json::array({"utf-8", "ascii", "latin1"})}}}}},
      {"required", json::array({"path"})},
  };

  McpToolBridge bridge(client, tool_info);

  // 验证 id 格式：mcp_<server>_<tool>
  EXPECT_EQ(bridge.id(), "mcp_test-server_read_file");

  // 验证参数转换
  auto params = bridge.parameters();
  ASSERT_EQ(params.size(), 2);

  // 查找各个参数（顺序可能因 JSON 对象迭代顺序而异）
  const ParameterSchema* path_param = nullptr;
  const ParameterSchema* encoding_param = nullptr;
  for (const auto& p : params) {
    if (p.name == "path") path_param = &p;
    if (p.name == "encoding") encoding_param = &p;
  }

  ASSERT_NE(path_param, nullptr);
  EXPECT_EQ(path_param->type, "string");
  EXPECT_EQ(path_param->description, "File path to read");
  EXPECT_TRUE(path_param->required);
  EXPECT_FALSE(path_param->default_value.has_value());
  EXPECT_FALSE(path_param->enum_values.has_value());

  ASSERT_NE(encoding_param, nullptr);
  EXPECT_EQ(encoding_param->type, "string");
  EXPECT_EQ(encoding_param->description, "File encoding");
  EXPECT_FALSE(encoding_param->required);
  ASSERT_TRUE(encoding_param->default_value.has_value());
  EXPECT_EQ(encoding_param->default_value.value(), "utf-8");
  ASSERT_TRUE(encoding_param->enum_values.has_value());
  EXPECT_EQ(encoding_param->enum_values->size(), 3);
  EXPECT_EQ((*encoding_param->enum_values)[0], "utf-8");
  EXPECT_EQ((*encoding_param->enum_values)[1], "ascii");
  EXPECT_EQ((*encoding_param->enum_values)[2], "latin1");
}

TEST(McpToolBridgeTest, EmptySchema) {
  McpServerConfig config;
  config.name = "srv";
  config.type = "local";
  config.command = "/nonexistent";

  auto client = std::make_shared<McpClient>(config);

  McpToolInfo tool_info;
  tool_info.name = "noop";
  tool_info.description = "A tool with no parameters";
  tool_info.input_schema = json{{"type", "object"}, {"properties", json::object()}};

  McpToolBridge bridge(client, tool_info);

  auto params = bridge.parameters();
  EXPECT_TRUE(params.empty());
}

// ============================================================
// McpManagerTest — 管理器
// ============================================================

TEST(McpManagerTest, Singleton) {
  auto& mgr1 = McpManager::instance();
  auto& mgr2 = McpManager::instance();

  EXPECT_EQ(&mgr1, &mgr2);
}

TEST(McpManagerTest, InitializeWithEmptyConfig) {
  auto& mgr = McpManager::instance();

  // 先断开并清理之前的状态
  mgr.disconnect_all();

  // 空配置初始化
  std::vector<McpServerConfig> empty_servers;
  mgr.initialize(empty_servers);

  // 没有客户端
  auto clients = mgr.all_clients();
  EXPECT_TRUE(clients.empty());

  // 按名称查找应返回 nullptr
  auto client = mgr.get_client("nonexistent");
  EXPECT_EQ(client, nullptr);

  EXPECT_EQ(mgr.tool_count(), 0);

  mgr.disconnect_all();
}

// ============================================================
// QwenOAuthTest — Qwen Portal OAuth
// ============================================================

#ifdef AGENT_PLUGIN_QWEN

TEST(QwenOAuthTest, Singleton) {
  auto& auth1 = qwen_portal_auth();
  auto& auth2 = qwen_portal_auth();

  EXPECT_EQ(&auth1, &auth2);
}

TEST(QwenOAuthTest, InitialState) {
  auto& auth = qwen_portal_auth();

  // 尝试加载 token 并打印信息
  auto token = auth.load_token();
  if (token) {
    std::cout << "\n=== Qwen OAuth Token ===" << std::endl;
    std::cout << "Provider: " << token->provider << std::endl;
    std::cout << "Access Token: " << token->access_token.substr(0, 20) << "..." << std::endl;
    std::cout << "Refresh Token: " << token->refresh_token.substr(0, 20) << "..." << std::endl;
    std::cout << "Expires At: " << token->expires_at << " ms" << std::endl;

    // 转换为可读时间
    auto expires_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(token->expires_at));
    auto time_t_val = std::chrono::system_clock::to_time_t(expires_time);
    std::cout << "Expires At (local): " << std::ctime(&time_t_val);

    std::cout << "Is Expired: " << (token->is_expired() ? "Yes" : "No") << std::endl;
    std::cout << "Needs Refresh: " << (token->needs_refresh() ? "Yes" : "No") << std::endl;
    std::cout << "========================\n" << std::endl;
  } else {
    std::cout << "\n[QwenOAuthTest] No token found (not logged in or no credentials)\n" << std::endl;
  }

  // 测试 has_valid_token 接口
  bool has_token = auth.has_valid_token();
  std::cout << "[QwenOAuthTest] has_valid_token(): " << (has_token ? "true" : "false") << std::endl;
}

TEST(QwenOAuthTest, TokenExpiry) {
  using namespace std::chrono;

  OAuthToken token;
  token.access_token = "test_access_token";
  token.refresh_token = "test_refresh_token";
  // 设置 1 小时后过期
  auto now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  token.expires_at = now_ms + 3600 * 1000;  // 1 hour from now

  // 新 token 应该是有效的（未过期）
  EXPECT_FALSE(token.is_expired());

  // 过期的 token 应该无效
  token.expires_at = now_ms - 3600 * 1000;  // 1 hour ago
  EXPECT_TRUE(token.is_expired());
}

TEST(QwenOAuthTest, TokenNearExpiry) {
  using namespace std::chrono;

  OAuthToken token;
  token.access_token = "test_access_token";
  token.refresh_token = "test_refresh_token";

  auto now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

  // 3 分钟后过期，需要刷新（在 5 分钟窗口内）
  token.expires_at = now_ms + 3 * 60 * 1000;
  EXPECT_TRUE(token.needs_refresh());

  // 1 小时后过期，不需要刷新
  token.expires_at = now_ms + 3600 * 1000;
  EXPECT_FALSE(token.needs_refresh());
}

TEST(QwenOAuthTest, DeviceCodeResponseParsing) {
  // 模拟服务器返回的设备码响应
  json response = {{"device_code", "dev_code_123"},
                   {"user_code", "ABCD-1234"},
                   {"verification_uri", "https://example.com/verify"},
                   {"verification_uri_complete", "https://example.com/verify?code=ABCD-1234"},
                   {"expires_in", 900},
                   {"interval", 5}};

  DeviceCodeResponse dcr;
  dcr.device_code = response["device_code"];
  dcr.user_code = response["user_code"];
  dcr.verification_uri = response["verification_uri"];
  dcr.verification_uri_complete = response.value("verification_uri_complete", "");
  dcr.expires_in = response["expires_in"];
  dcr.interval = response.value("interval", 5);

  EXPECT_EQ(dcr.device_code, "dev_code_123");
  EXPECT_EQ(dcr.user_code, "ABCD-1234");
  EXPECT_EQ(dcr.verification_uri, "https://example.com/verify");
  EXPECT_EQ(dcr.verification_uri_complete, "https://example.com/verify?code=ABCD-1234");
  EXPECT_EQ(dcr.expires_in, 900);
  EXPECT_EQ(dcr.interval, 5);
}

#endif  // AGENT_PLUGIN_QWEN

TEST(McpManagerTest, InitializeWithDisabledServer) {
  auto& mgr = McpManager::instance();

  // 先清理
  mgr.disconnect_all();

  // 创建一个禁用的服务器配置
  McpServerConfig disabled_config;
  disabled_config.name = "disabled-server";
  disabled_config.type = "local";
  disabled_config.command = "/nonexistent";
  disabled_config.enabled = false;

  std::vector<McpServerConfig> servers = {disabled_config};
  mgr.initialize(servers);

  // 禁用的服务器不应被注册
  auto clients = mgr.all_clients();
  EXPECT_TRUE(clients.empty());

  auto client = mgr.get_client("disabled-server");
  EXPECT_EQ(client, nullptr);

  mgr.disconnect_all();
}

TEST(McpManagerTest, InitializeWithEnabledServer) {
  auto& mgr = McpManager::instance();

  // 先清理
  mgr.disconnect_all();

  // 创建一个启用的服务器配置（命令不存在，但 initialize 不会连接）
  McpServerConfig config;
  config.name = "test-server";
  config.type = "local";
  config.command = "/nonexistent";
  config.enabled = true;

  std::vector<McpServerConfig> servers = {config};
  mgr.initialize(servers);

  // 启用的服务器应被注册为客户端
  auto clients = mgr.all_clients();
  EXPECT_EQ(clients.size(), 1);

  auto client = mgr.get_client("test-server");
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client->server_name(), "test-server");

  // 尚未连接，状态应为 Disconnected
  EXPECT_EQ(client->state(), ClientState::Disconnected);
  EXPECT_FALSE(client->is_ready());

  mgr.disconnect_all();
}
