# OpenCode 核心架构深度分析

## 1. Agent Loop（代理循环）

OpenCode 的 agent loop 实现在 `packages/opencode/src/session/prompt.ts` 的 `SessionPrompt.loop()` 函数中，**核心循环并不是简单的 `while tool_calls`，而是一个更精密的状态机**。

### 主循环伪代码

```
SessionPrompt.loop(sessionID):
  abort = start(sessionID)       // 创建 AbortController
  step = 0

  while true:
    // 1. 从持久化存储加载消息（过滤已压缩的部分）
    msgs = MessageV2.filterCompacted(MessageV2.stream(sessionID))

    // 2. 反向扫描找到 lastUser / lastAssistant / lastFinished / tasks
    //    tasks 是 compaction 或 subtask 类型的 part

    // 3. 退出条件：最后一个 assistant 已完成且不是 tool-calls
    if lastAssistant.finish && finish != "tool-calls":
      break

    step++

    // 4. 处理挂起的 subtask（子代理任务）
    if task.type == "subtask":
      TaskTool.execute(...)   // 启动子 session
      continue

    // 5. 处理挂起的 compaction（上下文压缩）
    if task.type == "compaction":
      SessionCompaction.process(...)
      continue

    // 6. 上下文溢出检测 → 自动触发 compaction
    if isOverflow(lastFinished.tokens, model):
      SessionCompaction.create(...)
      continue

    // 7. 正常处理：构建工具、调用 LLM
    tools = resolveTools(agent, model, mcp_tools, ...)
    processor = SessionProcessor.create(assistantMessage)

    result = processor.process(streamInput)
    // result 是 "stop" | "continue" | "compact"

    if result == "compact":
      SessionCompaction.create(...)  // 下一轮会进入压缩流程
    if result == "stop":
      break

  // 循环结束后裁剪老旧工具输出
  SessionCompaction.prune(sessionID)
```

### 关键设计特点

**循环不直接解析 tool_calls**，而是依赖 Vercel AI SDK 的 `streamText` 内部循环：`streamText` 本身会自动处理 `tool-calls` → 执行工具 → 将结果追加到对话 → 再次调用 LLM。`SessionProcessor.process()` 内部只有一层 `while(true)` 来处理流事件，每个 step 完成后外部循环重新检查消息状态决定是否继续。

实际的工具执行流在 `processor.ts` 的 `process()` 方法中：

```ts
// processor.ts - process() 方法
while (true) {
  const stream = await LLM.stream(streamInput)  // 调用 streamText

  for await (const value of stream.fullStream) {
    switch (value.type) {
      case "tool-call":     // 工具调用开始 → 检测 doom loop
      case "tool-result":   // 工具返回结果 → 持久化
      case "tool-error":    // 工具错误 → 检测权限拒绝
      case "text-delta":    // 文本流 → 实时推送
      case "finish-step":   // 一轮完成 → 计算 token 用量 → 检查是否需要 compaction
    }
    if (needsCompaction) break
  }

  // 错误重试逻辑 (指数退避)
  if (retryable_error) {
    attempt++
    await SessionRetry.sleep(delay, abort)
    continue
  }

  // 返回 "continue" | "stop" | "compact"
}
```

---

## 2. 工具调用系统

### 工具注册机制 (`ToolRegistry`)

工具来源有 **三类**：

| 来源 | 加载方式 | 示例 |
|------|---------|------|
| **内置工具** | 硬编码数组 | `BashTool`, `ReadTool`, `EditTool`, `GrepTool`, `TaskTool`, `SkillTool` |
| **自定义工具** | 从 `{tool,tools}/*.{js,ts}` 动态导入 | 用户在 `.opencode/tool/` 目录下定义 |
| **Plugin 工具** | 通过 `Plugin.list()` 注册 | 插件系统提供 |

```ts
// registry.ts
async function all(): Promise<Tool.Info[]> {
  return [
    InvalidTool,          // 修复无效工具调用
    QuestionTool,         // 向用户提问
    BashTool,             // 执行 shell 命令
    ReadTool, GlobTool, GrepTool,  // 文件操作
    EditTool, WriteTool,  // 文件编辑
    TaskTool,             // 子代理调度
    SkillTool,            // 技能加载
    WebSearchTool, WebFetchTool,   // 网络访问
    CodeSearchTool,       // 代码搜索
    ApplyPatchTool,       // GPT 系列专用 patch 格式
    ...custom,            // 自定义 + 插件工具
  ]
}
```

### 工具定义接口

每个工具实现 `Tool.Info` 接口：

