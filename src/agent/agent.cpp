// Agent initialization
#include "agent/agent.hpp"
#include "agent/tool/builtin/builtins.hpp"
#include "agent/llm/anthropic.hpp"

namespace agent {

// Force inclusion of Anthropic provider registration
namespace {
    // This function exists to force the linker to include the anthropic.cpp
    // translation unit which contains the static provider registration
    void force_provider_registration() {
        // Just reference the type to ensure the translation unit is linked
        (void)sizeof(llm::AnthropicProvider);
    }
}

void init() {
    force_provider_registration();
    tools::register_builtins();
}

void shutdown() {
    // Cleanup if needed
}

std::string version() {
    return "0.1.0";
}

}  // namespace agent
