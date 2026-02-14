# agent-cpp

[中文](README.md) | English

A lightweight AI Agent SDK implemented in C++. The core is an Agent Loop.

## Why Reimplement in C++?

Existing AI Agent projects (Claude Code, OpenCode, OpenClaw, etc.) share two common issues:

- **Feature-rich but high complexity**: These projects are excellent and fully featured, but for scenarios that only
  require core Agent capabilities, the barrier to understanding and extending them is quite high
- **Heavy runtime**: Almost all are built on Node.js / TypeScript — the plugin ecosystem is convenient, but the cost is
  carrying an entire Node runtime, with packaged binaries easily approaching 100MB, making embedding into other projects
  too expensive

agent-cpp chooses C++ precisely to address these pain points: the build output is a lightweight native static library
with zero runtime dependencies, easily embeddable into any C/C++ project, and even capable of running in embedded
environments.

## Goals

- **Keep it simple and easy to understand**: Clean code structure, modular design
- **Include mainstream core features**: Similar to Claude Code / OpenCode core capabilities
- **Use as a library/framework**: Provides a C++ static library `agent_core`, embeddable into other projects

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                   Application                   │
│              (examples/agent.cpp)               │
├─────────────────────────────────────────────────┤
│                  Session Layer                  │
│    Agent Loop · Context Management · Compaction │
├──────────┬──────────┬───────────┬───────────────┤
│ LLM      │  Tool    │   MCP     │   Skill       │
│ Provider │  System  │  Client   │   System      │
├──────────┴──────────┴───────────┴───────────────┤
│              Core / Event Bus / Net             │
│       Types · Message · Config · HTTP · SSE     │
└─────────────────────────────────────────────────┘
```

## Key Features

### 🤖 Agent Loop

A complete Agent Loop implementation supporting multi-turn conversations, tool calling, and streaming output:

- **Streaming responses**: Real-time streaming of LLM replies
- **Tool call loop**: Automatically executes tool calls and feeds results back to the LLM
- **Doom Loop detection**: Detects repeated tool calls to prevent infinite loops
- **Context compaction**: Automatically compresses conversation history when approaching context window limits
- **Output truncation**: Automatically truncates overly long tool outputs to save tokens

### 🔧 Built-in Tools

| Tool       | Description                           |
|------------|---------------------------------------|
| `bash`     | Execute shell commands (with timeout) |
| `read`     | Read file contents                    |
| `write`    | Write file contents                   |
| `edit`     | Search and replace in files           |
| `glob`     | Find files by pattern matching        |
| `grep`     | Search file contents                  |
| `task`     | Launch a subagent for subtasks        |
| `question` | Ask the user a question               |

### 🔌 LLM Providers

Supports multiple LLM providers with a unified Provider interface:

- **Anthropic** (Claude series)
- **OpenAI** (GPT series, and OpenAI API-compatible services)
- Register custom providers via `ProviderFactory`

### 🧠 Multiple Agent Types

| Agent Type   | Purpose               | Tool Permissions       |
|--------------|-----------------------|------------------------|
| `Build`      | Main coding agent     | Requires user approval |
| `Explore`    | Read-only exploration | Auto-allow (no writes) |
| `General`    | General subagent      | Requires user approval |
| `Plan`       | Planning agent        | read/glob/grep only    |
| `Compaction` | Context compression   | No tools               |

### 📡 Event Bus

A type-safe event bus for loosely coupled inter-module communication:

- `SessionCreated` / `SessionEnded`
- `MessageAdded` / `StreamDelta`
- `ToolCallStarted` / `ToolCallCompleted`
- `TokensUsed` / `ContextCompacted`
- `PermissionRequested`

### 🔐 Permission System

Permission control before tool execution:

- **Allow**: Automatically permitted
- **Deny**: Automatically rejected
- **Ask**: Prompt user for confirmation

Supports per-tool permission policy configuration.

### ⚙️ Configuration System

Supports layered configuration, from highest to lowest priority:

1. Project-level: `.agent-cpp/config.json`
2. Global: `~/.config/agent-cpp/config.json`
3. Instruction files: Hierarchical search for `AGENTS.md` (similar to Claude Code)

### 🌐 MCP Support (WIP)

Model Context Protocol client, supporting:

- Local stdio transport
- Remote SSE transport

## Dependencies

| Dependency                                         | Version | Purpose                 |
|----------------------------------------------------|---------|-------------------------|
| C++                                                | 20      | Language standard       |
| [Asio](https://github.com/chriskohlhoff/asio)      | 1.30.2  | Async I/O & networking  |
| [spdlog](https://github.com/gabime/spdlog)         | 1.13.0  | Logging                 |
| [nlohmann/json](https://github.com/nlohmann/json)  | 3.11.3  | JSON parsing            |
| [OpenSSL](https://www.openssl.org/)                | —       | HTTPS / TLS             |
| [GoogleTest](https://github.com/google/googletest) | 1.14.0  | Unit testing (optional) |

> Except for OpenSSL which requires system installation, all other dependencies are automatically fetched via CMake
`FetchContent`.

## Building

### Prerequisites

- CMake ≥ 3.20
- C++20 compatible compiler (GCC 12+ / Clang 15+ / MSVC 2022+)
- OpenSSL development libraries

### Build Steps

```bash
# Clone the project
git clone https://github.com/shuai132/agent-cpp.git
cd agent-cpp

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### CMake Options

