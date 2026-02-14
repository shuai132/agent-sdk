#include "agent/session/session.hpp"

#include <spdlog/spdlog.h>
#include <functional>

#include "agent/bus/bus.hpp"
#include "agent/llm/anthropic.hpp"

namespace agent {

std::string to_string(SessionState state) {
    switch (state) {
        case SessionState::Idle: return "idle";
        case SessionState::Running: return "running";
        case SessionState::WaitingForTool: return "waiting_for_tool";
        case SessionState::WaitingForUser: return "waiting_for_user";
        case SessionState::Compacting: return "compacting";
        case SessionState::Completed: return "completed";
        case SessionState::Failed: return "failed";
        case SessionState::Cancelled: return "cancelled";
    }
    return "unknown";
}

Session::Session(asio::io_context& io_ctx, const Config& config, AgentType agent_type)
    : io_ctx_(io_ctx)
    , config_(config)
    , agent_config_(config.get_or_create_agent(agent_type))
    , id_(UUID::generate())
    , abort_signal_(std::make_shared<std::atomic<bool>>(false)) {
    
    // Create provider
    auto provider_name = "anthropic";  // Default to Anthropic
    auto provider_config = config.get_provider(provider_name);
    
    if (provider_config) {
        provider_ = llm::ProviderFactory::instance().create(
            provider_name, *provider_config, io_ctx);
    }
}

Session::~Session() {
    cancel();
}

std::shared_ptr<Session> Session::create(
    asio::io_context& io_ctx,
    const Config& config,
    AgentType agent_type
) {
    auto session = std::shared_ptr<Session>(new Session(io_ctx, config, agent_type));
    
    Bus::instance().publish(events::SessionCreated{session->id()});
    
    return session;
}

std::shared_ptr<Session> Session::create_child(AgentType agent_type) {
    auto child = std::shared_ptr<Session>(new Session(io_ctx_, config_, agent_type));
    child->parent_id_ = id_;
    children_.push_back(child);
    return child;
}

void Session::add_message(Message msg) {
    msg.set_session_id(id_);
    messages_.push_back(std::move(msg));
    
    Bus::instance().publish(events::MessageAdded{id_, messages_.back().id()});
    
    if (on_message_) {
        on_message_(messages_.back());
    }
}

std::vector<Message> Session::get_context_messages() const {
    // Filter out compacted messages
    std::vector<Message> result;
    
    bool found_summary = false;
    
    // Scan from end to find most recent summary
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->is_summary() && it->is_finished()) {
            found_summary = true;
        }
        
        if (found_summary) {
            result.insert(result.begin(), *it);
            if (it->is_summary()) break;  // Stop at the summary
        }
    }
    
    if (!found_summary) {
        // No summary found, return all messages
        return messages_;
    }
    
    return result;
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

void Session::prompt(const std::string& text) {
    prompt(Message::user(text));
}

void Session::prompt(Message user_msg) {
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
    state_ = SessionState::Running;
    
    int step = 0;
    const int max_steps = 100;  // Prevent infinite loops
    
    while (!abort_signal_->load() && step < max_steps) {
        step++;
        
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
        bool needs_response = !context_msgs.empty() && 
                             context_msgs.back().role() == Role::User;
        
        // Check exit condition - stop if assistant has finished without requesting tools
        // AND there's no pending user message that needs a response
        if (!needs_response && last_assistant && last_assistant->is_finished() && 
            last_assistant->finish_reason() != FinishReason::ToolCalls) {
            spdlog::debug("Session {} completed after {} steps", id_, step);
            break;
        }
        
        // Check for context overflow
        if (needs_compaction()) {
            handle_compaction();
            continue;
        }
        
        // If we have pending tool calls, execute them first
        if (last_assistant && last_assistant->finish_reason() == FinishReason::ToolCalls) {
            execute_tool_calls();
        }
        
        // Process LLM - get next response
        process_stream();
        
        // Check if the new response has tool calls
        if (!messages_.empty() && messages_.back().role() == Role::Assistant) {
            auto& new_assistant = messages_.back();
            if (new_assistant.finish_reason() == FinishReason::ToolCalls) {
                execute_tool_calls();
            }
        }
    }
    
    if (abort_signal_->load()) {
        state_ = SessionState::Cancelled;
    } else {
        state_ = SessionState::Completed;
    }
    
    // Prune old outputs
    prune_old_outputs();
    
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
            std::visit([this, &accumulated_text, &usage, &finish_reason, &error_message, &tool_call_builders](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                
                if constexpr (std::is_same_v<T, llm::TextDelta>) {
                    // Stream text to callback immediately
                    if (on_stream_) {
                        on_stream_(e.text);
                    }
                    accumulated_text += e.text;
                } else if constexpr (std::is_same_v<T, llm::ToolCallDelta>) {
                    // Start tracking a new tool call
                    tool_call_builders.push_back({e.id, e.name, e.arguments_delta});
                } else if constexpr (std::is_same_v<T, llm::ToolCallComplete>) {
                    // Tool call arguments completed
                    // Find matching builder by id and update with complete args
                    bool found = false;
                    for (auto& builder : tool_call_builders) {
                        if (!e.id.empty() && builder.id == e.id) {
                            // Update with complete args
                            builder.args_json = e.arguments.dump();
                            
                            if (on_tool_call_) {
                                on_tool_call_(builder.name, e.arguments);
                            }
                            Bus::instance().publish(events::ToolCallStarted{id_, builder.id, builder.name});
                            found = true;
                            break;
                        }
                    }
                    // Handle case where we get ToolCallComplete without a prior ToolCallDelta
                    if (!found && !e.id.empty()) {
                        tool_call_builders.push_back({e.id, e.name, e.arguments.dump()});
                        if (on_tool_call_) {
                            on_tool_call_(e.name, e.arguments);
                        }
                        Bus::instance().publish(events::ToolCallStarted{id_, e.id, e.name});
                    }
                } else if constexpr (std::is_same_v<T, llm::FinishStep>) {
                    finish_reason = e.reason;
                    usage = e.usage;
                } else if constexpr (std::is_same_v<T, llm::StreamError>) {
                    error_message = e.message;
                }
            }, event);
        },
        [&stream_complete]() {
            stream_complete.set_value();
        }
    );
    
    // Wait for stream to complete
    stream_future.wait();
    
    // Check for errors
    if (error_message) {
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
        msg.add_text(accumulated_text);
    }
    
    // Add all tool calls
    for (const auto& builder : tool_call_builders) {
        try {
            json args = json::parse(builder.args_json);
            msg.add_tool_call(builder.id, builder.name, args);
        } catch (...) {
            // Skip invalid tool calls
        }
    }
    
    msg.set_finished(true);
    msg.set_finish_reason(finish_reason);
    msg.set_usage(usage);
    
    total_usage_ += usage;
    
    Bus::instance().publish(events::TokensUsed{
        id_, usage.input_tokens, usage.output_tokens
    });
    
    // Add the completed message
    add_message(std::move(msg));
}

