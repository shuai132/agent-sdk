// CLI example
#include <iostream>
#include <string>

#include <asio.hpp>

#include "agent/agent.hpp"

int main(int argc, char* argv[]) {
    std::cout << "Agent C++ CLI v" << agent::version() << "\n";
    
    // Initialize
    agent::init();
    
    // TODO: Implement CLI interface
    
    std::cout << "CLI not fully implemented yet.\n";
    std::cout << "See simple_chat example for usage.\n";
    
    agent::shutdown();
    return 0;
}
