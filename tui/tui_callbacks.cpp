#include "tui_callbacks.h"

#include <string>

#include "tui_components.h"
#include "tui_state.h"

namespace agent_cli {

void setup_tui_callbacks(AppState& state, AppContext& ctx) {
  auto& session = ctx.session;
  auto refresh_fn = ctx.refresh_fn;

  session->on_stream([&state, refresh_fn](const std::string& text) {
    state.chat_log.append_stream(text);
    state.agent_state.set_activity("Generating...");
    refresh_fn();
  });

  session->on_tool_call([&state, refresh_fn](const std::string& tool, const agent::json& args) {
    std::string args_str = args.dump(2);
    state.tool_panel.start_tool(tool, args_str);
    state.chat_log.push({EntryKind::ToolCall, tool, args_str});
    state.agent_state.set_activity("Running " + tool + "...");
    refresh_fn();
  });

  session->on_tool_result([&state, refresh_fn](const std::string& tool, const std::string& result, bool is_error) {
    std::string summary = result;
    if (summary.size() > 2000) summary = summary.substr(0, 2000) + "\n...(" + std::to_string(result.size()) + " chars total)";
    state.tool_panel.finish_tool(tool, summary, is_error);
    state.chat_log.push({EntryKind::ToolResult, tool + (is_error ? " ✗" : " ✓"), summary});
    state.agent_state.set_activity("Thinking...");
    refresh_fn();
  });

  session->on_complete([&state, refresh_fn](agent::FinishReason reason) {
    if (reason != agent::FinishReason::Stop && reason != agent::FinishReason::ToolCalls) {
      state.chat_log.push({EntryKind::SystemInfo, "Session ended: " + agent::to_string(reason), ""});
    }
    state.agent_state.set_activity("");
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
        state.chat_log.push({EntryKind::ToolCall, tc->name, tc->arguments.dump(2)});

        // 查找对应的工具结果
        bool found = false;
        for (size_t j = i + 1; j < msgs.size() && !found; ++j) {
          auto results = msgs[j].tool_results();
          for (const auto* tr : results) {
            if (tr->tool_call_id == tc->id) {
              std::string summary = tr->output;
              if (summary.size() > 2000) summary = summary.substr(0, 2000) + "...";
              state.chat_log.push({EntryKind::ToolResult, tc->name + (tr->is_error ? " ✗" : " ✓"), summary});
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