| Option                 | Default | Description      |
|------------------------|---------|------------------|
| `AGENT_BUILD_TESTS`    | `ON`    | Build unit tests |
| `AGENT_BUILD_EXAMPLES` | `ON`    | Build examples   |

## Quick Start

### Run the Agent CLI

```bash
# Set API Key
export ANTHROPIC_API_KEY="your-api-key"

# Optional: custom API endpoint and model
export ANTHROPIC_BASE_URL="https://api.anthropic.com"
export ANTHROPIC_MODEL="claude-sonnet-4-20250514"

# Run
./build/agent
```

### Code Example

```cpp
#include "agent/agent.hpp"

using namespace agent;

int main() {
    Config config = Config::load_default();
    config.providers["anthropic"] = ProviderConfig{
        "anthropic", "your-api-key", "https://api.anthropic.com"
    };

    asio::io_context io_ctx;
    tools::register_builtins();

    auto session = Session::create(io_ctx, config, AgentType::Build);

    // Set up streaming output callback
    session->on_stream([](const std::string& text) {
        std::cout << text << std::flush;
    });

    // Set up tool call callback
    session->on_tool_call([](const std::string& tool, const json& args) {
        std::cout << "[Tool: " << tool << "]\n";
    });

    session->on_complete([](FinishReason reason) {
        std::cout << "\n[Done]\n";
    });

    // Run IO in a background thread
    std::thread io_thread([&io_ctx]() {
        asio::io_context::work work(io_ctx);
        io_ctx.run();
    });

    // Send a message to trigger the Agent Loop
    session->prompt("Write a Hello World program for me");

    io_ctx.stop();
    io_thread.join();
}
```

## Project Structure

```
agent-cpp/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── core/           # Core types, messages, config, UUID
│   ├── bus/            # Event bus
│   ├── net/            # HTTP / SSE client (Asio + OpenSSL)
│   ├── llm/            # LLM Providers (Anthropic / OpenAI)
│   ├── tool/           # Tool system (registry, permissions, builtins)
│   │   └── builtin/    # Built-in tool implementations
│   ├── session/        # Session management (Agent Loop, compaction, truncation)
│   ├── agent/          # Agent framework entry point
│   ├── mcp/            # MCP client (WIP)
│   └── skill/          # Skill system (WIP)
├── examples/           # Example programs
│   ├── agent.cpp       # Interactive Agent CLI
│   ├── api_test.cpp    # API call test
│   └── tool_test.cpp   # Tool system test
└── tests/              # Unit tests (GoogleTest)
    ├── test_message.cpp
    ├── test_tool.cpp
    ├── test_session.cpp
    └── test_llm.cpp
```

## TODO

- [ ] TUI support
- [ ] Skill system improvements
- [ ] Session persistence
- [ ] REST API for server mode
- [ ] C++20 coroutine (`co_await`) interface
- [ ] Full MCP client implementation
- [ ] More LLM Providers (Gemini, local models, etc.)
- [ ] Vision support (image input)

## Related Docs

- [OpenCode Core Architecture Analysis](doc/OpenCode核心架构分析.md)

## Acknowledgments

- [OpenCode](https://github.com/anomalyco/opencode) — Architecture reference
- [Claude Code](https://docs.anthropic.com/en/docs/claude-code) — Feature reference

## License

MIT
