#include <gtest/gtest.h>

#include <asio.hpp>
#include <chrono>
#include <future>
#include <thread>

#include "agent/agent.hpp"

// 测试核心组件（不依赖 FTXUI）
#include "../tui/tui_components.h"

using namespace agent_cli;

// ============================================================
// ChatLog 测试
// ============================================================

TEST(ChatLogTest, PushAndSnapshot) {
  ChatLog log;
  log.push({EntryKind::UserMsg, "Hello", ""});
  log.push({EntryKind::AssistantText, "Hi there!", ""});

  auto entries = log.snapshot();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].kind, EntryKind::UserMsg);
  EXPECT_EQ(entries[0].text, "Hello");
  EXPECT_EQ(entries[1].kind, EntryKind::AssistantText);
  EXPECT_EQ(entries[1].text, "Hi there!");
}

TEST(ChatLogTest, AppendStream) {
  ChatLog log;
  log.append_stream("Hello ");
  log.append_stream("World");

  auto entries = log.snapshot();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].kind, EntryKind::AssistantText);
  EXPECT_EQ(entries[0].text, "Hello World");
}

TEST(ChatLogTest, AppendStreamCreatesNewEntry) {
  ChatLog log;
  // 先添加一个非 AssistantText entry
  log.push({EntryKind::UserMsg, "test", ""});
  // append_stream 应该创建新的 entry
  log.append_stream("response");

  auto entries = log.snapshot();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].kind, EntryKind::UserMsg);
  EXPECT_EQ(entries[1].kind, EntryKind::AssistantText);
  EXPECT_EQ(entries[1].text, "response");
}

TEST(ChatLogTest, AppendStreamToExistingAssistant) {
  ChatLog log;
  log.push({EntryKind::AssistantText, "first ", ""});
  log.append_stream("second");

  auto entries = log.snapshot();
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].text, "first second");
}

TEST(ChatLogTest, Clear) {
  ChatLog log;
  log.push({EntryKind::UserMsg, "test1", ""});
  log.push({EntryKind::AssistantText, "test2", ""});
  EXPECT_EQ(log.size(), 2);

  log.clear();
  EXPECT_EQ(log.size(), 0);
  EXPECT_TRUE(log.snapshot().empty());
}

TEST(ChatLogTest, Last) {
  ChatLog log;
  log.push({EntryKind::UserMsg, "first", ""});
  log.push({EntryKind::Error, "oops", "detail"});

  auto last = log.last();
  EXPECT_EQ(last.kind, EntryKind::Error);
  EXPECT_EQ(last.text, "oops");
  EXPECT_EQ(last.detail, "detail");
}

TEST(ChatLogTest, LastEmpty) {
  ChatLog log;
  auto last = log.last();
  EXPECT_EQ(last.kind, EntryKind::SystemInfo);
  EXPECT_TRUE(last.text.empty());
}

TEST(ChatLogTest, Filter) {
  ChatLog log;
  log.push({EntryKind::UserMsg, "msg1", ""});
  log.push({EntryKind::AssistantText, "resp1", ""});
  log.push({EntryKind::ToolCall, "bash", "{}"});
  log.push({EntryKind::ToolResult, "bash [OK]", "done"});
  log.push({EntryKind::UserMsg, "msg2", ""});
  log.push({EntryKind::AssistantText, "resp2", ""});

  auto user_msgs = log.filter(EntryKind::UserMsg);
  ASSERT_EQ(user_msgs.size(), 2);
  EXPECT_EQ(user_msgs[0].text, "msg1");
  EXPECT_EQ(user_msgs[1].text, "msg2");

  auto tool_calls = log.filter(EntryKind::ToolCall);
  ASSERT_EQ(tool_calls.size(), 1);
  EXPECT_EQ(tool_calls[0].text, "bash");
}

TEST(ChatLogTest, ThreadSafety) {
  ChatLog log;
  constexpr int num_threads = 8;
  constexpr int num_ops = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&log, t]() {
      for (int i = 0; i < num_ops; ++i) {
        if (i % 3 == 0) {
          log.push({EntryKind::UserMsg, "thread" + std::to_string(t) + "_" + std::to_string(i), ""});
        } else if (i % 3 == 1) {
          log.append_stream("delta_" + std::to_string(t) + "_" + std::to_string(i));
        } else {
          log.snapshot();  // 并发读
        }
      }
    });
  }
  for (auto& t : threads) t.join();

  // 只要不崩溃就算通过
  EXPECT_GT(log.size(), 0);
}

