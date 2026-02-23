// Tool registry initialization - delegates to tools::register_builtins() in tool/builtin/bash.cpp
#include <spdlog/spdlog.h>

#include "builtin/builtins.hpp"
#include "tool.hpp"

namespace agent {

void ToolRegistry::init_builtins() {
  spdlog::debug("[ToolRegistry] Initializing built-in tools");
  tools::register_builtins();
}

}  // namespace agent