```ts
interface Info {
  id: string
  init: (ctx?: InitContext) => Promise<{
    description: string
    parameters: ZodType         // Zod schema 自动转 JSON Schema
    execute(args, ctx): Promise<{ title, metadata, output }>
  }>
}
```

**关键设计**：`Tool.define()` 工厂函数自动包装了：
- **参数验证** → Zod parse
- **输出截断** → `Truncate.output()` 超过 2000 行/50KB 自动保存全文到磁盘，返回截断版本 + 文件路径提示

### 工具权限控制

通过 `PermissionNext` 系统实现细粒度权限，每个 agent 有独立的权限规则集：

```ts
// build agent: 大部分允许
permission: { "*": "allow", question: "allow", ... }

// plan agent: 禁止编辑（除 plan 文件外）
permission: { edit: { "*": "deny", ".opencode/plans/*.md": "allow" } }

// explore agent: 只读工具
permission: { "*": "deny", grep: "allow", glob: "allow", read: "allow", ... }
```

### Doom Loop 检测

当同一工具被用**相同参数**连续调用 3 次时，自动触发 `doom_loop` 权限请求（需用户确认）：

```ts
if (lastThree.every(p =>
  p.tool === value.toolName &&
  JSON.stringify(p.state.input) === JSON.stringify(value.input)
)) {
  await PermissionNext.ask({ permission: "doom_loop", ... })
}
```

---

## 3. MCP (Model Context Protocol) 集成

`packages/opencode/src/mcp/index.ts` 实现了完整的 MCP 客户端。

### 连接管理

```
配置 (config.mcp) → 遍历每个 server → 根据 type 创建 transport:
  - "remote": 尝试 StreamableHTTP → 失败回退 SSE → 支持 OAuth
  - "local": StdioClientTransport (子进程)
→ client.connect(transport) → listTools() 验证
→ 状态: connected | disabled | failed | needs_auth
```

### 工具集成流程

MCP 工具在 `resolveTools()` 中与内置工具合并：

```ts
// prompt.ts resolveTools()
// 1. 加载内置工具
for (const item of await ToolRegistry.tools(model, agent)) {
  tools[item.id] = tool({ ... })
}

// 2. 加载 MCP 工具
for (const [key, item] of Object.entries(await MCP.tools())) {
  // MCP 工具执行前需要权限检查
  item.execute = async (args, opts) => {
    await ctx.ask({ permission: key, ... })        // 权限检查
    const result = await execute(args, opts)        // 调用 MCP server
    const truncated = await Truncate.output(...)    // 截断处理
    return { output: truncated.content, ... }
  }
  tools[key] = item
}
```

MCP 还支持 **Prompts**（作为 Commands 暴露）和 **Resources**（可在用户消息中附加）。

---

## 4. Skill 技能系统

### 设计理念

Skill 是**按需加载的领域知识包**，不是预加载到 system prompt 中，而是 LLM 在识别到相关任务时主动调用 `skill` 工具来加载。

### 技能发现

从多个目录扫描 `SKILL.md` 文件：

```
扫描顺序（后面覆盖前面）:
1. ~/.claude/skills/**/ , ~/.agents/skills/**/  (全局外部目录)
2. 项目目录向上查找 .claude/skills/ , .agents/skills/  (项目级外部)
3. .opencode/skill/**/ , .opencode/skills/**/  (opencode 专属)
4. config.skills.paths 中的自定义路径
```

每个 `SKILL.md` 的 frontmatter 定义 `name` 和 `description`：

```markdown
---
name: deploy-aws
description: AWS deployment workflow with CDK
---
具体的技能指令内容...
```

### 技能加载流程

```
LLM 看到 skill 工具描述中的 <available_skills> 列表
→ 识别匹配任务 → 调用 skill 工具(name="deploy-aws")
→ SkillTool.execute():
  1. 从 Skill.get(name) 获取内容
  2. 权限检查
  3. 扫描 skill 目录附近文件（最多 10 个）
  4. 返回 <skill_content> 块 + 目录文件列表
→ 技能内容注入到对话上下文
```

### 扩展性设计

- **零代码添加**: 只需在约定目录放 `SKILL.md` 文件
- **兼容 Claude Code**: 共享 `.claude/skills/` 目录格式
- **权限隔离**: 每个 agent 可独立控制哪些 skill 可用
- **Compaction 保护**: `prune()` 时 `skill` 工具的输出不被清除（`PRUNE_PROTECTED_TOOLS = ["skill"]`）

---

## 5. 上下文管理与压缩

### 为什么需要上下文管理？