// ============================================================
// ToolPanel 测试
// ============================================================

TEST(ToolPanelTest, StartAndFinishTool) {
  ToolPanel panel;
  panel.start_tool("bash", "ls -la");
  EXPECT_EQ(panel.tool_status("bash"), "running");
  EXPECT_EQ(panel.size(), 1);

  panel.finish_tool("bash", "file1.txt\nfile2.txt", false);
  EXPECT_EQ(panel.tool_status("bash"), "done");
}

TEST(ToolPanelTest, FinishToolWithError) {
  ToolPanel panel;
  panel.start_tool("read", "/nonexistent");
  panel.finish_tool("read", "file not found", true);
  EXPECT_EQ(panel.tool_status("read"), "error");
}

TEST(ToolPanelTest, MultipleSameToolInstances) {
  ToolPanel panel;
  panel.start_tool("bash", "echo hello");
  panel.finish_tool("bash", "hello", false);
  panel.start_tool("bash", "echo world");

  // 最新的状态应该是 running（第二次调用）
  EXPECT_EQ(panel.tool_status("bash"), "running");

  panel.finish_tool("bash", "world", false);
  EXPECT_EQ(panel.tool_status("bash"), "done");
  EXPECT_EQ(panel.size(), 2);
}

TEST(ToolPanelTest, Snapshot) {
  ToolPanel panel;
  panel.start_tool("bash", "cmd1");
  panel.start_tool("read", "file.txt");
  panel.finish_tool("bash", "ok", false);

  auto snap = panel.snapshot();
  ASSERT_EQ(snap.size(), 2);
  EXPECT_EQ(snap[0].tool_name, "bash");
  EXPECT_EQ(snap[0].status, "done");
  EXPECT_EQ(snap[1].tool_name, "read");
  EXPECT_EQ(snap[1].status, "running");
}

TEST(ToolPanelTest, SnapshotLimit) {
  ToolPanel panel;
  // 添加 60 个工具活动
  for (int i = 0; i < 60; ++i) {
    panel.start_tool("tool_" + std::to_string(i), "args");
    panel.finish_tool("tool_" + std::to_string(i), "ok", false);
  }
  EXPECT_EQ(panel.size(), 60);

  // snapshot 最多返回 50 个
  auto snap = panel.snapshot();
  EXPECT_EQ(snap.size(), 50);
  // 应该是最新的 50 个
  EXPECT_EQ(snap[0].tool_name, "tool_10");
}

TEST(ToolPanelTest, Clear) {
  ToolPanel panel;
  panel.start_tool("bash", "test");
  panel.clear();
  EXPECT_EQ(panel.size(), 0);
  EXPECT_TRUE(panel.snapshot().empty());
}

TEST(ToolPanelTest, ThreadSafety) {
  ToolPanel panel;
  constexpr int num_threads = 4;
  constexpr int num_ops = 50;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&panel, t]() {
      for (int i = 0; i < num_ops; ++i) {
        std::string name = "tool_" + std::to_string(t) + "_" + std::to_string(i);
        panel.start_tool(name, "args");
        panel.snapshot();
        panel.finish_tool(name, "done", false);
      }
    });
  }
  for (auto& t : threads) t.join();

  EXPECT_EQ(panel.size(), num_threads * num_ops);
}

// ============================================================
// 命令解析测试
// ============================================================

TEST(CommandTest, ParseQuit) {
  auto cmd = parse_command("/q");
  EXPECT_EQ(cmd.type, CommandType::Quit);

  cmd = parse_command("/quit");
  EXPECT_EQ(cmd.type, CommandType::Quit);
}

TEST(CommandTest, ParseClear) {
  auto cmd = parse_command("/clear");
  EXPECT_EQ(cmd.type, CommandType::Clear);
}

TEST(CommandTest, ParseCompact) {
  auto cmd = parse_command("/compact");
  EXPECT_EQ(cmd.type, CommandType::Compact);
}

TEST(CommandTest, ParseExpand) {
  auto cmd = parse_command("/expand");
  EXPECT_EQ(cmd.type, CommandType::Expand);
}

TEST(CommandTest, ParseCollapse) {
  auto cmd = parse_command("/collapse");
  EXPECT_EQ(cmd.type, CommandType::Collapse);
}

