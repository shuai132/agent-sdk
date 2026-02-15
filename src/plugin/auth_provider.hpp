#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent::plugin {

// Abstract interface for dynamic authentication providers
// Allows plugins to provide API keys/tokens without core code knowing implementation details
class AuthProvider {
 public:
  virtual ~AuthProvider() = default;

  // Get the authentication scheme identifier (e.g., "qwen-oauth", "azure-ad")
  virtual std::string scheme() const = 0;

  // Get valid authorization header value (e.g., "Bearer xxx")
  // Returns nullopt if authentication failed or unavailable
  virtual std::optional<std::string> get_auth_header() = 0;

  // Check if this provider can handle the given api_key placeholder
  virtual bool can_handle(const std::string& api_key) const = 0;
};

using AuthProviderPtr = std::shared_ptr<AuthProvider>;

// Registry for authentication providers
class AuthProviderRegistry {
 public:
  static AuthProviderRegistry& instance();

  // Register an auth provider
  void register_provider(AuthProviderPtr provider);

  // Get the provider for the given scheme
  AuthProviderPtr get_provider(const std::string& scheme);

  // Get auth header for the given api_key
  // If api_key matches a registered provider's scheme, use that provider
  // Otherwise, return "Bearer {api_key}"
  std::string get_auth_header(const std::string& api_key);

  // Check if api_key is handled by a registered provider
  bool is_dynamic_auth(const std::string& api_key) const;

 private:
  AuthProviderRegistry() = default;
  std::vector<AuthProviderPtr> providers_;
};

}  // namespace agent::plugin
