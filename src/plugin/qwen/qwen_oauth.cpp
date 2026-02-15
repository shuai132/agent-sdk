#include "plugin/qwen/qwen_oauth.hpp"

#include <openssl/sha.h>
#include <spdlog/spdlog.h>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "core/config.hpp"

namespace agent::plugin::qwen {

namespace fs = std::filesystem;

// ============================================================
// OAuthToken
// ============================================================

bool OAuthToken::is_expired() const {
  auto now = std::chrono::system_clock::now();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  return expires_at <= now_ms;
}

bool OAuthToken::needs_refresh() const {
  auto now = std::chrono::system_clock::now();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  // Refresh if expiring within 5 minutes
  return expires_at <= (now_ms + 5 * 60 * 1000);
}

json OAuthToken::to_json() const {
  return json{
      {"type", "oauth"}, {"provider", provider}, {"access", access_token}, {"refresh", refresh_token}, {"expires", expires_at},
  };
}

OAuthToken OAuthToken::from_json(const json& j) {
  OAuthToken token;
  token.provider = j.value("provider", "qwen-portal");
  token.access_token = j.value("access", j.value("access_token", ""));
  token.refresh_token = j.value("refresh", j.value("refresh_token", ""));
  token.expires_at = j.value("expires", j.value("expiry_date", int64_t(0)));
  return token;
}

// ============================================================
// PKCE Helper
// ============================================================

PkceChallenge PkceChallenge::generate() {
  PkceChallenge challenge;

  // Generate random code_verifier (43-128 characters from A-Z, a-z, 0-9, "-", ".", "_", "~")
  static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

  challenge.code_verifier.reserve(64);
  for (int i = 0; i < 64; i++) {
    challenge.code_verifier += charset[dist(gen)];
  }

  // Calculate code_challenge = base64url(sha256(code_verifier))
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(challenge.code_verifier.c_str()), challenge.code_verifier.size(), hash);

  // Base64URL encode (no padding)
  static const char base64url_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  challenge.code_challenge.reserve(43);
  int i = 0;
  for (i = 0; i + 2 < SHA256_DIGEST_LENGTH; i += 3) {
    uint32_t triple = (hash[i] << 16) | (hash[i + 1] << 8) | hash[i + 2];
    challenge.code_challenge += base64url_table[(triple >> 18) & 0x3F];
    challenge.code_challenge += base64url_table[(triple >> 12) & 0x3F];
    challenge.code_challenge += base64url_table[(triple >> 6) & 0x3F];
    challenge.code_challenge += base64url_table[triple & 0x3F];
  }
  // Handle remaining bytes (SHA256 is 32 bytes, so 2 remaining)
  if (i < SHA256_DIGEST_LENGTH) {
    uint32_t triple = hash[i] << 16;
    if (i + 1 < SHA256_DIGEST_LENGTH) {
      triple |= hash[i + 1] << 8;
    }
    challenge.code_challenge += base64url_table[(triple >> 18) & 0x3F];
    challenge.code_challenge += base64url_table[(triple >> 12) & 0x3F];
    if (i + 1 < SHA256_DIGEST_LENGTH) {
      challenge.code_challenge += base64url_table[(triple >> 6) & 0x3F];
    }
  }

  return challenge;
}

// ============================================================
// HTTP Helper (synchronous, for OAuth flows)
// ============================================================

namespace {

// URL encode a string
std::string url_encode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped << c;
    } else {
      escaped << std::uppercase;
      escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
      escaped << std::nouppercase;
    }
  }

  return escaped.str();
}

// Build form-urlencoded body
std::string build_form_body(const std::map<std::string, std::string>& params) {
  std::ostringstream body;
  bool first = true;
  for (const auto& [key, value] : params) {
    if (!first) body << "&";
    body << url_encode(key) << "=" << url_encode(value);
    first = false;
  }
  return body.str();
}

// Parse URL into host, port, path
struct UrlParts {
  std::string host;
  std::string port;
  std::string path;
  bool is_https;
};