TEST(CommandTest, ParseHelp) {
  auto cmd = parse_command("/h");
  EXPECT_EQ(cmd.type, CommandType::Help);

  cmd = parse_command("/help");
  EXPECT_EQ(cmd.type, CommandType::Help);
}

TEST(CommandTest, ParseSessions) {
  auto cmd = parse_command("/s");
  EXPECT_EQ(cmd.type, CommandType::Sessions);

  cmd = parse_command("/sessions");
  EXPECT_EQ(cmd.type, CommandType::Sessions);

  cmd = parse_command("/s 3");
  EXPECT_EQ(cmd.type, CommandType::Sessions);
  EXPECT_EQ(cmd.arg, "3");
}

TEST(CommandTest, ParseNormalMessage) {
  auto cmd = parse_command("Hello, how are you?");
  EXPECT_EQ(cmd.type, CommandType::None);

  cmd = parse_command("");
  EXPECT_EQ(cmd.type, CommandType::None);
}

TEST(CommandTest, ParseUnknownCommand) {
  auto cmd = parse_command("/xyz");
  EXPECT_EQ(cmd.type, CommandType::Unknown);
  EXPECT_EQ(cmd.arg, "/xyz");
}

TEST(CommandTest, ParseCommandWithSpaces) {
  auto cmd = parse_command("/s save");
  EXPECT_EQ(cmd.type, CommandType::Sessions);
  EXPECT_EQ(cmd.arg, "save");
}

// ============================================================
// 命令补全匹配测试
// ============================================================

TEST(CommandMatchTest, MatchAll) {
  auto matches = match_commands("/");
  EXPECT_EQ(matches.size(), command_defs().size());
}

TEST(CommandMatchTest, MatchPrefix) {
  auto matches = match_commands("/q");
  ASSERT_GE(matches.size(), 1);
  // 应该匹配到 /quit
  bool found_quit = false;
  for (const auto& m : matches) {
    if (m.name == "/quit") found_quit = true;
  }
  EXPECT_TRUE(found_quit);
}

TEST(CommandMatchTest, MatchExact) {
  auto matches = match_commands("/clear");
  ASSERT_EQ(matches.size(), 1);
  EXPECT_EQ(matches[0].name, "/clear");
}

TEST(CommandMatchTest, MatchByShortcut) {
  auto matches = match_commands("/h");
  ASSERT_GE(matches.size(), 1);
  bool found_help = false;
  for (const auto& m : matches) {
    if (m.name == "/help") found_help = true;
  }
  EXPECT_TRUE(found_help);
}

TEST(CommandMatchTest, NoMatchNonSlash) {
  auto matches = match_commands("hello");
  EXPECT_TRUE(matches.empty());
}

TEST(CommandMatchTest, NoMatchEmpty) {
  auto matches = match_commands("");
  EXPECT_TRUE(matches.empty());
}

TEST(CommandMatchTest, CommandDefsHaveDescriptions) {
  for (const auto& def : command_defs()) {
    EXPECT_FALSE(def.name.empty());
    EXPECT_FALSE(def.description.empty());
    EXPECT_NE(def.type, CommandType::None);
    EXPECT_NE(def.type, CommandType::Unknown);
  }
}

// ============================================================
// 文本工具函数测试
// ============================================================

TEST(TextUtilTest, TruncateText) {
  EXPECT_EQ(truncate_text("short", 10), "short");
  EXPECT_EQ(truncate_text("hello world", 5), "hello...");
  EXPECT_EQ(truncate_text("", 10), "");
  EXPECT_EQ(truncate_text("exactly10!", 10), "exactly10!");
}

TEST(TextUtilTest, SplitLines) {
  auto lines = split_lines("line1\nline2\nline3");
  ASSERT_EQ(lines.size(), 3);
  EXPECT_EQ(lines[0], "line1");
  EXPECT_EQ(lines[1], "line2");
  EXPECT_EQ(lines[2], "line3");
}

TEST(TextUtilTest, SplitLinesEmpty) {
  auto lines = split_lines("");
  ASSERT_EQ(lines.size(), 1);
  EXPECT_EQ(lines[0], "");
}

TEST(TextUtilTest, SplitLinesSingle) {
  auto lines = split_lines("single line");
  ASSERT_EQ(lines.size(), 1);
  EXPECT_EQ(lines[0], "single line");
}

