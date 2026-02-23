#include "session.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <functional>
#include <sstream>

#include "bus/bus.hpp"
#include "llm/anthropic.hpp"
#include "tool/permission.hpp"

namespace agent {

std::string to_string(SessionState state) {
  switch (state) {
    case SessionState::Idle:
      return "idle";
    case SessionState::Running:
      return "running";
    case SessionState::WaitingForTool:
      return "waiting_for_tool";
    case SessionState::WaitingForUser:
      return "waiting_for_user";
    case SessionState::Compacting:
      return "compacting";
    case SessionState::Completed:
      return "completed";
    case SessionState::Failed:
      return "failed";
    case SessionState::Cancelled:
      return "cancelled";
  }
  return "unknown";
}

Session::Session(asio::io_context& io_ctx, const Config& config, AgentType agent_type, std::shared_ptr<MessageStore> store)
    : io_ctx_(io_ctx),
      config_(config),
      agent_config_(config.get_or_create_agent(agent_type)),
      id_(UUID::generate()),
      abort_signal_(std::make_shared<std::atomic<bool>>(false)),
      store_(std::move(store)) {
  // Create provider — infer from model name, then try configured providers
  auto model_name = agent_config_.model;

  // Determine preferred provider order based on model name
  std::vector<std::string> provider_order;
  if (model_name.starts_with("gpt-") || model_name.starts_with("o1") || model_name.starts_with("o3") || model_name.starts_with("o4")) {
    provider_order = {"openai", "anthropic"};
  } else if (model_name.starts_with("claude-")) {
    provider_order = {"anthropic", "openai"};
  } else {
    // Unknown model — try all configured providers
    provider_order = {"anthropic", "openai"};
  }

  for (const auto& provider_name : provider_order) {
    auto provider_config = config.get_provider(provider_name);
    if (provider_config) {
      provider_ = llm::ProviderFactory::instance().create(provider_name, *provider_config, io_ctx);
      if (provider_) break;
    }
  }

  // Inject AGENTS.md / CLAUDE.md instructions into system_prompt
  auto instruction_files = config_paths::find_agent_instructions(config.working_dir);
  if (!instruction_files.empty()) {
    std::string injected;
    for (const auto& file : instruction_files) {
      std::ifstream ifs(file);
      if (!ifs.is_open()) continue;

      std::ostringstream ss;
      ss << ifs.rdbuf();
      auto content = ss.str();
      if (content.empty()) continue;

      if (!injected.empty()) injected += "\n\n";
      injected += "Instructions from: " + file.string() + "\n" + content;
      spdlog::debug("Loaded instructions from: {}", file.string());
    }

    if (!injected.empty()) {
      if (!agent_config_.system_prompt.empty()) {
        agent_config_.system_prompt += "\n\n";
      }
      agent_config_.system_prompt += injected;
      spdlog::info("Injected {} instruction file(s) into system prompt", instruction_files.size());
    }
  }
}

Session::~Session() {
  cancel();
}

std::shared_ptr<Session> Session::create(asio::io_context& io_ctx, const Config& config, AgentType agent_type, std::shared_ptr<MessageStore> store) {
  auto session = std::shared_ptr<Session>(new Session(io_ctx, config, agent_type, std::move(store)));

  Bus::instance().publish(events::SessionCreated{session->id()});

  return session;
}

std::shared_ptr<Session> Session::create_child(AgentType agent_type) {
  auto child = std::shared_ptr<Session>(new Session(io_ctx_, config_, agent_type, store_));
  child->parent_id_ = id_;
  children_.push_back(child);

  return child;
}

void Session::add_message(Message msg) {
  msg.set_session_id(id_);
  messages_.push_back(std::move(msg));

  const auto& added = messages_.back();

  // Persist to store
  if (store_) {
    store_->save(added);
  }

  // Auto-generate title from first user message
  if (title_.empty() && added.role() == Role::User) {
    auto text = added.text();
    if (text.size() > 50) {
      text = text.substr(0, 50) + "...";
    }
    title_ = text;
    sync_to_store();
  }

  Bus::instance().publish(events::MessageAdded{id_, added.id()});

  if (on_message_) {
    on_message_(added);
  }
}

std::vector<Message> Session::get_context_messages() const {
  // Find most recent summary, return summary + everything after it
  int summary_index = -1;
  for (int i = static_cast<int>(messages_.size()) - 1; i >= 0; --i) {
    if (messages_[i].is_summary() && messages_[i].is_finished()) {
      summary_index = i;
      break;
    }
  }

  if (summary_index < 0) {
    // No summary found, return all messages
    return messages_;
  }

  // Return summary + all messages after it
  return std::vector<Message>(messages_.begin() + summary_index, messages_.end());
}

int64_t Session::estimated_context_tokens() const {
  // Rough estimation: 4 chars per token
  int64_t total = 0;
  for (const auto& msg : messages_) {
    total += msg.text().size() / 4;
    for (const auto& part : msg.parts()) {
      if (auto* tr = std::get_if<ToolResultPart>(&part)) {
        if (!tr->compacted) {
          total += tr->output.size() / 4;
        }
      }
    }
  }
  return total;
}

int64_t Session::context_window() const {
  auto model_info = provider_ ? provider_->get_model(agent_config_.model) : std::nullopt;
  return model_info ? model_info->context_window : 128000;  // 默认 128k
}

void Session::prompt(const std::string& text) {
  prompt(Message::user(text));
}

void Session::prompt(Message user_msg) {
  spdlog::debug("[Session {}] User input: {}", id_, user_msg.text());
  add_message(std::move(user_msg));
  run_loop();
}

void Session::cancel() {
  abort_signal_->store(true);
  state_ = SessionState::Cancelled;

  if (provider_) {
    provider_->cancel();
  }

  // Cancel child sessions
  for (auto& weak_child : children_) {
    if (auto child = weak_child.lock()) {
      child->cancel();
    }
  }
}

void Session::run_loop() {
  // Reset abort signal for new run
  abort_signal_->store(false);
  state_ = SessionState::Running;

  spdlog::debug("[Session {}] Starting run loop", id_);

  int step = 0;
  const int max_steps = 100;  // Prevent infinite loops

  while (!abort_signal_->load() && step < max_steps && state_ != SessionState::Failed) {
    step++;

    spdlog::debug("[Session {}] Step {} - State: {}", id_, step, to_string(state_));

    auto context_msgs = get_context_messages();

    // Find last assistant message
    const Message* last_assistant = nullptr;
    for (auto it = context_msgs.rbegin(); it != context_msgs.rend(); ++it) {
      if (it->role() == Role::Assistant) {
        last_assistant = &(*it);
        break;
      }
    }

    // Check if the last message is from user (needs response)
    bool needs_response = !context_msgs.empty() && context_msgs.back().role() == Role::User;

    // Check exit condition - stop if assistant has finished without requesting tools
    // AND there's no pending user message that needs a response
    if (!needs_response && last_assistant && last_assistant->is_finished() && last_assistant->finish_reason() != FinishReason::ToolCalls) {
      spdlog::debug("Session {} completed after {} steps", id_, step);
      break;
    }

    // Check for context overflow
    if (needs_compaction()) {
      spdlog::debug("[Session {}] Context needs compaction, triggering...", id_);
      handle_compaction();
      continue;
    }

    // If we have pending tool calls, execute them first
    if (last_assistant && last_assistant->finish_reason() == FinishReason::ToolCalls) {
      spdlog::debug("[Session {}] Executing pending tool calls", id_);
      execute_tool_calls();
    }

    // Process LLM - get next response
    spdlog::debug("[Session {}] Processing LLM stream", id_);
    process_stream();

    // Check if the new response has tool calls
    if (!messages_.empty() && messages_.back().role() == Role::Assistant) {
      auto& new_assistant = messages_.back();
      if (new_assistant.finish_reason() == FinishReason::ToolCalls) {
        spdlog::debug("[Session {}] LLM response contains tool calls, executing...", id_);
        execute_tool_calls();
      }
    }
  }

  if (abort_signal_->load()) {
    state_ = SessionState::Cancelled;
    spdlog::debug("[Session {}] Session cancelled", id_);
  } else if (state_ == SessionState::Failed) {
    // Keep Failed state, already set by process_stream()
    spdlog::debug("[Session {}] Session failed", id_);
  } else {
    state_ = SessionState::Completed;
    spdlog::debug("[Session {}] Session completed", id_);
  }

  // Prune old outputs
  prune_old_outputs();

  // Sync final usage to store
  sync_to_store();

  if (on_complete_) {
    on_complete_(FinishReason::Stop);
  }

  Bus::instance().publish(events::SessionEnded{id_});
}

void Session::process_stream() {
  if (!provider_) {
    if (on_error_) on_error_("No LLM provider configured");
    state_ = SessionState::Failed;
    return;
  }

  // Build request
  llm::LlmRequest request;
  request.model = agent_config_.model;
  request.system_prompt = agent_config_.system_prompt;
  request.messages = get_context_messages();

  // Get available tools
  for (const auto& tool : ToolRegistry::instance().for_agent(agent_config_)) {
    request.tools.push_back(tool);
  }

  spdlog::debug("[Session {}] LLM request: model={}, messages={}, tools={}", id_, request.model, request.messages.size(), request.tools.size());

  // Use streaming API for real-time output
  std::promise<void> stream_complete;
  auto stream_future = stream_complete.get_future();

  // Build message as we receive stream events
  std::string accumulated_text;
  TokenUsage usage;
  FinishReason finish_reason = FinishReason::Stop;
  std::optional<std::string> error_message;

  // Track tool calls being built
  struct ToolCallBuilder {
    std::string id;
    std::string name;
    std::string args_json;
  };
  std::vector<ToolCallBuilder> tool_call_builders;

  provider_->stream(
      request,
      [this, &accumulated_text, &usage, &finish_reason, &error_message, &tool_call_builders](const llm::StreamEvent& event) {
        std::visit(
            [this, &accumulated_text, &usage, &finish_reason, &error_message, &tool_call_builders](auto&& e) {
              using T = std::decay_t<decltype(e)>;

              if constexpr (std::is_same_v<T, llm::TextDelta>) {
                // Stream text to callback immediately
                if (on_stream_) {
                  on_stream_(e.text);
                }
                accumulated_text += e.text;
                spdlog::trace("[Session {}] Text delta: {}", id_, e.text);
              } else if constexpr (std::is_same_v<T, llm::ThinkingDelta>) {
                // Stream thinking/reasoning content to callback
                if (on_thinking_) {
                  on_thinking_(e.text);
                }
                spdlog::trace("[Session {}] Thinking delta: {}", id_, e.text);
              } else if constexpr (std::is_same_v<T, llm::ToolCallDelta>) {
                // Find existing builder by id and accumulate, or create new
                // Only create new builder if id is non-empty (first delta has the real id)
                bool found_builder = false;
                if (!e.id.empty()) {
                  for (auto& builder : tool_call_builders) {
                    if (builder.id == e.id) {
                      builder.args_json += e.arguments_delta;
                      found_builder = true;
                      break;
                    }
                  }
                  if (!found_builder) {
                    spdlog::debug("[Session {}] New tool call builder: id={}, name={}", id_, e.id, e.name);
                    tool_call_builders.push_back({e.id, e.name, e.arguments_delta});
                  }
                }
                // If id is empty, this is a continuation delta - ignore it as OpenAI provider
                // handles accumulation internally and will send ToolCallComplete with full args
              } else if constexpr (std::is_same_v<T, llm::ToolCallComplete>) {
                // Tool call arguments completed
                // Find matching builder by id and update with complete args
                bool found = false;
                for (auto& builder : tool_call_builders) {
                  if (!e.id.empty() && builder.id == e.id) {
                    // Update with complete args
                    builder.args_json = e.arguments.dump();

                    spdlog::debug("[Session {}] Tool call complete: name={}, args={}", id_, builder.name, e.arguments.dump());

                    if (on_tool_call_) {
                      on_tool_call_(builder.id, builder.name, e.arguments);
                    }
                    Bus::instance().publish(events::ToolCallStarted{id_, builder.id, builder.name});
                    found = true;
                    break;
                  }
                }
                // Handle case where we get ToolCallComplete without a prior ToolCallDelta
                if (!found && !e.id.empty()) {
                  tool_call_builders.push_back({e.id, e.name, e.arguments.dump()});
                  spdlog::debug("[Session {}] Tool call complete (no prior delta): name={}, args={}", id_, e.name, e.arguments.dump());
                  if (on_tool_call_) {
                    on_tool_call_(e.id, e.name, e.arguments);
                  }
                  Bus::instance().publish(events::ToolCallStarted{id_, e.id, e.name});
                }
              } else if constexpr (std::is_same_v<T, llm::FinishStep>) {
                finish_reason = e.reason;
                usage = e.usage;
                spdlog::debug("[Session {}] LLM finish step: reason={}, input_tokens={}, output_tokens={}", id_, to_string(finish_reason),
                              usage.input_tokens, usage.output_tokens);
              } else if constexpr (std::is_same_v<T, llm::StreamError>) {
                error_message = e.message;
              }
            },
            event);
      },
      [&stream_complete]() {
        stream_complete.set_value();
        spdlog::debug("[Session {}] LLM stream completed", static_cast<void*>(&stream_complete));
      });

  // Wait for stream to complete
  stream_future.wait();

  // Check for errors
  if (error_message) {
    spdlog::error("[Session {}] LLM stream error: {}", id_, *error_message);
    if (on_error_) {
      on_error_(*error_message);
    }
    state_ = SessionState::Failed;
    return;
  }

  // Finalize message - build from accumulated data
  Message msg(Role::Assistant, "");

  // Add accumulated text
  if (!accumulated_text.empty()) {
    spdlog::debug("[Session {}] LLM response text: {} chars", id_, accumulated_text.size());
    msg.add_text(accumulated_text);
  }

  // Add all tool calls
  for (const auto& builder : tool_call_builders) {
    try {
      json args = json::parse(builder.args_json);
      if (!args.is_object()) {
        spdlog::warn("Tool call args is not an object for {}, skipping", builder.name);
        continue;
      }
      spdlog::debug("[Session {}] Adding tool call: name={}, args={}", id_, builder.name, args.dump());
      msg.add_tool_call(builder.id, builder.name, args);
    } catch (...) {
      // Skip invalid tool calls
      spdlog::warn("[Session {}] Failed to parse tool call args for {}", id_, builder.name);
    }
  }

  msg.set_finished(true);
  msg.set_finish_reason(finish_reason);
  msg.set_usage(usage);

  total_usage_ += usage;

  Bus::instance().publish(events::TokensUsed{id_, usage.input_tokens, usage.output_tokens});

  // Add the completed message
  add_message(std::move(msg));
}

void Session::execute_tool_calls() {
  if (messages_.empty()) return;

  auto& last_msg = messages_.back();
  if (last_msg.role() != Role::Assistant) return;

  auto tool_calls = last_msg.tool_calls();
  if (tool_calls.empty()) return;

  spdlog::debug("[Session {}] Executing {} tool call(s)", id_, tool_calls.size());

  state_ = SessionState::WaitingForTool;

  // Create user message for tool results
  Message result_msg(Role::User, "");

  for (auto* tc : tool_calls) {
    if (tc->completed) continue;

    spdlog::debug("[Session {}] Executing tool call: name={}, args={}", id_, tc->name, tc->arguments.dump());

    // Check for doom loop
    if (detect_doom_loop(tc->name, tc->arguments)) {
      // Would need permission check here
      spdlog::warn("[Session {}] Potential doom loop detected for tool: {}", id_, tc->name);
    }

    // Get tool
    auto tool = ToolRegistry::instance().get(tc->name);
    if (!tool) {
      spdlog::error("[Session {}] Tool not found: {}", id_, tc->name);
      result_msg.add_tool_result(tc->id, tc->name, "Tool not found: " + tc->name, true);
      tc->completed = true;
      continue;
    }

    // Check permission
    auto perm = PermissionManager::instance().check_permission(tc->name, agent_config_);
    if (perm == Permission::Deny) {
      spdlog::info("[Session {}] Permission denied for tool: {}", id_, tc->name);
      result_msg.add_tool_result(tc->id, tc->name, "Permission denied: tool '" + tc->name + "' is not allowed", true);
      tc->completed = true;
      continue;
    }
    if (perm == Permission::Ask) {
      bool allowed = true;  // Default allow for non-interactive mode
      if (permission_handler_) {
        try {
          auto future = permission_handler_(tc->name, "Tool '" + tc->name + "' requires permission to execute");
          allowed = future.get();
        } catch (const std::exception& e) {
          spdlog::warn("Permission handler error for tool {}: {}", tc->name, e.what());
          allowed = false;
        }
      }
      if (!allowed) {
        spdlog::info("[Session {}] User denied permission for tool: {}", id_, tc->name);
        PermissionManager::instance().deny(tc->name);
        result_msg.add_tool_result(tc->id, tc->name, "Permission denied: tool '" + tc->name + "' is not allowed", true);
        tc->completed = true;
        continue;
      }
      PermissionManager::instance().grant(tc->name);
    }

    // Build context
    ToolContext ctx;
    ctx.session_id = id_;
    ctx.message_id = last_msg.id();
    ctx.working_dir = config_.working_dir.string();
    ctx.abort_signal = abort_signal_;
    ctx.ask_permission = permission_handler_;
    ctx.question_handler = question_handler_;

    // Provide child session creation callback for Task tool
    auto self = shared_from_this();
    ctx.create_child_session = [self](AgentType agent_type) {
      return self->create_child(agent_type);
    };

    // Provide subagent event callback for Task tool progress reporting
    std::string tool_call_id = tc->id;
    ctx.on_subagent_event = [self, tool_call_id](const SubagentEvent& event) {
      if (self->subagent_event_handler_) {
        self->subagent_event_handler_(tool_call_id, event);
      }
    };

    // Execute tool
    tc->started = true;

    try {
      spdlog::debug("[Session {}] Calling tool: {} with args: {}", id_, tc->name, tc->arguments.dump());
      auto future_result = tool->execute(tc->arguments, ctx);
      auto result = future_result.get();

      spdlog::debug("[Session {}] Tool {} completed, is_error={}, output length={}", id_, tc->name, result.is_error, result.output.size());

      // Truncate if needed
      auto truncated = Truncate::save_and_truncate(result.output, tc->name);

      // Sanitize invalid UTF-8 bytes to prevent JSON serialization errors
      auto safe_content = sanitize_utf8(truncated.content);

      result_msg.add_tool_result(tc->id, tc->name, safe_content, result.is_error);

      // Notify tool result callback
      if (on_tool_result_) {
        on_tool_result_(tc->id, tc->name, safe_content, result.is_error);
      }

      Bus::instance().publish(events::ToolCallCompleted{id_, tc->id, tc->name, !result.is_error});

    } catch (const std::exception& e) {
      std::string error_msg = std::string("Error: ") + e.what();
      spdlog::error("[Session {}] Tool {} exception: {}", id_, tc->name, e.what());
      result_msg.add_tool_result(tc->id, tc->name, error_msg, true);

      // Notify tool result callback
      if (on_tool_result_) {
        on_tool_result_(tc->id, tc->name, error_msg, true);
      }

      Bus::instance().publish(events::ToolCallCompleted{id_, tc->id, tc->name, false});
    }

    tc->completed = true;

    spdlog::debug("[Session {}] Tool call completed: {}", id_, tc->name);

    // Track for doom loop detection
    recent_tool_calls_.push_back({tc->name, tc->arguments.dump()});
    if (recent_tool_calls_.size() > 10) {
      recent_tool_calls_.erase(recent_tool_calls_.begin());
    }
  }

  // Add tool results
  if (!result_msg.tool_results().empty()) {
    spdlog::debug("[Session {}] Adding {} tool result(s) to messages", id_, result_msg.tool_results().size());
    add_message(std::move(result_msg));
  }

  state_ = SessionState::Running;
}

bool Session::needs_compaction() const {
  auto model_info = provider_ ? provider_->get_model(agent_config_.model) : std::nullopt;
  int64_t limit = model_info ? model_info->context_window : 100000;

  return estimated_context_tokens() > limit * 0.8;  // 80% threshold
}

void Session::trigger_compaction() {
  state_ = SessionState::Compacting;
  spdlog::info("Session {} triggering compaction", id_);

  if (!provider_) {
    // No provider available, fall back to pruning only
    prune_old_outputs();
    state_ = SessionState::Running;
    return;
  }

  // 1. Collect messages to summarize
  auto messages_to_summarize = collect_messages_for_compaction();

  if (messages_to_summarize.empty()) {
    prune_old_outputs();
    state_ = SessionState::Running;
    return;
  }

  // 2. Build compaction request
  llm::LlmRequest request;
  request.model = agent_config_.model;
  request.system_prompt =
      "You are a conversation summarizer. Summarize the following conversation into a concise summary "
      "that preserves all important context, decisions made, code changes, file paths, and any ongoing tasks. "
      "The summary will be used to continue the conversation, so include all information needed to pick up "
      "where the conversation left off.\n\n"
      "Format your summary as a structured overview:\n"
      "- **Topic/Goal**: What the user is working on\n"
      "- **Progress**: What has been done so far\n"
      "- **Key Decisions**: Important choices made\n"
      "- **Current State**: Where things stand now\n"
      "- **Pending Items**: What still needs to be done (if any)";
  request.messages = std::move(messages_to_summarize);
  // No tools for compaction agent

  // 3. Call LLM to generate summary
  std::string summary_text = stream_compaction(request);

  if (summary_text.empty()) {
    spdlog::warn("Session {} compaction failed, falling back to prune", id_);
    prune_old_outputs();
    state_ = SessionState::Running;
    return;
  }

  // 4. Create summary message
  Message summary_msg(Role::Assistant, "");
  summary_msg.add_text(summary_text);
  summary_msg.set_summary(true);
  summary_msg.set_finished(true);
  summary_msg.set_synthetic(true);

  // 5. Add summary message (auto-persists via store)
  add_message(std::move(summary_msg));

  // 6. Prune old tool outputs
  prune_old_outputs();

  state_ = SessionState::Running;
  spdlog::info("Session {} compaction completed", id_);
}

std::vector<Message> Session::collect_messages_for_compaction() const {
  // Find the most recent summary
  int summary_index = -1;
  for (int i = static_cast<int>(messages_.size()) - 1; i >= 0; --i) {
    if (messages_[i].is_summary() && messages_[i].is_finished()) {
      summary_index = i;
      break;
    }
  }

  // Collect all messages (or all since last summary) to be summarized
  // We pass the full context to the LLM so it can generate a comprehensive summary
  std::vector<Message> result;

  if (summary_index >= 0) {
    // Include old summary + everything after it (to create a new combined summary)
    for (int i = summary_index; i < static_cast<int>(messages_.size()); ++i) {
      result.push_back(messages_[i]);
    }
  } else {
    // No summary exists, include all messages
    result = messages_;
  }

  // Convert to a single user message containing the conversation for the summarizer
  if (result.empty()) return {};

  std::string conversation_text;
  for (const auto& msg : result) {
    if (msg.role() == Role::System) continue;

    if (msg.is_summary()) {
      conversation_text += "[Previous Summary]\n" + msg.text() + "\n\n";
      continue;
    }

    std::string role_str = (msg.role() == Role::User) ? "User" : "Assistant";
    auto text = msg.text();

    if (!text.empty()) {
      conversation_text += role_str + ": " + text + "\n\n";
    }

    // Include tool calls briefly
    for (const auto* tc : msg.tool_calls()) {
      conversation_text += "[Tool call: " + tc->name + "(" + tc->arguments.dump() + ")]\n";
    }

    // Include tool results briefly (skip compacted ones)
    for (const auto* tr : msg.tool_results()) {
      if (tr->compacted) {
        conversation_text += "[Tool result: " + tr->tool_name + " (content cleared)]\n";
      } else {
        auto output = tr->output;
        if (output.size() > 500) {
          output = output.substr(0, 500) + "... (truncated)";
        }
        conversation_text += "[Tool result: " + tr->tool_name + "]\n" + output + "\n\n";
      }
    }
  }

  // Return as a single user message asking for a summary
  std::vector<Message> compaction_messages;
  compaction_messages.push_back(Message::user("Please summarize the following conversation:\n\n" + conversation_text));
  return compaction_messages;
}

std::string Session::stream_compaction(const llm::LlmRequest& request) {
  std::promise<void> done;
  auto done_future = done.get_future();

  std::string accumulated_text;
  std::optional<std::string> error_message;

  provider_->stream(
      request,
      [&accumulated_text, &error_message](const llm::StreamEvent& event) {
        std::visit(
            [&accumulated_text, &error_message](auto&& e) {
              using T = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<T, llm::TextDelta>) {
                accumulated_text += e.text;
              } else if constexpr (std::is_same_v<T, llm::StreamError>) {
                error_message = e.message;
              }
              // Ignore tool calls and other events for compaction
            },
            event);
      },
      [&done]() {
        done.set_value();
      });