std::optional<UrlParts> parse_url(const std::string& url) {
  UrlParts parts;

  size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return std::nullopt;

  std::string scheme = url.substr(0, scheme_end);
  parts.is_https = (scheme == "https");
  parts.port = parts.is_https ? "443" : "80";

  size_t host_start = scheme_end + 3;
  size_t path_start = url.find('/', host_start);

  std::string host_port;
  if (path_start == std::string::npos) {
    host_port = url.substr(host_start);
    parts.path = "/";
  } else {
    host_port = url.substr(host_start, path_start - host_start);
    parts.path = url.substr(path_start);
  }

  size_t colon_pos = host_port.find(':');
  if (colon_pos != std::string::npos) {
    parts.host = host_port.substr(0, colon_pos);
    parts.port = host_port.substr(colon_pos + 1);
  } else {
    parts.host = host_port;
  }

  return parts;
}

// Synchronous HTTP POST with SSL
std::optional<std::pair<int, std::string>> http_post_sync(const std::string& url, const std::string& body,
                                                          const std::string& content_type = "application/x-www-form-urlencoded") {
  auto url_parts = parse_url(url);
  if (!url_parts) {
    spdlog::error("[QwenOAuth] Failed to parse URL: {}", url);
    return std::nullopt;
  }

  try {
    asio::io_context io_ctx;
    asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();

    asio::ip::tcp::resolver resolver(io_ctx);
    auto endpoints = resolver.resolve(url_parts->host, url_parts->port);

    asio::ssl::stream<asio::ip::tcp::socket> socket(io_ctx, ssl_ctx);

    // Set SNI hostname
    SSL_set_tlsext_host_name(socket.native_handle(), url_parts->host.c_str());

    asio::connect(socket.lowest_layer(), endpoints);
    socket.handshake(asio::ssl::stream_base::client);

    // Build HTTP request
    std::ostringstream request;
    request << "POST " << url_parts->path << " HTTP/1.1\r\n";
    request << "Host: " << url_parts->host << "\r\n";
    request << "Content-Type: " << content_type << "\r\n";
    request << "Content-Length: " << body.size() << "\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";
    request << body;

    std::string request_str = request.str();
    asio::write(socket, asio::buffer(request_str));

    // Read response
    asio::streambuf response_buf;
    asio::error_code ec;
    asio::read(socket, response_buf, ec);

    std::string response_str((std::istreambuf_iterator<char>(&response_buf)), std::istreambuf_iterator<char>());

    // Parse status code
    int status_code = 0;
    size_t status_start = response_str.find(' ');
    if (status_start != std::string::npos) {
      status_code = std::stoi(response_str.substr(status_start + 1, 3));
    }

    // Find body (after \r\n\r\n)
    size_t body_start = response_str.find("\r\n\r\n");
    std::string response_body;
    if (body_start != std::string::npos) {
      response_body = response_str.substr(body_start + 4);
    }

    return std::make_pair(status_code, response_body);
  } catch (const std::exception& e) {
    spdlog::error("[QwenOAuth] HTTP POST failed: {}", e.what());
    return std::nullopt;
  }
}

}  // namespace

// ============================================================
// QwenPortalAuth
// ============================================================

QwenPortalAuth::QwenPortalAuth() = default;
QwenPortalAuth::~QwenPortalAuth() = default;

void QwenPortalAuth::set_status_callback(StatusCallback callback) {
  status_callback_ = std::move(callback);
}

void QwenPortalAuth::set_user_code_callback(UserCodeCallback callback) {
  user_code_callback_ = std::move(callback);
}

fs::path QwenPortalAuth::token_storage_path() const {
  return config_paths::config_dir() / "qwen-oauth.json";
}

fs::path QwenPortalAuth::qwen_cli_credentials_path() const {
  return config_paths::home_dir() / ".qwen" / "oauth_creds.json";
}

bool QwenPortalAuth::has_qwen_cli_credentials() const {
  return fs::exists(qwen_cli_credentials_path());
}

