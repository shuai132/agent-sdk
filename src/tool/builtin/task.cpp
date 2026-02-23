#include "builtins.hpp"
#include "session/session.hpp"

namespace agent::tools {

// ============================================================================
// TaskTool
// ============================================================================

TaskTool::TaskTool() : SimpleTool("task", "Launch a new agent to handle complex, multistep tasks autonomously.") {}

std::vector<ParameterSchema> TaskTool::parameters() const {
  return {{"prompt", "string", "The task for the agent to perform", true, std::nullopt, std::nullopt},
          {"description", "string", "A short description of the task", true, std::nullopt, std::nullopt},
          {"subagent_type", "string", "The type of agent to use", true, std::nullopt, std::vector<std::string>{"general", "explore"}},
          {"task_id", "string", "Resume a previous task session", false, std::nullopt, std::nullopt}};
}

std::future<ToolResult> TaskTool::execute(const json& args, const ToolContext& ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string prompt = args.value("prompt", "");
    std::string description = args.value("description", "");
    std::string agent_type_str = args.value("subagent_type", "general");

    // Check if we have the child session creation callback
    if (!ctx.create_child_session) {
      return ToolResult::error("Task tool requires a session context to create child sessions");
    }

    // Map agent type string to enum
    AgentType agent_type = AgentType::General;  // default
    if (agent_type_str == "explore") {
      agent_type = AgentType::Explore;
    } else if (agent_type_str == "general") {
      agent_type = AgentType::General;
    }

    // Create child session
    auto child_session = ctx.create_child_session(agent_type);
    if (!child_session) {
      return ToolResult::error("Failed to create child session");
    }

    // Collect the response
    std::string response_text;
    std::promise<void> completion_promise;
    auto completion_future = completion_promise.get_future();

    // Helper to emit subagent events
    auto emit_event = [&ctx](SubagentEvent::Type type, const std::string& text, const std::string& detail = "", bool is_error = false) {
      if (ctx.on_subagent_event) {
        ctx.on_subagent_event({type, text, detail, is_error});
      }
    };

    // Set up callbacks to capture the response and emit progress events
    child_session->on_stream([&response_text, &emit_event](const std::string& text) {
      response_text += text;
      emit_event(SubagentEvent::Type::Stream, text);
    });

    child_session->on_thinking([&emit_event](const std::string& thinking) {
      emit_event(SubagentEvent::Type::Thinking, thinking);
    });

    child_session->on_tool_call([&emit_event](const std::string& tool, const json& args) {
      emit_event(SubagentEvent::Type::ToolCall, tool, args.dump(2));
    });

    child_session->on_tool_result([&emit_event](const std::string& tool, const std::string& result, bool is_error) {
      emit_event(SubagentEvent::Type::ToolResult, tool, result, is_error);
    });

    child_session->on_complete([&completion_promise, &emit_event](FinishReason reason) {
      emit_event(SubagentEvent::Type::Complete, to_string(reason));
      completion_promise.set_value();
    });

    child_session->on_error([&response_text, &completion_promise, &emit_event](const std::string& error) {
      response_text = "Error: " + error;
      emit_event(SubagentEvent::Type::Error, error);
      completion_promise.set_value();
    });

    // Send the prompt to the child session
    child_session->prompt(prompt);

    // Wait for completion
    completion_future.wait();

    // Return the result
    return ToolResult::with_title(response_text.empty() ? "Task completed with no output" : response_text, "Task: " + description);
  });
}

}  // namespace agent::tools