OpenCode 的 session 可能涉及**数十次甚至上百次**工具调用，token 消耗会线性增长直至超过模型上下文窗口。需要解决：

1. **上下文溢出**: Claude 200K tokens 看似很大，但一次 `bash ls -R` 就可能产生 50K tokens
2. **成本控制**: 输入 tokens 是累积计费的，重复发送无用历史会浪费大量成本
3. **相关性衰减**: 早期工具调用结果（如旧版文件内容）可能已经过时

### 三层上下文管理策略

#### 层 1: 输出截断 (Truncate)

**最先生效**，在工具返回结果时立即处理：

```ts
// 超过 2000 行或 50KB → 保存全文到磁盘 → 返回截断版本
Truncate.output(text, { maxLines: 2000, maxBytes: 50*1024 })
// → "...3000 lines truncated... Full output saved to: /path/to/tool_xxx"
// → 提示 LLM 用 Grep/Read 或委托 explore agent 处理
```

#### 层 2: 裁剪 (Prune)

**每次循环结束时**运行，清除旧工具输出：

```ts
SessionCompaction.prune():
  // 从最新消息向前扫描
  // 保留最近 40,000 tokens 的工具输出
  // 超过的标记为 compacted → "[Old tool result content cleared]"
  // 保护 skill 工具输出不被清除
```

- `PRUNE_PROTECT = 40_000` tokens: 最近的工具输出保护区
- `PRUNE_MINIMUM = 20_000` tokens: 只有清除量足够大才执行
- 被清除的 part 设置 `state.time.compacted` 时间戳

#### 层 3: 压缩 (Compaction) — 摘要式重写

**上下文溢出时自动触发**，这是最重要的机制：

```
检测: isOverflow() → input_tokens > model.limit.input
  ↓
创建 compaction 标记消息 (CompactionPart)
  ↓
下一轮循环检测到 task.type == "compaction"
  ↓
SessionCompaction.process():
  1. 使用 "compaction" agent（无工具权限）
  2. 将完整历史消息 + 压缩提示发给 LLM:
     "Provide a detailed prompt for continuing our conversation...
      Focus on what we did, what we're doing, which files, what's next"
  3. LLM 输出摘要 → 标记 assistant.summary = true
  4. 自动注入 "Continue if you have next steps" 用户消息
  ↓
filterCompacted() 过滤:
  从最新消息向前扫描 → 遇到已完成的 summary 消息 → 截断
  → 只保留: [summary, 后续消息...]
```

**关键函数 `filterCompacted()`**:

```ts
async function filterCompacted(stream) {
  const result = []
  const completed = new Set()
  for await (const msg of stream) {  // 从最新消息向前迭代
    result.push(msg)
    // 如果遇到 user 消息中有 compaction part，且对应 summary 已完成
    if (msg.role === "user" && completed.has(msg.id) && hasCompactionPart)
      break  // 截断！
    // 记录已完成的 summary 对应的 parentID
    if (msg.role === "assistant" && msg.summary && msg.finish)
      completed.add(msg.parentID)
  }
  result.reverse()
  return result  // 只返回 [compaction之后的消息]
}
```

这意味着**压缩后，老消息物理上还在存储中，但逻辑上被过滤掉**，只有摘要 + 后续消息进入 LLM。

---

## 6. 按需上下文加载

OpenCode 实现了多种"按需"机制来避免一次性加载过多上下文。

### AGENTS.md 层级加载

```ts
// instruction.ts - resolve()
// 读取文件时，自动向上查找目录层级中的 AGENTS.md
// 比如读取 src/components/Button.tsx 时，
// 会自动加载 src/components/AGENTS.md（如果存在且未加载过）
InstructionPrompt.resolve(messages, filepath, messageID)
```

使用 `claim` 机制防止同一消息重复加载同一文件。

### 系统指令异步加载

```ts
// instruction.ts - system()
// 加载优先级:
// 1. 项目级 AGENTS.md / CLAUDE.md (findUp)
// 2. 全局 ~/.config/opencode/AGENTS.md
// 3. config.instructions 中的文件/URL
// URL 支持远程加载（5 秒超时）
```

### 工具提示中的延迟加载

- `SkillTool`: 描述中列出所有可用 skill，LLM 按需调用加载
- `TaskTool`: 描述中列出可用 subagent，按需创建子 session
- `ReadTool`: 文件内容只在调用时读取，不预加载

### 用户消息中的文件附加

用户可以通过 `@file.ts` 语法附加文件，系统自动：
- 文本文件 → 调用 `ReadTool` 读取并截断
- 目录 → 调用 `ListTool` 列出内容
- MCP 资源 → 调用 `MCP.readResource()`
- 图片 → base64 编码为 `data:` URL