std::optional<OAuthToken> QwenPortalAuth::import_from_qwen_cli() const {
  auto cred_path = qwen_cli_credentials_path();
  if (!fs::exists(cred_path)) {
    return std::nullopt;
  }

  try {
    std::ifstream file(cred_path);
    if (!file.is_open()) return std::nullopt;

    json j = json::parse(file);

    OAuthToken token;
    token.provider = QwenPortalConfig::PROVIDER_ID;
    token.access_token = j.value("access_token", "");
    token.refresh_token = j.value("refresh_token", "");
    token.expires_at = j.value("expiry_date", int64_t(0));

    if (token.access_token.empty() || token.refresh_token.empty()) {
      return std::nullopt;
    }

    spdlog::info("[QwenOAuth] Imported credentials from Qwen CLI");
    return token;
  } catch (const std::exception& e) {
    spdlog::warn("[QwenOAuth] Failed to import Qwen CLI credentials: {}", e.what());
    return std::nullopt;
  }
}

std::optional<OAuthToken> QwenPortalAuth::load_token() const {
  // Try cached token first
  if (cached_token_) {
    return cached_token_;
  }

  // Try agent-sdk storage
  auto storage_path = token_storage_path();
  if (fs::exists(storage_path)) {
    try {
      std::ifstream file(storage_path);
      if (file.is_open()) {
        json j = json::parse(file);
        auto token = OAuthToken::from_json(j);
        if (!token.access_token.empty()) {
          cached_token_ = token;
          return token;
        }
      }
    } catch (const std::exception& e) {
      spdlog::warn("[QwenOAuth] Failed to load token: {}", e.what());
    }
  }

  // Try importing from Qwen CLI
  auto cli_token = import_from_qwen_cli();
  if (cli_token) {
    // Save to our storage
    save_token(*cli_token);
    cached_token_ = cli_token;
    return cli_token;
  }

  return std::nullopt;
}

void QwenPortalAuth::save_token(const OAuthToken& token) const {
  auto storage_path = token_storage_path();

  // Ensure directory exists
  fs::create_directories(storage_path.parent_path());

  try {
    std::ofstream file(storage_path);
    if (file.is_open()) {
      file << token.to_json().dump(2);
      cached_token_ = token;
      spdlog::info("[QwenOAuth] Token saved to {}", storage_path.string());
    }
  } catch (const std::exception& e) {
    spdlog::error("[QwenOAuth] Failed to save token: {}", e.what());
  }
}

void QwenPortalAuth::clear_token() const {
  cached_token_.reset();

  auto storage_path = token_storage_path();
  if (fs::exists(storage_path)) {
    std::error_code ec;
    fs::remove(storage_path, ec);
    if (!ec) {
      spdlog::info("[QwenOAuth] Token cleared");
    }
  }
}

std::optional<json> QwenPortalAuth::http_post(const std::string& url, const std::map<std::string, std::string>& form_data) {
  auto body = build_form_body(form_data);
  auto result = http_post_sync(url, body);

  if (!result) {
    return std::nullopt;
  }

  auto [status, response_body] = *result;

  if (status < 200 || status >= 300) {
    spdlog::error("[QwenOAuth] HTTP {} from {}: {}", status, url, response_body);
    return std::nullopt;
  }

  try {
    return json::parse(response_body);
  } catch (const std::exception& e) {
    spdlog::error("[QwenOAuth] Failed to parse JSON response: {}", e.what());
    return std::nullopt;
  }
}

