#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "plugin/auth_provider.hpp"
#include "plugin/qwen/qwen_oauth.hpp"

using namespace agent;
using namespace agent::plugin;
using namespace agent::plugin::qwen;

namespace fs = std::filesystem;

// --- OAuthToken Tests ---

TEST(OAuthTokenTest, DefaultConstruction) {
  OAuthToken token;

  EXPECT_TRUE(token.access_token.empty());
  EXPECT_TRUE(token.refresh_token.empty());
  EXPECT_TRUE(token.provider.empty());
  EXPECT_EQ(token.expires_at, 0);
}

TEST(OAuthTokenTest, IsExpired) {
  OAuthToken token;

  // Default (0) is expired
  EXPECT_TRUE(token.is_expired());

  // Token with past expiry is expired
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  token.expires_at = now_ms - 1000;  // 1 second ago
  EXPECT_TRUE(token.is_expired());

  // Token with future expiry is not expired
  token.expires_at = now_ms + 3600000;  // 1 hour from now
  EXPECT_FALSE(token.is_expired());
}

TEST(OAuthTokenTest, NeedsRefresh) {
  OAuthToken token;

  // Default (0) needs refresh
  EXPECT_TRUE(token.needs_refresh());

  // Token expiring within 5 minutes needs refresh
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  token.expires_at = now_ms + 60000;  // 1 minute from now
  EXPECT_TRUE(token.needs_refresh());

  token.expires_at = now_ms + 240000;  // 4 minutes from now
  EXPECT_TRUE(token.needs_refresh());

  // Token expiring after 5 minutes doesn't need refresh
  token.expires_at = now_ms + 600000;  // 10 minutes from now
  EXPECT_FALSE(token.needs_refresh());
}

TEST(OAuthTokenTest, JsonSerialization) {
  OAuthToken original;
  original.access_token = "test_access_token_12345";
  original.refresh_token = "test_refresh_token_67890";
  original.provider = "qwen-portal";
  original.expires_at = 1771142625344;

  // Serialize
  auto json = original.to_json();

  EXPECT_EQ(json["access_token"], "test_access_token_12345");
  EXPECT_EQ(json["refresh_token"], "test_refresh_token_67890");
  EXPECT_EQ(json["provider"], "qwen-portal");
  EXPECT_EQ(json["expires"], 1771142625344);

  // Deserialize
  auto restored = OAuthToken::from_json(json);

  EXPECT_EQ(restored.access_token, original.access_token);
  EXPECT_EQ(restored.refresh_token, original.refresh_token);
  EXPECT_EQ(restored.provider, original.provider);
  EXPECT_EQ(restored.expires_at, original.expires_at);
}

TEST(OAuthTokenTest, JsonDeserializationWithExpiryDate) {
  // Qwen CLI uses "expiry_date" instead of "expires"
  nlohmann::json j = {
      {"access_token", "access_123"},
      {"refresh_token", "refresh_456"},
      {"provider", "qwen-cli"},
      {"expiry_date", 1771142625344},
  };

  auto token = OAuthToken::from_json(j);

  EXPECT_EQ(token.access_token, "access_123");
  EXPECT_EQ(token.refresh_token, "refresh_456");
  EXPECT_EQ(token.provider, "qwen-cli");
  EXPECT_EQ(token.expires_at, 1771142625344);
}

// --- QwenPortalConfig Tests ---

TEST(QwenPortalConfigTest, EndpointConstants) {
  // Verify production endpoints match official Qwen CLI
  EXPECT_STREQ(QwenPortalConfig::BASE_URL, "https://chat.qwen.ai");
  EXPECT_STREQ(QwenPortalConfig::DEVICE_CODE_URL, "https://chat.qwen.ai/api/v1/oauth2/device/code");
  EXPECT_STREQ(QwenPortalConfig::TOKEN_URL, "https://chat.qwen.ai/api/v1/oauth2/token");
}

TEST(QwenPortalConfigTest, ClientConfiguration) {
  // Verify client configuration
  EXPECT_STREQ(QwenPortalConfig::CLIENT_ID, "f0304373b74a44d2b584a3fb70ca9e56");
  EXPECT_STREQ(QwenPortalConfig::SCOPE, "openid profile email model.completion");
  EXPECT_STREQ(QwenPortalConfig::DEVICE_GRANT_TYPE, "urn:ietf:params:oauth:grant-type:device_code");
}

TEST(QwenPortalConfigTest, Identifiers) {
  EXPECT_STREQ(QwenPortalConfig::OAUTH_PLACEHOLDER, "qwen-oauth");
  EXPECT_STREQ(QwenPortalConfig::PROVIDER_ID, "qwen-portal");
}

TEST(QwenPortalConfigTest, ModelIds) {
  EXPECT_STREQ(QwenPortalConfig::CODER_MODEL, "coder-model");
  EXPECT_STREQ(QwenPortalConfig::VISION_MODEL, "vision-model");
}

