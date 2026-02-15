# agent-sdk

agent sdk for cpp, like claude code sdk but lightweight.

中文 | [English](README_EN.md)

## agent_cli

![agent_cli.png](doc/img/agent_cli.png)

## 为什么用 C++ 实现？

现有的 AI Agent 项目（Claude Code、OpenCode、OpenClaw 等）普遍存在两个问题：

- **复杂度高**：这些项目功能全面，但对于只需要核心 Agent 能力的场景并不适合，几乎无法拆分做二次开发。
- **运行时笨重**：基于Node.js、TypeScript技术栈，依赖Node运行时，打包后近百兆，嵌入到项目的代价太大。

agent-sdk 选择 C++ 正是为了解决这些痛点：编译产物是一个轻量的原生静态库，零运行时依赖，可以轻松嵌入任何 C/C++
项目，甚至可以在嵌入式环境中运行。

| 二进制（macOS）       | 大小       |
|------------------|----------|
| claude v2.1.39   | 174M     |
| opencode v1.1.65 | 101M     |
| **agent-sdk**    | **1~3M** |

## 项目目标

- **保持简单，容易理解**：代码结构清晰，模块化设计
- **包含主流基本功能**：类似 Claude Code / OpenCode 的核心能力
- **作为库/框架使用**：提供 C++ 静态库 `agent_sdk`，可嵌入到其他项目

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
| `skill`    | 按需加载 Skill 指令            |

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

1. 项目级配置：`.agent-sdk/config.json`
2. 全局配置：`~/.config/agent-sdk/config.json`
3. 指令文件：层级搜索 `AGENTS.md`，兼容 `CLAUDE.md`、`.agents/`、`.claude/`、`.opencode/` 等多种规范

### 🌐 MCP 支持（WIP）

Model Context Protocol 客户端，支持：

- 本地 stdio 传输
- 远程 SSE 传输

### 📦 Skill 系统

兼容 OpenCode、Claude Code 等主流 AI Agent 工具的 Skill 规范，支持跨工具共享技能包：

- **多规范兼容**：自动搜索 `.agent-sdk/`、`.agents/`、`.claude/`、`.opencode/` 目录下的 `SKILL.md`
- **层级发现**：从项目目录向上遍历到 git 根目录，收集所有 skill
- **全局共享**：支持 `~/.agents/skills/` 等全局目录，与其他 Agent 工具共享 skill
- **按需加载**：LLM 通过内置 `skill` 工具按需加载，不浪费上下文窗口
- **标准格式**：YAML frontmatter + Markdown 正文，包含名称、描述、许可证等元数据

详见 [Skill 系统设计文档](doc/skill-system.md)。

## 依赖

