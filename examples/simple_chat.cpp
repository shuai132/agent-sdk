#include <iostream>
#include <string>
#include <thread>

#include <asio.hpp>

#include "agent/agent.hpp"

using namespace agent;

int main(int argc, char* argv[]) {
    std::cout << "Agent C++ - Simple Chat Example\n";
    std::cout << "================================\n\n";
    
    // Load configuration
    Config config = Config::load_default();
    
    // Check for API key in environment (support both ANTHROPIC_API_KEY and ANTHROPIC_AUTH_TOKEN)
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    if (!api_key) {
        api_key = std::getenv("ANTHROPIC_AUTH_TOKEN");
    }
    
    const char* base_url = std::getenv("ANTHROPIC_BASE_URL");
    const char* model = std::getenv("ANTHROPIC_MODEL");
    
    if (api_key) {
        config.providers["anthropic"] = ProviderConfig{
            "anthropic",
            api_key,
            base_url ? base_url : "https://api.anthropic.com",
            std::nullopt,
            {}
        };
        
        if (model) {
            config.default_model = model;
        }
        
        std::cout << "Using API: " << (base_url ? base_url : "https://api.anthropic.com") << "\n";
        std::cout << "Model: " << config.default_model << "\n\n";
    } else {
        std::cerr << "Warning: ANTHROPIC_API_KEY or ANTHROPIC_AUTH_TOKEN not set\n";
    }
    
    // Initialize ASIO
    asio::io_context io_ctx;
    
    // Register builtin tools
    tools::register_builtins();
    
    // Create session
    auto session = Session::create(io_ctx, config, AgentType::Build);
    
    // Set up callbacks
    session->on_stream([](const std::string& text) {
        std::cout << text << std::flush;
    });
    
    session->on_tool_call([](const std::string& tool, const json& args) {
        std::cout << "\n[Calling tool: " << tool << "]\n";
    });
    
    session->on_complete([](FinishReason reason) {
        std::cout << "\n\n[Session completed: " << to_string(reason) << "]\n";
    });
    
    session->on_error([](const std::string& error) {
        std::cerr << "\n[Error: " << error << "]\n";
    });
    
    // Simple permission handler
    session->set_permission_handler(
        [](const std::string& permission, const std::string& description) {
            std::promise<bool> promise;
            auto future = promise.get_future();
            
            std::cout << "\n[Permission requested: " << permission << "]\n";
            std::cout << description << "\n";
            std::cout << "Allow? (y/n): ";
            
            std::string input;
            std::getline(std::cin, input);
            
            promise.set_value(input == "y" || input == "Y" || input == "yes");
            return future;
        }
    );
    
    // Run IO context in background thread
    std::thread io_thread([&io_ctx]() {
        asio::io_context::work work(io_ctx);
        io_ctx.run();
    });
    
    // Chat loop
    std::string input;
    std::cout << "Enter your message (or 'quit' to exit):\n\n";
    
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);
        
        if (input == "quit" || input == "exit") {
            break;
        }
        
        if (input.empty()) {
            continue;
        }
        
        std::cout << "\nAssistant: ";
        session->prompt(input);
        std::cout << "\n\n";
    }
    
    // Cleanup
    session->cancel();
    io_ctx.stop();
    io_thread.join();
    
    std::cout << "Goodbye!\n";
    return 0;
}