TEST(TextUtilTest, FormatTokens) {
  EXPECT_EQ(format_tokens(0), "0");
  EXPECT_EQ(format_tokens(500), "500");
  EXPECT_EQ(format_tokens(999), "999");
  EXPECT_EQ(format_tokens(1000), "1.0K");
  EXPECT_EQ(format_tokens(1500), "1.5K");
  EXPECT_EQ(format_tokens(10000), "10.0K");
  EXPECT_EQ(format_tokens(100000), "100.0K");
  EXPECT_EQ(format_tokens(1000000), "1.0M");
  EXPECT_EQ(format_tokens(2500000), "2.5M");
}

// ============================================================
// AgentState 测试
// ============================================================

TEST(AgentStateTest, BasicState) {
  AgentState state;
  EXPECT_FALSE(state.is_running());
  EXPECT_EQ(state.input_tokens(), 0);
  EXPECT_EQ(state.output_tokens(), 0);

  state.set_running(true);
  EXPECT_TRUE(state.is_running());

  state.set_running(false);
  EXPECT_FALSE(state.is_running());
}

TEST(AgentStateTest, ModelAndSessionId) {
  AgentState state;
  state.set_model("claude-sonnet-4-20250514");
  state.set_session_id("abc-123");

  EXPECT_EQ(state.model(), "claude-sonnet-4-20250514");
  EXPECT_EQ(state.session_id(), "abc-123");
}

TEST(AgentStateTest, TokenUpdate) {
  AgentState state;
  state.update_tokens(1000, 500);
  EXPECT_EQ(state.input_tokens(), 1000);
  EXPECT_EQ(state.output_tokens(), 500);

  state.update_tokens(2000, 1000);
  EXPECT_EQ(state.input_tokens(), 2000);
  EXPECT_EQ(state.output_tokens(), 1000);
}

TEST(AgentStateTest, StatusText) {
  AgentState state;
  state.set_model("test-model");
  state.update_tokens(1500, 500);

  auto text = state.status_text();
  EXPECT_NE(text.find("test-model"), std::string::npos);
  EXPECT_NE(text.find("1.5K"), std::string::npos);
  EXPECT_NE(text.find("[Ready]"), std::string::npos);

  state.set_running(true);
  text = state.status_text();
  EXPECT_NE(text.find("[Running...]"), std::string::npos);
}

TEST(AgentStateTest, ThreadSafety) {
  AgentState state;
  constexpr int num_threads = 4;
  constexpr int num_ops = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&state, t]() {
      for (int i = 0; i < num_ops; ++i) {
        state.set_running(i % 2 == 0);
        state.update_tokens(i * 100, i * 50);
        state.set_model("model_" + std::to_string(i));
        state.status_text();
        state.is_running();
      }
    });
  }
  for (auto& t : threads) t.join();

  // 无崩溃即通过
  SUCCEED();
}

TEST(AgentStateTest, ModeToggle) {
  AgentState state;
  EXPECT_EQ(state.mode(), AgentMode::Build);

  state.toggle_mode();
  EXPECT_EQ(state.mode(), AgentMode::Plan);

  state.toggle_mode();
  EXPECT_EQ(state.mode(), AgentMode::Build);
}

TEST(AgentStateTest, Activity) {
  AgentState state;
  EXPECT_TRUE(state.activity().empty());

  state.set_activity("Running bash...");
  EXPECT_EQ(state.activity(), "Running bash...");

  state.set_activity("");
  EXPECT_TRUE(state.activity().empty());
}

TEST(AgentModeTest, ToString) {
  EXPECT_EQ(to_string(AgentMode::Build), "build");
  EXPECT_EQ(to_string(AgentMode::Plan), "plan");
}

// ============================================================
// EntryKind to_string 测试
// ============================================================

TEST(EntryKindTest, ToString) {
  EXPECT_EQ(to_string(EntryKind::UserMsg), "UserMsg");
  EXPECT_EQ(to_string(EntryKind::AssistantText), "AssistantText");
  EXPECT_EQ(to_string(EntryKind::ToolCall), "ToolCall");
  EXPECT_EQ(to_string(EntryKind::ToolResult), "ToolResult");
  EXPECT_EQ(to_string(EntryKind::SubtaskStart), "SubtaskStart");
  EXPECT_EQ(to_string(EntryKind::SubtaskEnd), "SubtaskEnd");
  EXPECT_EQ(to_string(EntryKind::Error), "Error");
  EXPECT_EQ(to_string(EntryKind::SystemInfo), "SystemInfo");
}