| 依赖                                                 | 版本     | 用途          |
|----------------------------------------------------|--------|-------------|
| C++                                                | 20     | 语言标准        |
| [Asio](https://github.com/chriskohlhoff/asio)      | 1.36.0 | 异步 I/O 与网络  |
| [spdlog](https://github.com/gabime/spdlog)         | 1.17.0 | 日志          |
| [nlohmann/json](https://github.com/nlohmann/json)  | 3.12.0 | JSON 解析     |
| [FTXUI](https://github.com/ArthurSonzogni/FTXUI)   | 6.1.9  | TUI 终端界面    |
| [OpenSSL](https://www.openssl.org/)                | —      | HTTPS / TLS |
| [GoogleTest](https://github.com/google/googletest) | 1.14.0 | 单元测试（可选）    |

> 除 OpenSSL 需要系统安装外，其他依赖均通过 git submodule 管理，位于 `thirdparty/` 目录。

## 构建

### 前置要求

- CMake ≥ 3.20
- 支持 C++20 的编译器（GCC 12+ / Clang 15+ / MSVC 2022+）
- OpenSSL 开发库

### 编译步骤

```bash
# 克隆项目（包含子模块）
git clone --recursive https://github.com/shuai132/agent-sdk.git
cd agent-sdk

# 如果已克隆但未拉取子模块
git submodule update --init --recursive

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
# 运行
./build/agent_cli

# 查看帮助
./build/agent_cli --help
```

**环境变量配置**（三选一）：

```bash
# 方式一：Qwen Portal（OAuth 认证，无需 API Key，推荐）
export QWEN_OAUTH=1
# 可选覆盖：
export QWEN_BASE_URL="https://portal.qwen.ai"
export QWEN_MODEL="coder-model"
# 首次使用会显示二维码进行登录认证

# 方式二：Anthropic
export ANTHROPIC_API_KEY="your-api-key"
# 可选覆盖：
export ANTHROPIC_BASE_URL="https://api.anthropic.com"
export ANTHROPIC_MODEL="claude-sonnet-4-20250514"

# 方式三：OpenAI（或兼容 API）
export OPENAI_API_KEY="your-api-key"
# 可选覆盖：
export OPENAI_BASE_URL="https://api.openai.com"
export OPENAI_MODEL="gpt-4o"
```

> **优先级**：`QWEN_OAUTH` > `OPENAI_API_KEY`（当同时设置时 Qwen OAuth 优先）

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

## TUI 终端界面（agent_cli）

agent-sdk 提供了一个功能完整的 TUI（Terminal User Interface）应用 `agent_cli`
，基于 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 构建。

### 运行

```bash
# 设置 API Key
export ANTHROPIC_API_KEY="your-key"
# 或
export OPENAI_API_KEY="your-key"

# 运行 TUI
./build/agent_cli
```

### 功能特性

- ✅ **实时流式输出**：LLM 响应实时显示
- ✅ **工具调用可视化**：工具卡片显示，支持点击展开/折叠详情
- ✅ **命令补全**：输入 `/` 自动显示命令菜单
- ✅ **会话管理**：创建、切换、删除会话
- ✅ **滚动查看**：支持鼠标滚轮和 PageUp/PageDown
- ✅ **模式切换**：Build / Plan 模式快速切换（Tab 键）
- ✅ **复制聊天**：一键复制聊天内容到剪贴板

### 交互方式

#### 键盘快捷键

| 快捷键           | 功能               |
|---------------|------------------|
| `Tab`         | 切换 Build/Plan 模式 |
| `Esc`         | 中断运行中的 Agent     |
| `Ctrl+C`      | 按两次退出            |
| `PageUp/Down` | 滚动聊天记录           |
| `↑/↓`         | 命令菜单导航           |

#### 鼠标交互

| 操作      | 功能           |
|---------|--------------|
| 点击工具卡片  | 展开/折叠工具调用详情  |
| 滚轮      | 滚动聊天历史       |
| 点击会话列表项 | 选择会话（在会话面板中） |

#### 命令

| 命令          | 快捷键  | 功能         |
|-------------|------|------------|
| `/quit`     | `/q` | 退出程序       |
| `/clear`    | -    | 清空聊天记录     |
| `/help`     | `/h` | 显示帮助信息     |
| `/sessions` | `/s` | 打开会话列表面板   |
| `/expand`   | -    | 展开所有工具调用   |
| `/collapse` | -    | 折叠所有工具调用   |
| `/copy`     | `/c` | 复制聊天内容到剪贴板 |
| `/compact`  | -    | 触发上下文压缩    |

### 会话管理

在会话列表面板中（使用 `/sessions` 打开）：

- `↑/↓` 或 `j/k`：导航
- `Enter`：加载选中的会话
- `d`：删除选中的会话
- `n`：创建新会话
- `Esc`：关闭面板

## 项目结构

```
agent-sdk/
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
│   └── skill/          # Skill 系统（发现、解析、注册）
├── examples/           # 示例程序
│   ├── agent.cpp       # 交互式 Agent CLI
│   ├── api_test.cpp    # API 调用测试
│   └── tool_test.cpp   # 工具系统测试
└── tests/              # 单元测试（GoogleTest）
    ├── test_message.cpp
    ├── test_tool.cpp
    ├── test_session.cpp
    ├── test_llm.cpp
    └── test_skill.cpp
```

## TODO

- [x] TUI 支持
- [x] Skill 系统
- [x] 会话持久化存储
- [x] MCP 客户端完整实现
- [ ] 更多 LLM Provider（Gemini、本地模型等）
- [ ] 支持 Vision（图片输入）
- [ ] 提供 REST API，可作为 Server 使用
- [ ] C++20 协程（`co_await`）接口

## 相关文档

- [Skill 系统设计](doc/skill-system.md)
- [OpenCode核心架构分析](doc/OpenCode核心架构分析.md)

## 致谢

- [OpenCode](https://github.com/anomalyco/opencode) — 架构参考
- [Claude Code](https://docs.anthropic.com/en/docs/claude-code) — 功能参考

## License

MIT
