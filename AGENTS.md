# AGENTS.md — agent-sdk

轻量级 C++20 AI Agent SDK，基于 Asio 和 OpenSSL 构建。编译为原生静态库 （`libagent_sdk.a`），零运行时依赖。

## 构建命令

```bash
# 配置（在项目根目录下执行）
cmake -B build

# 构建全部（库 + 示例 + 测试）
cmake --build build -j$(nproc)

# 仅构建库
cmake --build build --target agent_sdk

# 仅构建测试
cmake --build build --target agent_tests
```

**前置条件**：CMake >= 3.20、支持 C++20 的编译器、系统安装的 OpenSSL。
其余依赖（nlohmann/json、asio、spdlog、ftxui、googletest）通过 git submodule 管理，位于 `thirdparty/` 目录。
克隆项目后需要执行 `git submodule update --init --recursive` 来拉取依赖。

## 测试命令

- 测试框架：**GoogleTest v1.14.0**，通过 `gtest_discover_tests` 自动发现。
- 执行测试时，需要添加超时，因为有可能阻塞，尤其是[examples](examples)。

```bash
# 运行所有测试
ctest --test-dir build

# 直接运行所有测试（详细输出）
./build/agent_tests

# 按完整名称运行单个测试
./build/agent_tests --gtest_filter=MessageTest.CreateUserMessage

# 运行某个测试套件下的所有测试
./build/agent_tests --gtest_filter=MessageTest.*

# 通过 CTest 按名称过滤运行
ctest --test-dir build -R "MessageTest"
```

## 代码检查 / 格式化

```bash
# 格式化所有源文件（clang-format，基于 Google 风格）
find examples src tests tui -type f | xargs clang-format -i
```

配置文件为 `.clang-format`：基于 Google 风格，150 列宽度限制，仅允许空的短函数/Lambda 写在单行。
未配置 clang-tidy 或 editorconfig。

## 项目结构

```
src/
  core/       # 基础类型、Message、Config、UUID、JsonStore、Version
  bus/        # 类型安全的事件总线
  net/        # HTTP / SSE 客户端（Asio + OpenSSL，PIMPL 模式）
  llm/        # LLM 提供商（Anthropic、OpenAI、Ollama）
  tool/       # 工具注册表、权限系统、9 个内置工具
  session/    # 会话管理、Agent Loop、上下文压缩与截断、Prompt 处理
  agent/      # Agent 框架入口
  mcp/        # MCP 客户端（已实现 Transport 和 Client）
  skill/      # 技能系统（已实现基础功能）
examples/     # simple_chat、api_test、tool_test
tests/        # GoogleTest 单元测试（15 个测试文件）
tui/          # agent_cli TUI 应用（基于 FTXUI）
```

## LLM 提供商支持

SDK 支持多种 LLM 提供商，通过统一的 `Provider` 接口提供一致的体验。

### 支持的提供商

| 提供商           | 类名                  | 描述              | 特殊功能                |
|---------------|---------------------|-----------------|---------------------|
| **OpenAI**    | `OpenAIProvider`    | OpenAI GPT 系列模型 | 原生支持 reasoning、工具调用 |
| **Anthropic** | `AnthropicProvider` | Claude 系列模型     | 支持工具调用、系统提示         |  
| **Ollama**    | `OllamaProvider`    | 本地 LLM 服务器      | 自动模型发现、思考块解析        |

### Ollama 本地支持

`OllamaProvider` 通过继承 `OpenAIProvider` 实现，提供完整的本地 LLM 支持：

#### 核心特性

- **OpenAI 兼容 API**：使用 `/v1/chat/completions` 端点
- **自动模型发现**：通过 `/api/tags` 获取本地可用模型
- **思考块解析**：自动识别 `<think></think>` 标签，提取推理过程
- **流式 & 非流式**：支持实时流式输出和批量响应
- **工具调用支持**：完整的函数调用能力
- **默认配置**：自动连接 `http://localhost:11434`

#### 架构设计

```cpp
class OllamaProvider : public OpenAIProvider {
  // 继承复用所有 OpenAI 兼容逻辑
  std::vector<ModelInfo> models() const override;  // 仅重写模型发现
};
```

#### 配置示例

```json
{
  "provider": "ollama",
  "model": "deepseek-r1:7b",
  "base_url": "http://localhost:11434"
}
```

#### 支持的模型类型

- **推理模型**：DeepSeek-R1、Qwen2.5-Math 等（自动解析思考过程）
- **对话模型**：Llama、Qwen、ChatGLM 等
- **代码模型**：CodeLlama、CodeQwen 等
- **工具调用模型**：支持 OpenAI 函数调用格式的任意模型

### 继承复用架构

为减少代码重复，采用继承复用设计：

- **`OpenAIProvider`**：提供完整的 OpenAI 兼容实现
- **`OllamaProvider`**：继承并仅重写差异化功能（模型发现）
- **优势**：90% 代码复用、自动获得新特性、行为一致性保证

## 代码风格指南