// ============================================================
// 模拟完整的 Agent 交互流程测试
// ============================================================

// 这组测试模拟一个完整的 TUI agent 交互流程，
// 验证 ChatLog 和 ToolPanel 在典型场景下的行为

TEST(IntegrationTest, SimulateToolCallFlow) {
  // 模拟一个典型的 agent 交互：用户提问 -> agent 调用工具 -> 返回结果 -> 生成回答
  ChatLog log;
  ToolPanel panel;

  // 1. 用户发送消息
  log.push({EntryKind::UserMsg, "列出当前目录的文件", ""});

  // 2. Agent 开始流式回答
  log.append_stream("我来帮你");
  log.append_stream("查看当前目录...");

  // 3. Agent 调用 bash 工具
  log.push({EntryKind::ToolCall, "bash", R"({"command":"ls -la"})"});
  panel.start_tool("bash", R"({"command":"ls -la"})");

  // 4. 工具返回结果
  std::string tool_result = "file1.txt\nfile2.cpp\nREADME.md";
  log.push({EntryKind::ToolResult, "bash [OK]", tool_result});
  panel.finish_tool("bash", tool_result, false);

  // 5. Agent 继续生成回答
  log.append_stream("当前目录包含以下文件：\n- file1.txt\n- file2.cpp\n- README.md");

  // 验证
  auto entries = log.snapshot();
  ASSERT_EQ(entries.size(), 5);
  EXPECT_EQ(entries[0].kind, EntryKind::UserMsg);
  EXPECT_EQ(entries[0].text, "列出当前目录的文件");
  EXPECT_EQ(entries[1].kind, EntryKind::AssistantText);
  EXPECT_EQ(entries[1].text, "我来帮你查看当前目录...");
  EXPECT_EQ(entries[2].kind, EntryKind::ToolCall);
  EXPECT_EQ(entries[3].kind, EntryKind::ToolResult);
  EXPECT_EQ(entries[4].kind, EntryKind::AssistantText);
  EXPECT_NE(entries[4].text.find("file1.txt"), std::string::npos);

  EXPECT_EQ(panel.tool_status("bash"), "done");
}

TEST(IntegrationTest, SimulateSubagentFlow) {
  // 模拟 subagent 场景：主 agent 启动子任务
  ChatLog log;
  ToolPanel panel;

  // 用户让 AI 用 subagent
  log.push({EntryKind::UserMsg, "演示subagent，一个列出目录，一个列出文件", ""});

  // Agent 开始调用 Task 工具（模拟 subagent）
  log.push({EntryKind::ToolCall, "task", R"({"prompt":"列出目录","agent_type":"explore"})"});
  panel.start_tool("task", "列出目录");

  log.push({EntryKind::SubtaskStart, "列出目录", "explore"});

  // 第一个 subagent 完成
  log.push({EntryKind::SubtaskEnd, "src/\ntests/\nexamples/", ""});
  panel.finish_tool("task", "src/ tests/ examples/", false);

  // 第二个 subagent
  log.push({EntryKind::ToolCall, "task", R"({"prompt":"列出文件","agent_type":"explore"})"});
  panel.start_tool("task", "列出文件");
  log.push({EntryKind::SubtaskStart, "列出文件", "explore"});
  log.push({EntryKind::SubtaskEnd, "main.cpp\nutils.hpp", ""});
  panel.finish_tool("task", "main.cpp utils.hpp", false);

  // 主 agent 汇总
  log.append_stream("我已完成两个子任务：\n1. 目录: src/, tests/, examples/\n2. 文件: main.cpp, utils.hpp");

  // 验证整个流程
  auto entries = log.snapshot();
  EXPECT_GE(entries.size(), 8);

  // 验证 ToolPanel
  auto snap = panel.snapshot();
  ASSERT_EQ(snap.size(), 2);
  EXPECT_EQ(snap[0].tool_name, "task");
  EXPECT_EQ(snap[0].status, "done");
  EXPECT_EQ(snap[1].tool_name, "task");
  EXPECT_EQ(snap[1].status, "done");

  // 验证 subagent 相关 entries
  auto subtask_starts = log.filter(EntryKind::SubtaskStart);
  EXPECT_EQ(subtask_starts.size(), 2);

  auto subtask_ends = log.filter(EntryKind::SubtaskEnd);
  EXPECT_EQ(subtask_ends.size(), 2);
}

