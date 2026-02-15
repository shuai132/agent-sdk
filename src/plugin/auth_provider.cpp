#include "plugin/auth_provider.hpp"

#include <spdlog/spdlog.h>

namespace agent::plugin {

AuthProviderRegistry& AuthProviderRegistry::instance() {
  static AuthProviderRegistry instance;
  return instance;
}

void AuthProviderRegistry::register_provider(AuthProviderPtr provider) {
  spdlog::info("[Plugin] Registered auth provider: {}", provider->scheme());
  providers_.push_back(std::move(provider));
}

AuthProviderPtr AuthProviderRegistry::get_provider(const std::string& scheme) {
  for (const auto& provider : providers_) {
    if (provider->scheme() == scheme) {
      return provider;
    }
  }
  return nullptr;
}

std::string AuthProviderRegistry::get_auth_header(const std::string& api_key) {
  for (const auto& provider : providers_) {
    if (provider->can_handle(api_key)) {
      auto header = provider->get_auth_header();
      if (header) {
        return *header;
      }
      spdlog::warn("[Plugin] Auth provider {} failed to get header", provider->scheme());
    }
  }
  // Default: use api_key as Bearer token
  return "Bearer " + api_key;
}

bool AuthProviderRegistry::is_dynamic_auth(const std::string& api_key) const {
  for (const auto& provider : providers_) {
    if (provider->can_handle(api_key)) {
      return true;
    }
  }
  return false;
}

}  // namespace agent::plugin
