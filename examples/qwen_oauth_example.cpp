#include "llm/qwen.hpp"
#include "llm/qwen_oauth.hpp"
#include "core/config.hpp"
#include "session/session.hpp"
#include <iostream>
#include <asio/io_context.hpp>

using namespace agent;
using namespace agent::llm;

int main() {
    asio::io_context io_ctx;
    
    // Example 1: Using API Key (traditional method)
    {
        std::cout << "=== Using Qwen with API Key ===" << std::endl;
        
        ProviderConfig config;
        config.name = "qwen";
        config.api_key = "your-qwen-api-key-here";  // Replace with actual API key
        config.base_url = "https://dashscope.aliyuncs.com";  // Qwen API endpoint
        
        auto provider = std::make_shared<QwenProvider>(config, io_ctx);
        
        // Create a session with the Qwen provider
        auto session = Session::create(io_ctx, "test-session", provider);
        
        std::cout << "Available Qwen models:" << std::endl;
        for (const auto& model : provider->models()) {
            std::cout << "  - " << model.id << " (context: " << model.context_window << ")" << std::endl;
        }
        
        std::cout << std::endl;
    }
    
    // Example 2: Using OAuth Token (OAuth method)
    {
        std::cout << "=== Using Qwen with OAuth Token ===" << std::endl;
        
        // Step 1: Initiate OAuth flow
        std::string auth_url = QwenOAuthHelper::initiate_oauth_flow(
            "your-client-id", 
            "your-redirect-uri",
            "api_invoke"
        );
        
        std::cout << "Visit this URL to authorize the application:" << std::endl;
        std::cout << auth_url << std::endl;
        std::cout << "After authorization, you'll receive an authorization code." << std::endl;
        
        // Step 2: Exchange authorization code for access token
        // This would typically happen in a real application after the user visits the auth URL
        // and provides the authorization code received from the redirect
        std::string auth_code = "received-auth-code";  // Replace with actual code from redirect
        std::string client_id = "your-client-id";
        std::string client_secret = "your-client-secret";
        std::string redirect_uri = "your-redirect-uri";
        
        auto access_token = QwenOAuthHelper::exchange_code_for_token(
            client_id, 
            client_secret, 
            auth_code, 
            redirect_uri
        );
        
        if (access_token) {
            std::cout << "Successfully obtained access token!" << std::endl;
            
            // Validate the token
            if (QwenOAuthHelper::validate_token(*access_token)) {
                std::cout << "Token is valid." << std::endl;
                
                // Create provider config with OAuth token
                ProviderConfig config;
                config.name = "qwen";
                config.api_key = *access_token;  // Store OAuth token as api_key
                config.base_url = "https://dashscope.aliyuncs.com";
                
                // Add Authorization header to use OAuth token
                config.headers["Authorization"] = "Bearer " + *access_token;
                
                auto provider = std::make_shared<QwenProvider>(config, io_ctx);
                
                // Create a session with the Qwen provider using OAuth
                auto session = Session::create(io_ctx, "oauth-test-session", provider);
                
                std::cout << "Successfully created session with OAuth authentication!" << std::endl;
            } else {
                std::cout << "Token validation failed!" << std::endl;
            }
        } else {
            std::cout << "Failed to obtain access token!" << std::endl;
        }
        
        std::cout << std::endl;
    }
    
    // Example 3: Using refresh token to get new access token
    {
        std::cout << "=== Refreshing OAuth Token ===" << std::endl;
        
        std::string refresh_token = "your-refresh-token";
        std::string client_id = "your-client-id";
        std::string client_secret = "your-client-secret";
        
        auto new_access_token = QwenOAuthHelper::refresh_access_token(
            client_id,
            client_secret,
            refresh_token
        );
        
        if (new_access_token) {
            std::cout << "Successfully refreshed access token!" << std::endl;
        } else {
            std::cout << "Failed to refresh access token!" << std::endl;
        }
    }
    
    return 0;
}