TEST(IntegrationTest, SimulateMultiToolSequence) {
  // 模拟多工具连续调用场景
  ChatLog log;
  ToolPanel panel;

  log.push({EntryKind::UserMsg, "读取文件内容并修改", ""});

  // 工具 1: read
  panel.start_tool("read", "/path/to/file.cpp");
  log.push({EntryKind::ToolCall, "read", "/path/to/file.cpp"});
  panel.finish_tool("read", "int main() { return 0; }", false);
  log.push({EntryKind::ToolResult, "read [OK]", "int main() { return 0; }"});

  // 工具 2: edit
  panel.start_tool("edit", "修改 main 函数");
  log.push({EntryKind::ToolCall, "edit", "修改 main 函数"});
  panel.finish_tool("edit", "修改成功", false);
  log.push({EntryKind::ToolResult, "edit [OK]", "修改成功"});

  // 工具 3: bash (编译)
  panel.start_tool("bash", "g++ file.cpp");
  log.push({EntryKind::ToolCall, "bash", "g++ file.cpp"});
  panel.finish_tool("bash", "编译成功", false);
  log.push({EntryKind::ToolResult, "bash [OK]", "编译成功"});

  // 最终回答
  log.append_stream("文件已修改并编译成功。");

  // 验证
  auto tool_calls = log.filter(EntryKind::ToolCall);
  ASSERT_EQ(tool_calls.size(), 3);
  EXPECT_EQ(tool_calls[0].text, "read");
  EXPECT_EQ(tool_calls[1].text, "edit");
  EXPECT_EQ(tool_calls[2].text, "bash");

  auto tool_results = log.filter(EntryKind::ToolResult);
  ASSERT_EQ(tool_results.size(), 3);

  EXPECT_EQ(panel.size(), 3);
  auto snap = panel.snapshot();
  for (const auto& a : snap) {
    EXPECT_EQ(a.status, "done");
  }
}

TEST(IntegrationTest, SimulateErrorRecovery) {
  // 模拟错误恢复场景
  ChatLog log;
  ToolPanel panel;

  log.push({EntryKind::UserMsg, "删除一个不存在的文件", ""});

  // 工具报错
  panel.start_tool("bash", "rm nonexistent.txt");
  log.push({EntryKind::ToolCall, "bash", "rm nonexistent.txt"});
  panel.finish_tool("bash", "No such file or directory", true);
  log.push({EntryKind::ToolResult, "bash [FAILED]", "No such file or directory"});

  // Agent 处理错误
  log.append_stream("文件不存在，无法删除。请确认文件路径是否正确。");

  // 验证错误状态
  EXPECT_EQ(panel.tool_status("bash"), "error");

  auto results = log.filter(EntryKind::ToolResult);
  ASSERT_EQ(results.size(), 1);
  EXPECT_NE(results[0].text.find("[FAILED]"), std::string::npos);
}

TEST(IntegrationTest, SimulateStreamInterrupt) {
  // 模拟用户中断场景
  ChatLog log;
  AgentState state;

  state.set_running(true);
  log.push({EntryKind::UserMsg, "写一篇很长的文章", ""});
  log.append_stream("让我来写一篇关于");
  log.append_stream("人工智能的文章...");

  // 用户中断
  state.set_running(false);
  log.push({EntryKind::SystemInfo, "Interrupted by user", ""});

  auto entries = log.snapshot();
  EXPECT_GE(entries.size(), 3);
  EXPECT_EQ(entries.back().kind, EntryKind::SystemInfo);
  EXPECT_EQ(entries.back().text, "Interrupted by user");
  EXPECT_FALSE(state.is_running());
}

TEST(IntegrationTest, SimulateCommandSequence) {
  // 模拟一系列命令操作
  ChatLog log;
  ToolPanel panel;

  // 添加一些内容
  log.push({EntryKind::SystemInfo, "Welcome", ""});
  log.push({EntryKind::UserMsg, "hello", ""});
  log.append_stream("Hi!");
  panel.start_tool("bash", "echo test");
  panel.finish_tool("bash", "test", false);

  EXPECT_EQ(log.size(), 3);
  EXPECT_EQ(panel.size(), 1);

  // 模拟 /clear 命令
  auto cmd = parse_command("/clear");
  EXPECT_EQ(cmd.type, CommandType::Clear);
  log.clear();
  panel.clear();

  EXPECT_EQ(log.size(), 0);
  EXPECT_EQ(panel.size(), 0);

  // 模拟 /help 命令
  cmd = parse_command("/help");
  EXPECT_EQ(cmd.type, CommandType::Help);

  // 模拟 /compact 命令
  cmd = parse_command("/compact");
  EXPECT_EQ(cmd.type, CommandType::Compact);

  // 模拟 /quit 命令
  cmd = parse_command("/quit");
  EXPECT_EQ(cmd.type, CommandType::Quit);
}