  done_future.wait();

  if (error_message) {
    spdlog::warn("Compaction stream error: {}", *error_message);
    return "";
  }

  return accumulated_text;
}

void Session::handle_compaction() {
  trigger_compaction();
}

void Session::prune_old_outputs() {
  const int64_t protect_tokens = config_.context.prune_protect_tokens;
  const int64_t minimum_tokens = config_.context.prune_minimum_tokens;

  int64_t accumulated = 0;
  int64_t pruned = 0;

  // Track messages that were modified for store sync
  std::vector<const Message*> modified_messages;

  // Scan from newest to oldest
  for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
    bool modified = false;
    for (auto& part : it->parts()) {
      if (auto* tr = std::get_if<ToolResultPart>(&part)) {
        int64_t part_tokens = tr->output.size() / 4;

        if (accumulated < protect_tokens) {
          accumulated += part_tokens;
        } else if (!tr->compacted) {
          // Check if tool is protected (e.g., skill)
          if (tr->tool_name == "skill") continue;

          // Compact this output
          tr->compacted = true;
          tr->compacted_at = std::chrono::system_clock::now();
          tr->output = "[Old tool result content cleared]";
          pruned += part_tokens;
          modified = true;
        }
      }
    }
    if (modified) {
      modified_messages.push_back(&(*it));
    }
  }

  // Sync modified messages to store
  if (store_ && !modified_messages.empty()) {
    for (const auto* msg : modified_messages) {
      store_->update(*msg);
    }
  }

  if (pruned >= minimum_tokens) {
    spdlog::info("Session {} pruned {} tokens", id_, pruned);

    Bus::instance().publish(events::ContextCompacted{id_, accumulated + pruned, accumulated});
  }
}

