# agent-cpp

中文 | [English](README_EN.md)

一个使用 C++ 实现的 AI Agent（智能体）SDK，核心部分是一个Agent Loop。

## 为什么用 C++ 重新实现？

现有的 AI Agent 项目（Claude Code、OpenCode、OpenClaw 等）普遍存在两个问题：

- **功能丰富但复杂度高**：这些项目都非常优秀且功能全面，但对于只需要核心 Agent 能力的场景来说，理解和二次开发的门槛较高
- **运行时笨重**：几乎清一色基于 Node.js / TypeScript 构建——插件生态确实方便，但代价是必须携带整个 Node
  运行时，打包后动辄近百兆，嵌入到项目的代价太大

agent-cpp 选择 C++ 正是为了解决这些痛点：编译产物是一个轻量的原生静态库，零运行时依赖，可以轻松嵌入任何 C/C++
项目，甚至可以在嵌入式环境中运行。

## 项目目标

- **保持简单，容易理解**：代码结构清晰，模块化设计
- **包含主流基本功能**：类似 Claude Code / OpenCode 的核心能力
- **作为库/框架使用**：提供 C++ 静态库 `agent_core`，可嵌入到其他项目

## 架构概览

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

## 核心特性

### 🤖 Agent Loop

完整的 Agent Loop 实现，支持多轮对话、工具调用、流式输出：

- **流式响应**：实时流式输出 LLM 回复
- **工具调用循环**：自动执行工具调用并将结果反馈给 LLM
- **Doom Loop 检测**：检测重复的工具调用，防止无限循环
- **上下文压缩（Compaction）**：当上下文窗口接近限制时自动压缩对话历史
- **输出截断（Truncation）**：工具输出过长时自动截断，避免浪费 Token

### 🔧 内置工具

| 工具         | 描述                       |
|------------|--------------------------|
| `bash`     | 执行 Shell 命令（支持超时控制）      |
| `read`     | 读取文件内容                   |
| `write`    | 写入文件内容                   |
| `edit`     | 搜索替换编辑文件                 |
| `glob`     | 按模式匹配查找文件                |
| `grep`     | 搜索文件内容                   |
| `task`     | 启动子 Agent（subagent）执行子任务 |
| `question` | 向用户提问                    |

### 🔌 LLM Provider

支持多种 LLM 提供商，使用统一的 Provider 接口：

- **Anthropic**（Claude 系列）
- **OpenAI**（GPT 系列，以及兼容 OpenAI API 的服务）
- 支持通过 `ProviderFactory` 注册自定义 Provider

### 🧠 多 Agent 类型

| Agent 类型     | 用途        | 工具权限             |
|--------------|-----------|------------------|
| `Build`      | 主编码 Agent | 需询问用户            |
| `Explore`    | 只读探索      | 自动允许（禁止写入）       |
| `General`    | 通用子 Agent | 需询问用户            |
| `Plan`       | 规划 Agent  | 仅 read/glob/grep |
| `Compaction` | 上下文压缩     | 无工具              |

### 📡 事件总线（Event Bus）

类型安全的事件总线，用于模块间松耦合通信：

- `SessionCreated` / `SessionEnded`
- `MessageAdded` / `StreamDelta`
- `ToolCallStarted` / `ToolCallCompleted`
- `TokensUsed` / `ContextCompacted`
- `PermissionRequested`

### 🔐 权限系统

工具执行前的权限控制：

- **Allow**：自动允许
- **Deny**：自动拒绝
- **Ask**：询问用户确认

支持按工具粒度配置权限策略。

### ⚙️ 配置系统

支持分层配置，优先级由高到低：

1. 项目级配置：`.agent-cpp/config.json`
2. 全局配置：`~/.config/agent-cpp/config.json`
3. 指令文件：层级搜索 `AGENTS.md`（类似 Claude Code）

### 🌐 MCP 支持（WIP）

Model Context Protocol 客户端，支持：

- 本地 stdio 传输
- 远程 SSE 传输

## 依赖