// ============================================================
// 端到端测试（需要 API key，条件执行）
// ============================================================

class AgentCliE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_ = agent::Config::load_default();

    // 检查 API key
    const char* openai_key = std::getenv("OPENAI_API_KEY");
    const char* anthropic_key = std::getenv("ANTHROPIC_API_KEY");
    if (!anthropic_key) anthropic_key = std::getenv("ANTHROPIC_AUTH_TOKEN");

    has_api_key_ = false;

    if (anthropic_key) {
      const char* base_url = std::getenv("ANTHROPIC_BASE_URL");
      const char* model = std::getenv("ANTHROPIC_MODEL");
      config_.providers["anthropic"] =
          agent::ProviderConfig{"anthropic", anthropic_key, base_url ? base_url : "https://api.anthropic.com", std::nullopt, {}};
      if (model) config_.default_model = model;
      has_api_key_ = true;
    }

    if (openai_key) {
      const char* base_url = std::getenv("OPENAI_BASE_URL");
      const char* model = std::getenv("OPENAI_MODEL");
      config_.providers["openai"] = agent::ProviderConfig{"openai", openai_key, base_url ? base_url : "https://api.openai.com", std::nullopt, {}};
      if (model) {
        config_.default_model = model;
      } else if (!anthropic_key) {
        config_.default_model = "gpt-4o";
      }
      has_api_key_ = true;
    }

    if (has_api_key_) {
      agent::init();
    }
  }

  agent::Config config_;
  bool has_api_key_ = false;
};

TEST_F(AgentCliE2ETest, SimpleChat) {
  if (!has_api_key_) {
    GTEST_SKIP() << "No API key available, skipping E2E test";
  }

  asio::io_context io_ctx;
  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  auto session = agent::Session::create(io_ctx, config_, agent::AgentType::Build);
  ChatLog log;
  AgentState state;

  // 设置回调
  session->on_stream([&log](const std::string& text) {
    log.append_stream(text);
  });
  session->on_tool_call([&log](const std::string& tool_call_id, const std::string& tool, const agent::json& args) {
    log.push({EntryKind::ToolCall, tool, args.dump()});
  });
  session->on_tool_result([&log](const std::string& tool_call_id, const std::string& tool, const std::string& result, bool is_error) {
    log.push({EntryKind::ToolResult, tool + (is_error ? " [FAILED]" : " [OK]"), result});
  });
  session->on_error([&log](const std::string& error) {
    log.push({EntryKind::Error, error, ""});
  });
  session->set_permission_handler([](const std::string&, const std::string&) {
    std::promise<bool> p;
    p.set_value(true);
    return p.get_future();
  });

  // 发送简单消息
  log.push({EntryKind::UserMsg, "回复两个字：你好", ""});
  state.set_running(true);
  session->prompt("回复两个字：你好");
  state.set_running(false);

  auto usage = session->total_usage();
  state.update_tokens(usage.input_tokens, usage.output_tokens);

  // 验证收到了回复
  auto entries = log.snapshot();
  EXPECT_GE(entries.size(), 2);  // 至少有用户消息和 AI 回复

  // 验证有 assistant 回复
  auto assistant_entries = log.filter(EntryKind::AssistantText);
  EXPECT_GE(assistant_entries.size(), 1);
  EXPECT_FALSE(assistant_entries[0].text.empty());

  // 验证 token 使用
  EXPECT_GT(state.input_tokens(), 0);
  EXPECT_GT(state.output_tokens(), 0);

  // 清理
  session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();
}