std::optional<DeviceCodeResponse> QwenPortalAuth::request_device_code() {
  if (status_callback_) {
    status_callback_("Requesting device code...");
  }

  // Generate PKCE challenge
  auto pkce = PkceChallenge::generate();
  current_code_verifier_ = pkce.code_verifier;  // Save for token exchange

  std::map<std::string, std::string> params{
      {"client_id", QwenPortalConfig::CLIENT_ID},
      {"code_challenge", pkce.code_challenge},
      {"code_challenge_method", "S256"},
  };

  auto result = http_post(QwenPortalConfig::DEVICE_CODE_URL, params);
  if (!result) {
    return std::nullopt;
  }

  const auto& j = *result;

  DeviceCodeResponse response;
  response.device_code = j.value("device_code", "");
  response.user_code = j.value("user_code", "");
  response.verification_uri = j.value("verification_uri", j.value("verification_url", ""));
  response.verification_uri_complete = j.value("verification_uri_complete", "");
  response.expires_in = j.value("expires_in", 600);
  response.interval = j.value("interval", 5);

  if (response.device_code.empty() || response.user_code.empty()) {
    spdlog::error("[QwenOAuth] Invalid device code response");
    return std::nullopt;
  }

  return response;
}

std::optional<OAuthToken> QwenPortalAuth::poll_for_token(const DeviceCodeResponse& device_code) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(device_code.expires_in);

  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::seconds(device_code.interval));

    std::map<std::string, std::string> params{
        {"grant_type", QwenPortalConfig::DEVICE_GRANT_TYPE},
        {"client_id", QwenPortalConfig::CLIENT_ID},
        {"device_code", device_code.device_code},
        {"code_verifier", current_code_verifier_},  // PKCE code_verifier
    };

    auto body = build_form_body(params);
    auto result = http_post_sync(QwenPortalConfig::TOKEN_URL, body);

    if (!result) {
      continue;
    }

    auto [status, response_body] = *result;

    try {
      json j = json::parse(response_body);

      // Check for errors
      if (j.contains("error")) {
        std::string error = j["error"];
        if (error == "authorization_pending") {
          // User hasn't authorized yet, keep polling
          if (status_callback_) {
            status_callback_("Waiting for authorization...");
          }
          continue;
        } else if (error == "slow_down") {
          // Increase polling interval
          std::this_thread::sleep_for(std::chrono::seconds(5));
          continue;
        } else if (error == "expired_token") {
          spdlog::error("[QwenOAuth] Device code expired");
          return std::nullopt;
        } else if (error == "access_denied") {
          spdlog::error("[QwenOAuth] Access denied by user");
          return std::nullopt;
        } else {
          spdlog::error("[QwenOAuth] Token error: {}", error);
          return std::nullopt;
        }
      }

      // Success - parse token
      OAuthToken token;
      token.provider = QwenPortalConfig::PROVIDER_ID;
      token.access_token = j.value("access_token", "");
      token.refresh_token = j.value("refresh_token", "");

      int expires_in = j.value("expires_in", 3600);
      auto now = std::chrono::system_clock::now();
      token.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() + expires_in * 1000;

      if (!token.access_token.empty()) {
        spdlog::info("[QwenOAuth] Successfully obtained access token");
        return token;
      }
    } catch (const std::exception& e) {
      spdlog::warn("[QwenOAuth] Failed to parse token response: {}", e.what());
    }
  }

  spdlog::error("[QwenOAuth] Device code flow timed out");
  return std::nullopt;
}

std::optional<OAuthToken> QwenPortalAuth::do_refresh(const std::string& refresh_token) {
  if (status_callback_) {
    status_callback_("Refreshing token...");
  }

  std::map<std::string, std::string> params{
      {"grant_type", "refresh_token"},
      {"refresh_token", refresh_token},
      {"client_id", QwenPortalConfig::CLIENT_ID},
  };

  auto result = http_post(QwenPortalConfig::TOKEN_URL, params);
  if (!result) {
    return std::nullopt;
  }

  const auto& j = *result;

  OAuthToken token;
  token.provider = QwenPortalConfig::PROVIDER_ID;
  token.access_token = j.value("access_token", "");
  token.refresh_token = j.value("refresh_token", refresh_token);  // May return same refresh token

  int expires_in = j.value("expires_in", 3600);
  auto now = std::chrono::system_clock::now();
  token.expires_at = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() + expires_in * 1000;

  if (token.access_token.empty()) {
    spdlog::error("[QwenOAuth] Refresh failed - no access token in response");
    return std::nullopt;
  }

  spdlog::info("[QwenOAuth] Token refreshed successfully");
  return token;
}

