# agent-sdk

agent sdk for cpp, like claude code sdk but lightweight.

[中文](README.md) | English

## agent_cli

![agent_cli.png](doc/img/agent_cli.png)

## Why Implement in C++?

Existing AI Agent projects (Claude Code, OpenCode, OpenClaw, etc.) share two common issues:

- **High complexity**: These projects are fully featured, but not suitable for scenarios that only require core Agent
  capabilities, and are nearly impossible to decompose for secondary development.
- **Heavy runtime**: Built on Node.js / TypeScript stack, dependent on the Node runtime, with packaged binaries
  approaching 100MB, making embedding into other projects too expensive.

agent-sdk chooses C++ precisely to address these pain points: the build output is a lightweight native static library
with zero runtime dependencies, easily embeddable into any C/C++ project, and even capable of running in embedded
environments.

| Binary (macOS)   | Size     |
|------------------|----------|
| claude v2.1.39   | 174M     |
| opencode v1.1.65 | 101M     |
| **agent-sdk**    | **1~3M** |

## Goals

- **Keep it simple and easy to understand**: Clean code structure, modular design
- **Include mainstream core features**: Similar to Claude Code / OpenCode core capabilities
- **Use as a library/framework**: Provides a C++ static library `agent_sdk`, embeddable into other projects

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
| `skill`    | Load skill instructions on demand     |

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

1. Project-level: `.agent-sdk/config.json`
2. Global: `~/.config/agent-sdk/config.json`
3. Instruction files: Hierarchical search for `AGENTS.md`, compatible with `CLAUDE.md`, `.agents/`, `.claude/`,
   `.opencode/` conventions

### 🌐 MCP Support (WIP)

Model Context Protocol client, supporting:

- Local stdio transport
- Remote SSE transport

### 📦 Skill System

Compatible with the skill specifications of mainstream AI Agent tools such as OpenCode and Claude Code, enabling
cross-tool skill sharing:

- **Multi-convention compatible**: Automatically searches `.agent-sdk/`, `.agents/`, `.claude/`, `.opencode/`
  directories for `SKILL.md`
- **Hierarchical discovery**: Traverses from the project directory up to the git root, collecting all skills
- **Global sharing**: Supports global directories like `~/.agents/skills/` for sharing skills across Agent tools
- **On-demand loading**: LLM loads skills via the built-in `skill` tool, preserving context window space
- **Standard format**: YAML frontmatter + Markdown body with name, description, license and other metadata

See [Skill System Design](doc/skill-system.md) for details.

## Dependencies