TEST_F(AgentCliE2ETest, ToolCallChat) {
  if (!has_api_key_) {
    GTEST_SKIP() << "No API key available, skipping E2E test";
  }

  asio::io_context io_ctx;
  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  auto session = agent::Session::create(io_ctx, config_, agent::AgentType::Build);
  ChatLog log;
  ToolPanel panel;

  session->on_stream([&log](const std::string& text) {
    log.append_stream(text);
  });
  session->on_tool_call([&log, &panel](const std::string& tool_call_id, const std::string& tool, const agent::json& args) {
    std::string args_str = args.dump();
    panel.start_tool(tool, args_str);
    log.push({EntryKind::ToolCall, tool, args_str});
  });
  session->on_tool_result([&log, &panel](const std::string& tool_call_id, const std::string& tool, const std::string& result, bool is_error) {
    std::string summary = result.size() > 200 ? result.substr(0, 200) + "..." : result;
    panel.finish_tool(tool, summary, is_error);
    log.push({EntryKind::ToolResult, tool + (is_error ? " [FAILED]" : " [OK]"), summary});
  });
  session->on_error([&log](const std::string& error) {
    log.push({EntryKind::Error, error, ""});
  });
  session->set_permission_handler([](const std::string&, const std::string&) {
    std::promise<bool> p;
    p.set_value(true);
    return p.get_future();
  });

  // 请求需要工具调用的任务
  log.push({EntryKind::UserMsg, "用 bash 工具执行 echo hello_agent_cli 命令，只需执行这一个命令", ""});
  session->prompt("用 bash 工具执行 echo hello_agent_cli 命令，只需执行这一个命令");

  // 验证工具被调用
  auto tool_calls = log.filter(EntryKind::ToolCall);
  EXPECT_GE(tool_calls.size(), 1);

  // 验证至少有一个工具完成
  auto tool_results = log.filter(EntryKind::ToolResult);
  EXPECT_GE(tool_results.size(), 1);

  // 验证 ToolPanel 中有记录
  EXPECT_GE(panel.size(), 1);

  // 清理
  session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();
}

TEST_F(AgentCliE2ETest, SubagentDemo) {
  if (!has_api_key_) {
    GTEST_SKIP() << "No API key available, skipping E2E test";
  }

  asio::io_context io_ctx;
  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  auto session = agent::Session::create(io_ctx, config_, agent::AgentType::Build);
  ChatLog log;
  ToolPanel panel;

  session->on_stream([&log](const std::string& text) {
    log.append_stream(text);
  });
  session->on_tool_call([&log, &panel](const std::string& tool_call_id, const std::string& tool, const agent::json& args) {
    std::string args_str = args.dump();
    if (args_str.size() > 200) args_str = args_str.substr(0, 200) + "...";
    panel.start_tool(tool, args_str);
    log.push({EntryKind::ToolCall, tool, args_str});
  });
  session->on_tool_result([&log, &panel](const std::string& tool_call_id, const std::string& tool, const std::string& result, bool is_error) {
    std::string summary = result.size() > 300 ? result.substr(0, 300) + "..." : result;
    panel.finish_tool(tool, summary, is_error);
    log.push({EntryKind::ToolResult, tool + (is_error ? " [FAILED]" : " [OK]"), summary});
  });
  session->on_error([&log](const std::string& error) {
    log.push({EntryKind::Error, error, ""});
  });
  session->set_permission_handler([](const std::string&, const std::string&) {
    std::promise<bool> p;
    p.set_value(true);
    return p.get_future();
  });

  // 请求 subagent 演示：一个列出目录，一个列出文件
  std::string prompt =
      "请使用两个 task 子代理完成以下工作：\n"
      "1. 第一个子代理：用 bash 工具执行 ls -d */ 列出当前目录下的子目录\n"
      "2. 第二个子代理：用 bash 工具执行 ls *.cpp *.hpp 2>/dev/null || echo 'no files' 列出当前目录下的 cpp/hpp 文件\n"
      "然后在主会话中汇总两个子代理的结果";

  log.push({EntryKind::UserMsg, prompt, ""});
  session->prompt(prompt);

  // 验证有 task 工具调用
  auto tool_calls = log.filter(EntryKind::ToolCall);
  EXPECT_GE(tool_calls.size(), 1);

  // 检查是否有 task 工具调用
  bool has_task = false;
  for (const auto& tc : tool_calls) {
    if (tc.text == "task") {
      has_task = true;
      break;
    }
  }
  // task 调用可能存在也可能不存在（取决于模型选择）
  // 但至少应该有某种工具调用或回复
  auto all_entries = log.snapshot();
  EXPECT_GE(all_entries.size(), 2);  // 至少有用户消息和某种回复

  // 验证有 assistant 回复（汇总）
  auto assistant_entries = log.filter(EntryKind::AssistantText);
  EXPECT_GE(assistant_entries.size(), 1);

  // 清理
  session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();
}