风格基于 **Google C++**（由 `.clang-format` 强制执行），并做了以下调整。
缩进：2 空格；列宽限制：150；K&R 大括号风格。访问修饰符缩进 1 空格。
嵌套命名空间使用 C++17 风格（`namespace agent::llm {`），以 `}  // namespace agent::llm` 结尾。
`using namespace` 仅允许在测试/示例中使用，**禁止**出现在头文件中。

### 命名规范

| 元素            | 规范                 | 示例                                          |
|---------------|--------------------|---------------------------------------------|
| 类 / 结构体       | `PascalCase`       | `Session`、`ToolRegistry`、`SseEvent`         |
| 成员函数          | `snake_case`       | `add_message()`、`get_context_messages()`    |
| 自由函数          | `snake_case`       | `to_string()`、`register_builtins()`         |
| 私有成员变量        | `snake_case_`      | `id_`、`config_`、`io_ctx_`                   |
| 公有结构体字段       | `snake_case`       | `input_tokens`、`base_url`                   |
| 局部变量          | `snake_case`       | `stream_complete`、`finish_reason`           |
| 枚举类型          | `PascalCase`       | `FinishReason`、`Role`、`Permission`          |
| 枚举值           | `PascalCase`       | `FinishReason::ToolCalls`、`Role::Assistant` |
| 命名空间          | `snake_case`       | `agent`、`agent::llm`、`agent::tools`         |
| 类型别名          | `PascalCase`       | `SessionId`、`StreamCallback`                |
| 常量（constexpr） | `UPPER_SNAKE_CASE` | `DEFAULT_TIMEOUT_MS`                        |
| 文件名           | `snake_case`       | `http_client.hpp`、`test_message.cpp`        |
| 模板参数          | `PascalCase`       | `Result<T>` 中的 `T`                          |

### 头文件

- 扩展名：始终使用 `.hpp`（不使用 `.h`）
- 头文件保护：始终使用 `#pragma once`（不使用 `#ifndef` 宏守卫）
- 谨慎使用前置声明以打破循环依赖
- 网络层（`HttpClient`、`SseClient`）使用 PIMPL 模式

### 头文件包含顺序

```cpp
#include "module/this_file.hpp"   // 1. 对应的头文件

#include <string>                 // 2. C++ 标准库
#include <vector>

#include <spdlog/spdlog.h>        // 3. 第三方库
#include <nlohmann/json.hpp>

#include "core/types.hpp"         // 4. 项目内部头文件
#include "llm/provider.hpp"
```

- 系统/第三方头文件使用 `<>`，项目内部头文件使用 `""`
- 项目内部包含路径相对于 `src/`（例如 `"core/types.hpp"`）
- 各组之间用空行分隔

### 类型模式

- `std::variant` 用于和类型（Sum Type）：`MessagePart`、`StreamEvent`
- `std::optional` 用于可空字段
- `std::shared_ptr` 用于共享所有权（工具、提供商、会话）
- `std::unique_ptr` 用于 PIMPL 和独占所有权
- `std::weak_ptr` 用于非拥有的反向引用（子会话）
- `auto` 用于迭代器、工厂返回值、`std::get_if` 结果 —— 不用于基本类型
- 常用别名：`using json = nlohmann::json;`、`namespace fs = std::filesystem;`

### 错误处理

- **`Result<T>`**（`core/types.hpp`）：`Result::success(val)` / `Result::failure(msg)` 用于验证
- **`ToolResult::error(msg)`**：工具执行失败的工厂方法
- **`std::optional<std::string> error`** + `bool ok()`：用于 `LlmResponse`、`HttpResponse`
- **`try-catch`**：在边界处使用（JSON 解析、工具执行）；捕获 `const std::exception &e`
- **日志**：使用 `spdlog::warn/info/debug` 输出诊断信息 —— 库代码中禁止使用 `std::cout`
- 不使用自定义异常类；生产代码中不使用 `assert()`

### 设计模式与参数传递

- **单例**（Meyers 单例）：`ToolRegistry::instance()`、`Bus::instance()`、`ProviderFactory::instance()`
- **工厂方法**：`Session::create(...)`、`Message::system(...)`、`ToolResult::success(...)`
- **回调注册**：`session->on_stream(...)`、`session->on_tool_call(...)`
- **抽象基类**：`Provider`、`Tool`、`MessageStore`，使用 `virtual` + `override`
- **便捷基类**：`SimpleTool` 继承自 `Tool`
- **`std::enable_shared_from_this`**：`Session` 中使用
- Getter 始终为 `const`：`const std::string &id() const`
- 大对象通过 `const &` 传递；小类型/枚举按值传递
- 所有权转移使用移动语义；通过返回值输出结果，不使用输出参数

### 使用现代 C++ 特性（C++20）

### 提交风格

使用约定式提交（Conventional Commits），提交信息使用中文：

- `feat: 实现 TaskTool 子代理功能`
- `fix: 修复流式响应中工具调用的ID和name丢失问题`

## 代码检索

- 请忽略`build`、`cmake-build-*`等文件夹，这些是构建产物。
- `thirdparty`里都是三方库目录，不需要修改。
