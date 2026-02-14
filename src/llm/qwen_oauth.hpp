#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace agent::llm {

// OAuth helper for Qwen API authentication
class QwenOAuthHelper {
public:
    // Initialize OAuth flow - returns the authorization URL to visit
    static std::string initiate_oauth_flow(const std::string& client_id, 
                                         const std::string& redirect_uri,
                                         const std::string& scope = "api_invoke");

    // Exchange authorization code for access token
    static std::optional<std::string> exchange_code_for_token(const std::string& client_id,
                                                            const std::string& client_secret,
                                                            const std::string& code,
                                                            const std::string& redirect_uri);

    // Refresh access token using refresh token
    static std::optional<std::string> refresh_access_token(const std::string& client_id,
                                                         const std::string& client_secret,
                                                         const std::string& refresh_token);

    // Validate access token
    static bool validate_token(const std::string& access_token);

private:
    static const std::string AUTH_URL;
    static const std::string TOKEN_URL;
    static const std::string VALIDATION_URL;
};

} // namespace agent::llm