bool Session::detect_doom_loop(const std::string& tool_name, const json& args) {
  if (recent_tool_calls_.size() < 3) return false;

  std::string args_hash = args.dump();

  int matches = 0;
  for (auto it = recent_tool_calls_.rbegin(); it != recent_tool_calls_.rend() && matches < 3; ++it) {
    if (it->tool_name == tool_name && it->args_hash == args_hash) {
      matches++;
    } else {
      break;
    }
  }

  return matches >= 3;
}

std::shared_ptr<Session> Session::resume(asio::io_context& io_ctx, const Config& config, const SessionId& session_id,
                                         std::shared_ptr<JsonMessageStore> store) {
  if (!store) {
    spdlog::warn("Cannot resume session without a store");
    return nullptr;
  }

  // Load session metadata
  auto meta = store->get_session(session_id);
  if (!meta) {
    spdlog::warn("Session {} not found in store", session_id);
    return nullptr;
  }

  auto session = std::shared_ptr<Session>(new Session(io_ctx, config, meta->agent_type, store));

  // Override the generated id with the stored one
  session->id_ = session_id;
  session->title_ = meta->title;
  session->parent_id_ = meta->parent_id;
  session->total_usage_ = meta->total_usage;

  // Load messages from store
  session->messages_ = store->list(session_id);

  spdlog::info("Resumed session {} with {} messages", session_id, session->messages_.size());

  Bus::instance().publish(events::SessionCreated{session->id()});

  return session;
}

void Session::sync_to_store() {
  auto* json_store = dynamic_cast<JsonMessageStore*>(store_.get());
  if (!json_store) {
    return;
  }

  // Don't persist empty sessions (no messages yet)
  if (messages_.empty()) {
    return;
  }

  SessionMeta meta;
  meta.id = id_;
  meta.title = title_;
  meta.parent_id = parent_id_;
  meta.agent_type = agent_config_.type;
  meta.updated_at = std::chrono::system_clock::now();
  meta.total_usage = total_usage_;

  // Preserve original created_at if session already exists
  auto existing = json_store->get_session(id_);
  if (existing) {
    meta.created_at = existing->created_at;
  }

  json_store->save_session(meta);
}

}  // namespace agent