| Dependency                                         | Version | Purpose                 |
|----------------------------------------------------|---------|-------------------------|
| C++                                                | 20      | Language standard       |
| [Asio](https://github.com/chriskohlhoff/asio)      | 1.36.0  | Async I/O & networking  |
| [spdlog](https://github.com/gabime/spdlog)         | 1.17.0  | Logging                 |
| [nlohmann/json](https://github.com/nlohmann/json)  | 3.12.0  | JSON parsing            |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI)   | 6.1.9   | TUI terminal interface  |
| [OpenSSL](https://www.openssl.org/)                | —       | HTTPS / TLS             |
| [GoogleTest](https://github.com/google/googletest) | 1.14.0  | Unit testing (optional) |

> Except for OpenSSL which requires system installation, all other dependencies are managed via git submodule in the
`thirdparty/` directory.

## Building

### Prerequisites

- CMake ≥ 3.20
- C++20 compatible compiler (GCC 12+ / Clang 15+ / MSVC 2022+)
- OpenSSL development libraries

### Build Steps

```bash
# Clone the project (with submodules)
git clone --recursive https://github.com/shuai132/agent-sdk.git
cd agent-sdk

# If already cloned without submodules
git submodule update --init --recursive

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
# Run
./build/agent_cli

# Show help
./build/agent_cli --help
```

**Environment Variable Configuration** (choose one):

```bash
# Option 1: Qwen Portal (OAuth authentication, no API key required, recommended)
export QWEN_OAUTH=1
# Optional overrides:
export QWEN_BASE_URL="https://portal.qwen.ai"
export QWEN_MODEL="coder-model"
# On first use, a QR code will be displayed for login authentication

# Option 2: Anthropic
export ANTHROPIC_API_KEY="your-api-key"
# Optional overrides:
export ANTHROPIC_BASE_URL="https://api.anthropic.com"
export ANTHROPIC_MODEL="claude-sonnet-4-20250514"

# Option 3: OpenAI (or compatible API)
export OPENAI_API_KEY="your-api-key"
# Optional overrides:
export OPENAI_BASE_URL="https://api.openai.com"
export OPENAI_MODEL="gpt-4o"
```

> **Priority**: `QWEN_OAUTH` > `OPENAI_API_KEY` (Qwen OAuth takes precedence when both are set)

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

## TUI Terminal Interface (agent_cli)

agent-sdk provides a full-featured TUI (Terminal User Interface) application `agent_cli`, built
with [FTXUI](https://github.com/ArthurSonzogni/FTXUI).

### Running

```bash
# Set API Key
export ANTHROPIC_API_KEY="your-key"
# or
export OPENAI_API_KEY="your-key"

# Run TUI
./build/agent_cli
```

### Features

- ✅ **Real-time streaming**: LLM responses display in real-time
- ✅ **Tool call visualization**: Tool cards with click-to-expand/collapse details
- ✅ **Command completion**: Type `/` for automatic command menu
- ✅ **Session management**: Create, switch, and delete sessions
- ✅ **Scrolling**: Mouse wheel and PageUp/PageDown support
- ✅ **Mode switching**: Quick toggle between Build/Plan modes (Tab key)
- ✅ **Copy chat**: One-click copy chat content to clipboard

### Interactions

#### Keyboard Shortcuts

| Shortcut      | Function                |
|---------------|-------------------------|
| `Tab`         | Switch Build/Plan mode  |
| `Esc`         | Interrupt running agent |
| `Ctrl+C`      | Press twice to exit     |
| `PageUp/Down` | Scroll chat history     |
| `↑/↓`         | Navigate command menu   |

#### Mouse Interactions

| Action             | Function                          |
|--------------------|-----------------------------------|
| Click tool card    | Expand/collapse tool call details |
| Scroll wheel       | Scroll chat history               |
| Click session item | Select session (in session panel) |

#### Commands

| Command     | Shortcut | Function                   |
|-------------|----------|----------------------------|
| `/quit`     | `/q`     | Exit program               |
| `/clear`    | -        | Clear chat history         |
| `/help`     | `/h`     | Show help                  |
| `/sessions` | `/s`     | Open session list panel    |
| `/expand`   | -        | Expand all tool calls      |
| `/collapse` | -        | Collapse all tool calls    |
| `/copy`     | `/c`     | Copy chat to clipboard     |
| `/compact`  | -        | Trigger context compaction |

### Session Management

In the session list panel (open with `/sessions`):

- `↑/↓` or `j/k`: Navigate
- `Enter`: Load selected session
- `d`: Delete selected session
- `n`: Create new session
- `Esc`: Close panel

## Project Structure

```
agent-sdk/
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
│   └── skill/          # Skill system (discovery, parsing, registry)
├── examples/           # Example programs
│   ├── agent.cpp       # Interactive Agent CLI
│   ├── api_test.cpp    # API call test
│   └── tool_test.cpp   # Tool system test
└── tests/              # Unit tests (GoogleTest)
    ├── test_message.cpp
    ├── test_tool.cpp
    ├── test_session.cpp
    ├── test_llm.cpp
    └── test_skill.cpp
```

## TODO

- [x] TUI support
- [x] Skill system
- [x] Session persistence
- [x] Full MCP client implementation
- [ ] More LLM Providers (Gemini, local models, etc.)
- [ ] Vision support (image input)
- [ ] REST API for server mode
- [ ] C++20 coroutine (`co_await`) interface

## Related Docs

- [Skill System Design](doc/skill-system.md)
- [OpenCode Core Architecture Analysis](doc/OpenCode核心架构分析.md)

## Acknowledgments

- [OpenCode](https://github.com/anomalyco/opencode) — Architecture reference
- [Claude Code](https://docs.anthropic.com/en/docs/claude-code) — Feature reference

## License

MIT