bool QwenPortalAuth::open_browser(const std::string& url) const {
#ifdef __APPLE__
  std::string cmd = "open \"" + url + "\"";
#elif defined(_WIN32)
  std::string cmd = "start \"\" \"" + url + "\"";
#else
  std::string cmd = "xdg-open \"" + url + "\"";
#endif
  int result = std::system(cmd.c_str());
  return result == 0;
}

std::future<std::optional<OAuthToken>> QwenPortalAuth::authenticate() {
  return std::async(std::launch::async, [this]() -> std::optional<OAuthToken> {
    // Request device code
    auto device_code = request_device_code();
    if (!device_code) {
      if (status_callback_) {
        status_callback_("Failed to request device code");
      }
      return std::nullopt;
    }

    // Display verification info to user
    if (user_code_callback_) {
      user_code_callback_(device_code->verification_uri, device_code->user_code, device_code->verification_uri_complete);
    } else {
      // Default: log to console
      spdlog::info("[QwenOAuth] Please visit: {}", device_code->verification_uri);
      spdlog::info("[QwenOAuth] Enter code: {}", device_code->user_code);
    }

    // Try to open browser
    std::string browser_url = device_code->verification_uri_complete.empty() ? device_code->verification_uri : device_code->verification_uri_complete;
    if (!open_browser(browser_url)) {
      spdlog::warn("[QwenOAuth] Failed to open browser. Please open the URL manually.");
    }

    if (status_callback_) {
      status_callback_("Waiting for authorization in browser...");
    }

    // Poll for token
    auto token = poll_for_token(*device_code);
    if (token) {
      save_token(*token);
      if (status_callback_) {
        status_callback_("Authentication successful!");
      }
    } else {
      if (status_callback_) {
        status_callback_("Authentication failed");
      }
    }

    return token;
  });
}

std::future<std::optional<OAuthToken>> QwenPortalAuth::refresh(const OAuthToken& token) {
  return std::async(std::launch::async, [this, token]() -> std::optional<OAuthToken> {
    auto new_token = do_refresh(token.refresh_token);
    if (new_token) {
      save_token(*new_token);
    }
    return new_token;
  });
}

std::optional<OAuthToken> QwenPortalAuth::get_valid_token() {
  auto token = load_token();
  if (!token) {
    return std::nullopt;
  }

  // Check if needs refresh
  if (token->needs_refresh()) {
    spdlog::info("[QwenOAuth] Token expiring soon, refreshing...");
    auto new_token = do_refresh(token->refresh_token);
    if (new_token) {
      save_token(*new_token);
      return new_token;
    }
    // If refresh failed and token is actually expired, return nullopt
    if (token->is_expired()) {
      spdlog::warn("[QwenOAuth] Token expired and refresh failed");
      return std::nullopt;
    }
  }

  return token;
}

bool QwenPortalAuth::has_valid_token() {
  auto token = load_token();
  if (!token) {
    return false;
  }
  return !token->is_expired();
}

// ============================================================
// Singleton accessor
// ============================================================

QwenPortalAuth& qwen_portal_auth() {
  static QwenPortalAuth instance;
  return instance;
}

// ============================================================
// Plugin implementation
// ============================================================

std::optional<std::string> QwenAuthProvider::get_auth_header() {
  auto token = qwen_portal_auth().get_valid_token();
  if (token) {
    return "Bearer " + token->access_token;
  }
  spdlog::warn("[QwenOAuth] Token not available");
  return std::nullopt;
}

void register_qwen_plugin() {
  auto provider = std::make_shared<QwenAuthProvider>();
  AuthProviderRegistry::instance().register_provider(provider);
  spdlog::info("[QwenPlugin] Qwen OAuth plugin registered");
}

}  // namespace agent::plugin::qwen