void Session::execute_tool_calls() {
    if (messages_.empty()) return;
    
    auto& last_msg = messages_.back();
    if (last_msg.role() != Role::Assistant) return;
    
    auto tool_calls = last_msg.tool_calls();
    if (tool_calls.empty()) return;
    
    state_ = SessionState::WaitingForTool;
    
    // Create user message for tool results
    Message result_msg(Role::User, "");
    
    for (auto* tc : tool_calls) {
        if (tc->completed) continue;
        
        // Check for doom loop
        if (detect_doom_loop(tc->name, tc->arguments)) {
            // Would need permission check here
            spdlog::warn("Potential doom loop detected for tool: {}", tc->name);
        }
        
        // Get tool
        auto tool = ToolRegistry::instance().get(tc->name);
        if (!tool) {
            result_msg.add_tool_result(tc->id, tc->name, 
                "Tool not found: " + tc->name, true);
            tc->completed = true;
            continue;
        }
        
        // Build context
        ToolContext ctx;
        ctx.session_id = id_;
        ctx.message_id = last_msg.id();
        ctx.working_dir = config_.working_dir.string();
        ctx.abort_signal = abort_signal_;
        ctx.ask_permission = permission_handler_;
        
        // Provide child session creation callback for Task tool
        auto self = shared_from_this();
        ctx.create_child_session = [self](AgentType agent_type) {
            return self->create_child(agent_type);
        };
        
        // Execute tool
        tc->started = true;
        
        try {
            auto future_result = tool->execute(tc->arguments, ctx);
            auto result = future_result.get();
            
            // Truncate if needed
            auto truncated = Truncate::save_and_truncate(result.output, tc->name);
            
            result_msg.add_tool_result(tc->id, tc->name, 
                truncated.content, result.is_error);
            
            // Notify tool result callback
            if (on_tool_result_) {
                on_tool_result_(tc->name, truncated.content, result.is_error);
            }
            
            Bus::instance().publish(events::ToolCallCompleted{
                id_, tc->id, tc->name, !result.is_error
            });
            
        } catch (const std::exception& e) {
            std::string error_msg = std::string("Error: ") + e.what();
            result_msg.add_tool_result(tc->id, tc->name, error_msg, true);
            
            // Notify tool result callback
            if (on_tool_result_) {
                on_tool_result_(tc->name, error_msg, true);
            }
            
            Bus::instance().publish(events::ToolCallCompleted{
                id_, tc->id, tc->name, false
            });
        }
        
        tc->completed = true;
        
        // Track for doom loop detection
        recent_tool_calls_.push_back({tc->name, tc->arguments.dump()});
        if (recent_tool_calls_.size() > 10) {
            recent_tool_calls_.erase(recent_tool_calls_.begin());
        }
    }
    
    // Add tool results
    if (!result_msg.tool_results().empty()) {
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
    
    // In a full implementation, this would:
    // 1. Create a compaction marker message
    // 2. Use a compaction agent to summarize the conversation
    // 3. Mark old messages as compacted
    
    spdlog::info("Session {} triggering compaction", id_);
    
    // For now, just prune
    prune_old_outputs();
    
    state_ = SessionState::Running;
}

void Session::handle_compaction() {
    trigger_compaction();
}

void Session::prune_old_outputs() {
    const int64_t protect_tokens = config_.context.prune_protect_tokens;
    const int64_t minimum_tokens = config_.context.prune_minimum_tokens;
    
    int64_t accumulated = 0;
    int64_t pruned = 0;
    
    // Scan from newest to oldest
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
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
                }
            }
        }
    }
    
    if (pruned >= minimum_tokens) {
        spdlog::info("Session {} pruned {} tokens", id_, pruned);
        
        Bus::instance().publish(events::ContextCompacted{
            id_, accumulated + pruned, accumulated
        });
    }
}

bool Session::detect_doom_loop(const std::string& tool_name, const json& args) {
    if (recent_tool_calls_.size() < 3) return false;
    
    std::string args_hash = args.dump();
    
    int matches = 0;
    for (auto it = recent_tool_calls_.rbegin(); 
         it != recent_tool_calls_.rend() && matches < 3; ++it) {
        if (it->tool_name == tool_name && it->args_hash == args_hash) {
            matches++;
        } else {
            break;
        }
    }
    
    return matches >= 3;
}

}  // namespace agent
