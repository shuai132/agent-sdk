#pragma once

// Core types
#include "agent/core/types.hpp"
#include "agent/core/uuid.hpp"
#include "agent/core/message.hpp"
#include "agent/core/config.hpp"

// Event bus
#include "agent/bus/bus.hpp"

// Network
#include "agent/net/http_client.hpp"
#include "agent/net/sse_client.hpp"

// LLM providers
#include "agent/llm/provider.hpp"

// Tool system
#include "agent/tool/tool.hpp"
#include "agent/tool/builtin/builtins.hpp"

// Session management
#include "agent/session/session.hpp"

namespace agent {

// Initialize the agent framework
void init();

// Shutdown the agent framework
void shutdown();

// Get version string
std::string version();

}  // namespace agent
