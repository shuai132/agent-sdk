#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "plugin/auth_provider.hpp"

namespace agent::plugin::qwen {

using json = nlohmann::json;

// OAuth token data
struct OAuthToken {
  std::string access_token;
  std::string refresh_token;
  std::string provider;    // e.g., "qwen-portal"
  int64_t expires_at = 0;  // Unix timestamp in milliseconds

  bool is_expired() const;
  bool needs_refresh() const;  // Returns true if expiring within 5 minutes

  json to_json() const;
  static OAuthToken from_json(const json& j);
};

// Device code response from OAuth server
struct DeviceCodeResponse {
  std::string device_code;
  std::string user_code;
  std::string verification_uri;
  std::string verification_uri_complete;  // Optional: URI with code pre-filled
  int expires_in = 0;                     // Seconds until device code expires
  int interval = 5;                       // Polling interval in seconds
};

// PKCE (Proof Key for Code Exchange) helper
struct PkceChallenge {
  std::string code_verifier;   // Random string (43-128 chars)
  std::string code_challenge;  // Base64URL(SHA256(code_verifier))

  // Generate a new PKCE challenge pair
  static PkceChallenge generate();
};

// Qwen Portal OAuth configuration
struct QwenPortalConfig {
  // Qwen OAuth endpoints (matching official Qwen CLI)
  static constexpr const char* BASE_URL = "https://chat.qwen.ai";
  static constexpr const char* DEVICE_CODE_URL = "https://chat.qwen.ai/api/v1/oauth2/device/code";
  static constexpr const char* TOKEN_URL = "https://chat.qwen.ai/api/v1/oauth2/token";

  // OAuth client configuration (matching official Qwen CLI)
  static constexpr const char* CLIENT_ID = "f0304373b74a44d2b584a3fb70ca9e56";
  static constexpr const char* SCOPE = "openid profile email model.completion";
  static constexpr const char* DEVICE_GRANT_TYPE = "urn:ietf:params:oauth:grant-type:device_code";

  // API key placeholder for OAuth (used in config)
  static constexpr const char* OAUTH_PLACEHOLDER = "qwen-oauth";

  // Provider identifier
  static constexpr const char* PROVIDER_ID = "qwen-portal";

  // Model IDs (for portal.qwen.ai API)
  static constexpr const char* CODER_MODEL = "coder-model";
  static constexpr const char* VISION_MODEL = "vision-model";
};

// OAuth authenticator for Qwen Portal
class QwenPortalAuth {
 public:
  QwenPortalAuth();
  ~QwenPortalAuth();

  // Perform device code OAuth authentication flow
  // 1. Request device code
  // 2. Display verification URL and user code to user
  // 3. Open browser (optional)
  // 4. Poll for token
  // Returns token on success, nullopt on failure/timeout
  std::future<std::optional<OAuthToken>> authenticate();

  // Refresh an existing token
  std::future<std::optional<OAuthToken>> refresh(const OAuthToken& token);

  // Get valid token (from cache, file, or trigger auth)
  // Automatically refreshes if expired
  std::optional<OAuthToken> get_valid_token();

  // Check if we have a valid (non-expired) token
  bool has_valid_token();

  // Load token from storage (agent-sdk storage or Qwen CLI)
  std::optional<OAuthToken> load_token() const;

  // Save token to storage
  void save_token(const OAuthToken& token) const;

  // Clear stored token
  void clear_token() const;

  // Check if Qwen CLI credentials exist
  bool has_qwen_cli_credentials() const;

  // Import credentials from Qwen CLI (~/.qwen/oauth_creds.json)
  std::optional<OAuthToken> import_from_qwen_cli() const;

  // Set callback for status updates (for UI display)
  using StatusCallback = std::function<void(const std::string& message)>;
  void set_status_callback(StatusCallback callback);

  // Set callback for user code display (required for device code flow)
  // Callback receives: verification_uri, user_code, verification_uri_complete (optional)
  using UserCodeCallback = std::function<void(const std::string& uri, const std::string& code, const std::string& uri_complete)>;
  void set_user_code_callback(UserCodeCallback callback);

 private:
  // Request device code from OAuth server
  std::optional<DeviceCodeResponse> request_device_code();

  // Poll for token after user authorization
  std::optional<OAuthToken> poll_for_token(const DeviceCodeResponse& device_code);

  // Exchange refresh token for new access token
  std::optional<OAuthToken> do_refresh(const std::string& refresh_token);

  // Open URL in system browser
  bool open_browser(const std::string& url) const;

  // Storage paths
  std::filesystem::path token_storage_path() const;
  std::filesystem::path qwen_cli_credentials_path() const;

  // HTTP helper
  std::optional<json> http_post(const std::string& url, const std::map<std::string, std::string>& form_data);

  StatusCallback status_callback_;
  UserCodeCallback user_code_callback_;
  mutable std::optional<OAuthToken> cached_token_;
  mutable std::string current_code_verifier_;  // PKCE code_verifier for current auth flow
};

// Get or create the shared authenticator instance
QwenPortalAuth& qwen_portal_auth();

// Auth provider implementation for plugin system
class QwenAuthProvider : public AuthProvider {
 public:
  std::string scheme() const override {
    return QwenPortalConfig::OAUTH_PLACEHOLDER;
  }

  std::optional<std::string> get_auth_header() override;

  bool can_handle(const std::string& api_key) const override {
    return api_key == QwenPortalConfig::OAUTH_PLACEHOLDER;
  }
};

// Register Qwen OAuth plugin with the auth provider registry
// Call this at startup to enable Qwen OAuth support
void register_qwen_plugin();

}  // namespace agent::plugin::qwen