| 依赖                                                 | 版本     | 用途          |
|----------------------------------------------------|--------|-------------|
| C++                                                | 20     | 语言标准        |
| [Asio](https://github.com/chriskohlhoff/asio)      | 1.30.2 | 异步 I/O 与网络  |
| [spdlog](https://github.com/gabime/spdlog)         | 1.13.0 | 日志          |
| [nlohmann/json](https://github.com/nlohmann/json)  | 3.11.3 | JSON 解析     |
| [OpenSSL](https://www.openssl.org/)                | —      | HTTPS / TLS |
| [GoogleTest](https://github.com/google/googletest) | 1.14.0 | 单元测试（可选）    |

> 除 OpenSSL 需要系统安装外，其他依赖均通过 CMake `FetchContent` 自动拉取。

## 构建

### 前置要求

- CMake ≥ 3.20
- 支持 C++20 的编译器（GCC 12+ / Clang 15+ / MSVC 2022+）
- OpenSSL 开发库

### 编译步骤

```bash
# 克隆项目
git clone https://github.com/shuai132/agent-cpp.git
cd agent-cpp

# 构建
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### CMake 选项

| 选项                     | 默认值  | 描述     |
|------------------------|------|--------|
| `AGENT_BUILD_TESTS`    | `ON` | 构建单元测试 |
| `AGENT_BUILD_EXAMPLES` | `ON` | 构建示例程序 |

## 快速开始

### 运行 Agent CLI

```bash
# 设置 API Key
export ANTHROPIC_API_KEY="your-api-key"

# 可选：自定义 API 地址和模型
export ANTHROPIC_BASE_URL="https://api.anthropic.com"
export ANTHROPIC_MODEL="claude-sonnet-4-20250514"

# 运行
./build/agent
```

### 代码示例

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

    // 设置流式输出回调
    session->on_stream([](const std::string& text) {
        std::cout << text << std::flush;
    });

    // 设置工具调用回调
    session->on_tool_call([](const std::string& tool, const json& args) {
        std::cout << "[Tool: " << tool << "]\n";
    });

    session->on_complete([](FinishReason reason) {
        std::cout << "\n[Done]\n";
    });

    // 在后台线程运行 IO
    std::thread io_thread([&io_ctx]() {
        asio::io_context::work work(io_ctx);
        io_ctx.run();
    });

    // 发送消息，触发 Agent Loop
    session->prompt("帮我写一个 Hello World 程序");

    io_ctx.stop();
    io_thread.join();
}
```

## 项目结构

```
agent-cpp/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── core/           # 核心类型、消息、配置、UUID
│   ├── bus/            # 事件总线
│   ├── net/            # HTTP / SSE 客户端（基于 Asio + OpenSSL）
│   ├── llm/            # LLM Provider（Anthropic / OpenAI）
│   ├── tool/           # 工具系统（注册、权限、内置工具）
│   │   └── builtin/    # 内置工具实现
│   ├── session/        # 会话管理（Agent Loop、上下文压缩、截断）
│   ├── agent/          # Agent 框架入口
│   ├── mcp/            # MCP 客户端（WIP）
│   └── skill/          # Skill 系统（WIP）
├── examples/           # 示例程序
│   ├── agent.cpp       # 交互式 Agent CLI
│   ├── api_test.cpp    # API 调用测试
│   └── tool_test.cpp   # 工具系统测试
└── tests/              # 单元测试（GoogleTest）
    ├── test_message.cpp
    ├── test_tool.cpp
    ├── test_session.cpp
    └── test_llm.cpp
```

## TODO

- [ ] TUI 支持
- [ ] Skill 系统完善
- [ ] 会话持久化存储
- [ ] 提供 REST API，可作为 Server 使用
- [ ] C++20 协程（`co_await`）接口
- [ ] MCP 客户端完整实现
- [ ] 更多 LLM Provider（Gemini、本地模型等）
- [ ] 支持 Vision（图片输入）

## 相关文档

- [OpenCode核心架构分析](doc/OpenCode核心架构分析.md)

## 致谢

- [OpenCode](https://github.com/anomalyco/opencode) — 架构参考
- [Claude Code](https://docs.anthropic.com/en/docs/claude-code) — 功能参考

## License

MIT
