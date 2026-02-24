#include "tui_callbacks.h"

#include <future>
#include <string>

#include "tui_components.h"
#include "tui_state.h"

namespace agent_cli {

void setup_tui_callbacks(AppState& state, AppContext& ctx) {
  auto& session = ctx.session;
  auto refresh_fn = ctx.refresh_fn;

  // Accumulated thinking text
  static std::string thinking_buffer;

  session->on_stream([&state, refresh_fn](const std::string& text) {
    // When actual content starts streaming, finalize thinking
    if (!thinking_buffer.empty()) {
      // Push thinking as a special entry in chat log
      state.chat_log.push({EntryKind::Thinking, thinking_buffer, ""});
      thinking_buffer.clear();
    }
    state.chat_log.append_stream(text);
    state.agent_state.set_activity("Generating...");
    refresh_fn();
  });

  session->on_thinking([&state, refresh_fn](const std::string& thinking) {
    // Append to thinking buffer
    thinking_buffer += thinking;

    // Show thinking content in activity area without truncation
    std::string display = thinking_buffer;
    // Replace newlines with spaces for display
    for (auto& c : display) {
      if (c == '\n' || c == '\r') c = ' ';
    }
    // Don't truncate - show full thinking content
    state.agent_state.set_activity("💭 " + display);
    refresh_fn();
  });

  session->on_tool_call([&state, refresh_fn](const std::string& tool_call_id, const std::string& tool, const agent::json& args) {
    // Finalize thinking if there was thinking before tool call
    if (!thinking_buffer.empty()) {
      state.chat_log.push({EntryKind::Thinking, thinking_buffer, ""});
      thinking_buffer.clear();
    }
    std::string args_str = args.dump(2);
    state.tool_panel.start_tool(tool, args_str);
    state.chat_log.push({EntryKind::ToolCall, tool, args_str, tool_call_id});
    // 记录工具开始执行时间
    state.chat_log.update_tool_started(tool_call_id);
    state.agent_state.set_activity("Running " + tool + "...");
    refresh_fn();
  });

  session->on_tool_result([&state, refresh_fn](const std::string& tool_call_id, const std::string& tool, const std::string& result, bool is_error) {
    std::string summary = result;
    if (summary.size() > 2000) summary = summary.substr(0, 2000) + "\n...(" + std::to_string(result.size()) + " chars total)";
    state.tool_panel.finish_tool(tool, summary, is_error);
    // 记录工具完成时间
    state.chat_log.update_tool_completed(tool_call_id);
    state.chat_log.push({EntryKind::ToolResult, tool + (is_error ? " ✗" : " ✓"), summary, tool_call_id});
    state.agent_state.set_activity("Thinking...");
    refresh_fn();
  });

  // Subagent event handler for Task tool progress
  session->set_subagent_event_handler([&state, refresh_fn](const std::string& tool_call_id, const agent::SubagentEvent& event) {
    // Convert subagent event to nested chat entry
    ChatEntry nested_entry;
    switch (event.type) {
      case agent::SubagentEvent::Type::Stream:
        // Accumulate stream text - update the last nested entry if it's AssistantText
        nested_entry = {EntryKind::AssistantText, event.text, ""};
        break;
      case agent::SubagentEvent::Type::Thinking:
        // Use cumulative approach for thinking - don't create separate entries
        state.chat_log.append_nested_thinking(tool_call_id, event.text);
        state.chat_log.update_tool_activity(tool_call_id, "💭 Thinking...");
        refresh_fn();
        return;  // Early return, don't add as separate nested entry
      case agent::SubagentEvent::Type::ToolCall:
        nested_entry = {EntryKind::ToolCall, event.text, event.detail};
        state.chat_log.update_tool_activity(tool_call_id, "🔧 " + event.text + "...");
        break;
      case agent::SubagentEvent::Type::ToolResult:
        nested_entry = {EntryKind::ToolResult, event.text + (event.is_error ? " ✗" : " ✓"), event.detail};
        break;
      case agent::SubagentEvent::Type::Complete:
        state.chat_log.update_tool_activity(tool_call_id, "");  // Clear activity
        return;                                                 // Don't add nested entry for complete
      case agent::SubagentEvent::Type::Error:
        nested_entry = {EntryKind::Error, event.text, ""};
        break;
    }
    state.chat_log.add_nested_entry(tool_call_id, std::move(nested_entry));
    refresh_fn();
  });

  session->on_complete([&state, refresh_fn](agent::FinishReason reason) {
    if (reason != agent::FinishReason::Stop && reason != agent::FinishReason::ToolCalls) {
      state.chat_log.push({EntryKind::SystemInfo, "Session ended: " + agent::to_string(reason), ""});
    }
    state.agent_state.set_activity("");
    state.agent_state.pause_session_timer();  // 暂停计时器
    refresh_fn();
  });

  session->on_error([&state, refresh_fn](const std::string& error) {
    state.chat_log.push({EntryKind::Error, error, ""});
    state.agent_state.set_activity("");
    refresh_fn();
  });

  session->set_permission_handler([&state, refresh_fn](const std::string& permission, const std::string& description) {
    state.chat_log.push({EntryKind::SystemInfo, "Auto-allowed: " + permission, description});
    refresh_fn();
    std::promise<bool> p;
    p.set_value(true);
    return p.get_future();
  });

  // Question handler for interactive question tool
  session->set_question_handler([&state, refresh_fn](const agent::QuestionInfo& info) {
    // Set up the question panel state
    state.question_list = info.questions;
    state.question_answers.clear();
    state.question_answers.resize(info.questions.size());
    state.question_current_index = 0;
    state.question_input_text.clear();

    // Create a promise/future pair for async response
    state.question_promise = std::make_shared<std::promise<agent::QuestionResponse>>();
    auto future = state.question_promise->get_future();

    // Show the panel
    state.show_question_panel = true;
    state.agent_state.set_activity("Waiting for your answer...");
    refresh_fn();

    // The future will be fulfilled when the user submits answers in the TUI event handler
    return future;
  });
}

void load_history_to_chat_log(AppState& state, const std::shared_ptr<agent::Session>& session) {
  const auto& msgs = session->messages();
  if (msgs.empty()) return;

  // 查找最近的摘要消息
  int start_index = 0;
  for (int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i) {
    if (msgs[i].is_summary() && msgs[i].is_finished()) {
      start_index = i;
      break;
    }
  }

  if (start_index > 0) {
    state.chat_log.push({EntryKind::SystemInfo, "[" + std::to_string(start_index) + " earlier messages compacted]", ""});
  }

  for (size_t i = static_cast<size_t>(start_index); i < msgs.size(); ++i) {
    const auto& msg = msgs[i];

    // 跳过系统消息
    if (msg.role() == agent::Role::System) continue;

    // 显示摘要消息
    if (msg.is_summary()) {
      state.chat_log.push({EntryKind::SystemInfo, "[Summary] " + msg.text(), ""});
      continue;
    }

    // 处理用户消息
    if (msg.role() == agent::Role::User) {
      auto tool_results = msg.tool_results();
      // 跳过纯工具结果消息（没有文本内容）
      if (!tool_results.empty() && msg.text().empty()) continue;
      if (!msg.text().empty()) {
        state.chat_log.push({EntryKind::UserMsg, msg.text(), ""});
      }
      continue;  // 重要：处理完用户消息后继续下一条
    }

    // 处理助手消息
    if (msg.role() == agent::Role::Assistant) {
      // 添加文本内容
      if (!msg.text().empty()) {
        state.chat_log.push({EntryKind::AssistantText, msg.text(), ""});
      }

      // 添加工具调用和结果
      auto tool_calls = msg.tool_calls();
      for (const auto* tc : tool_calls) {
        state.chat_log.push({EntryKind::ToolCall, tc->name, tc->arguments.dump(2), tc->id});

        // 查找对应的工具结果
        bool found = false;
        for (size_t j = i + 1; j < msgs.size() && !found; ++j) {
          auto results = msgs[j].tool_results();
          for (const auto* tr : results) {
            if (tr->tool_call_id == tc->id) {
              std::string summary = tr->output;
              if (summary.size() > 2000) summary = summary.substr(0, 2000) + "...";
              state.chat_log.push({EntryKind::ToolResult, tc->name + (tr->is_error ? " ✗" : " ✓"), summary, tc->id});
              found = true;
              break;
            }
          }
        }
      }
      continue;  // 重要：处理完助手消息后继续下一条
    }
  }
}

}  // namespace agent_cli