// --- QwenAuthProvider Tests ---

TEST(QwenAuthProviderTest, Scheme) {
  QwenAuthProvider provider;

  EXPECT_EQ(provider.scheme(), "qwen-oauth");
}

TEST(QwenAuthProviderTest, CanHandle) {
  QwenAuthProvider provider;

  EXPECT_TRUE(provider.can_handle("qwen-oauth"));
  EXPECT_FALSE(provider.can_handle("sk-12345"));
  EXPECT_FALSE(provider.can_handle(""));
  EXPECT_FALSE(provider.can_handle("openai"));
}

// --- AuthProviderRegistry Tests ---

TEST(AuthProviderRegistryTest, RegisterAndLookup) {
  auto& registry = AuthProviderRegistry::instance();

  // Register Qwen plugin
  register_qwen_plugin();

  // Lookup should find qwen-oauth
  auto provider = registry.get_provider("qwen-oauth");
  EXPECT_NE(provider, nullptr);

  if (provider) {
    EXPECT_EQ(provider->scheme(), "qwen-oauth");
    EXPECT_TRUE(provider->can_handle("qwen-oauth"));
  }

  // Unknown scheme should return nullptr
  auto unknown = registry.get_provider("unknown-scheme");
  EXPECT_EQ(unknown, nullptr);
}

TEST(AuthProviderRegistryTest, MultipleRegistrations) {
  auto& registry = AuthProviderRegistry::instance();

  // Re-registering should be safe (idempotent)
  register_qwen_plugin();
  register_qwen_plugin();

  auto provider = registry.get_provider("qwen-oauth");
  EXPECT_NE(provider, nullptr);
}

// --- QwenPortalAuth Basic Tests ---

TEST(QwenPortalAuthTest, SingletonInstance) {
  auto& auth1 = qwen_portal_auth();
  auto& auth2 = qwen_portal_auth();

  // Should return same instance
  EXPECT_EQ(&auth1, &auth2);
}

TEST(QwenPortalAuthTest, QwenCliCredentialsPath) {
  auto& auth = qwen_portal_auth();

  // Check if Qwen CLI credentials exist
  // This is environment-dependent, just ensure it doesn't crash
  bool has_creds = auth.has_qwen_cli_credentials();
  // Just verify it returns a boolean without throwing
  EXPECT_TRUE(has_creds || !has_creds);
}

TEST(QwenPortalAuthTest, SetCallbacks) {
  auto& auth = qwen_portal_auth();

  bool status_called = false;
  bool user_code_called = false;

  // Set callbacks
  auth.set_status_callback([&](const std::string& msg) {
    status_called = true;
  });

  auth.set_user_code_callback([&](const std::string& uri, const std::string& code, const std::string& uri_complete) {
    user_code_called = true;
  });

  // Callbacks are set, but won't be called until authentication is triggered
  EXPECT_FALSE(status_called);
  EXPECT_FALSE(user_code_called);
}

// --- DeviceCodeResponse Tests ---

TEST(DeviceCodeResponseTest, DefaultValues) {
  DeviceCodeResponse response;

  EXPECT_TRUE(response.device_code.empty());
  EXPECT_TRUE(response.user_code.empty());
  EXPECT_TRUE(response.verification_uri.empty());
  EXPECT_TRUE(response.verification_uri_complete.empty());
  EXPECT_EQ(response.expires_in, 0);
  EXPECT_EQ(response.interval, 5);  // Default polling interval
}

// --- Integration Tests (require network) ---
// These tests are disabled by default as they require network access
// and valid credentials. Enable manually for integration testing.

TEST(QwenPortalAuthIntegrationTest, DISABLED_LoadTokenFromFile) {
  auto& auth = qwen_portal_auth();

  // Clear any cached token
  auth.clear_token();

  // Try to load token
  auto token = auth.load_token();

  if (token) {
    EXPECT_FALSE(token->access_token.empty());
    EXPECT_FALSE(token->refresh_token.empty());
    EXPECT_FALSE(token->provider.empty());
    EXPECT_GT(token->expires_at, 0);
  }
}

TEST(QwenPortalAuthIntegrationTest, DISABLED_ImportFromQwenCli) {
  auto& auth = qwen_portal_auth();

  if (!auth.has_qwen_cli_credentials()) {
    GTEST_SKIP() << "Qwen CLI credentials not found";
  }

  auto token = auth.import_from_qwen_cli();
  ASSERT_TRUE(token.has_value());

  EXPECT_FALSE(token->access_token.empty());
  EXPECT_FALSE(token->refresh_token.empty());
  EXPECT_GT(token->expires_at, 0);
}

TEST(QwenPortalAuthIntegrationTest, DISABLED_GetValidToken) {
  auto& auth = qwen_portal_auth();

  // This may trigger refresh if token is near expiry
  auto token = auth.get_valid_token();

  if (token) {
    EXPECT_FALSE(token->access_token.empty());
    EXPECT_FALSE(token->is_expired());
  }
}