---

## 7. 子代理（Subagent）系统

### 架构

```
Parent Session (build agent)
  ↓ TaskTool.execute()
  ↓ Session.create({ parentID })  // 创建子 session
  ↓ SessionPrompt.prompt()        // 独立循环
  ↓ 结果返回给 parent

Child Session (explore/general agent)
  - 独立的消息历史
  - 独立的权限（默认禁用 todowrite/todoread）
  - 可通过 task_id 恢复之前的子 session
```

### Subtask 队列机制

opencode 支持**在循环迭代之间排队子任务**：

```ts
// prompt.ts loop() 中
let tasks = []  // 收集 compaction 和 subtask parts
for (let i = msgs.length - 1; i >= 0; i--) {
  // 在未完成的 assistant 消息中查找 subtask part
  const task = msg.parts.filter(p => p.type === "compaction" || p.type === "subtask")
  if (!lastFinished) tasks.push(...task)
}
// tasks.pop() → 处理一个 pending subtask
```

这允许 LLM 在一次回复中安排多个子任务，由 loop 逐个处理。

---

## 8. 主动唤醒机制

OpenCode **没有传统意义上的"主动唤醒"（如定时触发 agent 自主行动）**。它的"唤醒"体现在以下方面：

### Scheduler（定时任务）

`Scheduler` 是一个简单的 `setInterval` 注册表，**只用于后台维护任务**：

```ts
// 目前只有一个注册：
Scheduler.register({
  id: "tool.truncation.cleanup",   // 清理 7 天前的截断文件
  interval: HOUR_MS,               // 每小时运行
  scope: "global",
})
```

**不会主动触发 LLM 调用。**

### 自动继续机制

真正的"主动"行为是 **compaction 后的自动继续**：

```ts
// compaction.ts process()
if (result === "continue" && input.auto) {
  // 自动注入 "Continue if you have next steps" 用户消息
  await Session.updatePart({
    text: "Continue if you have next steps",
    synthetic: true,  // 标记为系统生成
  })
}
```

这确保 compaction 后 agent 不会静默停止，而是继续执行未完成的任务。

### 中断恢复

如果用户在 agent 执行过程中发送新消息：

```ts
// prompt.ts loop()
if (step > 1 && lastFinished) {
  // 用 <system-reminder> 包裹新消息
  part.text = [
    "<system-reminder>",
    "The user sent the following message:",
    part.text,
    "Please address this message and continue with your tasks.",
    "</system-reminder>",
  ].join("\n")
}
```

### MCP 工具变更通知

```ts
client.setNotificationHandler(ToolListChangedNotificationSchema, async () => {
  Bus.publish(ToolsChanged, { server: serverName })
})
```

当 MCP server 工具列表变化时，通过 Bus 事件通知 UI 刷新。

---

## 9. 总结架构图

```
┌─────────────────────────────────────────────────┐
│                  SessionPrompt.loop()            │
│  ┌────────────────────────────────────────────┐  │
│  │  while true:                               │  │
│  │    msgs = filterCompacted(stream)          │  │  ← 上下文过滤
│  │    check subtask → TaskTool (child session) │  │  ← 子代理调度
│  │    check compaction → summarize + continue  │  │  ← 上下文压缩
│  │    check overflow → create compaction       │  │  ← 溢出检测
│  │    resolveTools(builtin + MCP + custom)     │  │  ← 工具解析
│  │    processor.process(LLM.stream())          │  │  ← LLM 调用
│  │      ├─ tool-call → execute → tool-result   │  │
│  │      ├─ doom loop detection                 │  │
│  │      ├─ permission checks                   │  │
│  │      └─ truncation                          │  │
│  │    prune old tool outputs                   │  │  ← 裁剪
│  └────────────────────────────────────────────┘  │
│                                                   │
│  System Prompt = provider_prompt                  │
│               + environment info                  │
│               + AGENTS.md (hierarchical)          │
│               + config.instructions               │
│               + plugin transforms                 │
│                                                   │
│  Tools = ToolRegistry(builtin + custom + plugin)  │
│        + MCP.tools()                              │
│        + SkillTool (lazy-load skills)             │
└─────────────────────────────────────────────────┘
```

这是一个非常工程化的 agent 实现：**没有长期向量记忆数据库，没有主动唤醒调度器**，而是通过**分层截断 + 裁剪 + 摘要压缩**来管理上下文窗口，通过**子 session + skill 按需加载**来实现知识扩展，通过**compaction 自动继续**来保持 agent 连续工作。
