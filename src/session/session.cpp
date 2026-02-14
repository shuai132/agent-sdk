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
        
        // Check exit condition
        if (last_assistant && last_assistant->is_finished() && 
            last_assistant->finish_reason() != FinishReason::ToolCalls) {
            spdlog::debug("Session {} completed after {} steps", id_, step);
            break;
        }
        
        // Check for context overflow
        if (needs_compaction()) {
            handle_compaction();
            continue;
        }
        
        // Process LLM stream
        process_stream();
        
        // Execute any pending tool calls
        if (last_assistant && last_assistant->finish_reason() == FinishReason::ToolCalls) {
            execute_tool_calls();
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
    
    // Create assistant message to accumulate response
    Message assistant_msg(Role::Assistant, "");
    
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();
    
    provider_->stream(
        request,
        [this, &assistant_msg](const llm::StreamEvent& event) {
            std::visit([this, &assistant_msg](auto&& ev) {
                using T = std::decay_t<decltype(ev)>;
                
                if constexpr (std::is_same_v<T, llm::TextDelta>) {
                    // Accumulate text
                    auto& parts = assistant_msg.parts();
                    if (parts.empty() || !std::holds_alternative<TextPart>(parts.back())) {
                        assistant_msg.add_text(ev.text);
                    } else {
                        std::get<TextPart>(parts.back()).text += ev.text;
                    }
                    
                    if (on_stream_) {
                        on_stream_(ev.text);
                    }
                    
                    Bus::instance().publish(events::StreamDelta{id_, ev.text});
                }
                else if constexpr (std::is_same_v<T, llm::ToolCallDelta>) {
                    // Start of tool call
                    assistant_msg.add_tool_call(ev.id, ev.name, json::object());
                    
                    if (on_tool_call_) {
                        on_tool_call_(ev.name, json::object());
                    }
                    
                    Bus::instance().publish(events::ToolCallStarted{id_, ev.id, ev.name});
                }
                else if constexpr (std::is_same_v<T, llm::ToolCallComplete>) {
                    // Update tool call arguments
                    for (auto& part : assistant_msg.parts()) {
                        if (auto* tc = std::get_if<ToolCallPart>(&part)) {
                            if (tc->id == ev.id || (tc->id.empty() && tc->name == ev.name)) {
                                tc->arguments = ev.arguments;
                                break;
                            }
                        }
                    }
                }
                else if constexpr (std::is_same_v<T, llm::FinishStep>) {
                    assistant_msg.set_finished(true);
                    assistant_msg.set_finish_reason(ev.reason);
                    assistant_msg.set_usage(ev.usage);
                    
                    total_usage_ += ev.usage;
                    
                    Bus::instance().publish(events::TokensUsed{
                        id_, ev.usage.input_tokens, ev.usage.output_tokens
                    });
                }
                else if constexpr (std::is_same_v<T, llm::StreamError>) {
                    if (on_error_) {
                        on_error_(ev.message);
                    }
                }
            }, event);
        },
        [&done_promise]() {
            done_promise.set_value();
        }
    );
    
    done_future.wait();
    
    // Add the completed message
    add_message(std::move(assistant_msg));
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
        
        // Execute tool
        tc->started = true;
        
        try {
            auto future_result = tool->execute(tc->arguments, ctx);
            auto result = future_result.get();
            
            // Truncate if needed
            auto truncated = Truncate::save_and_truncate(result.output, tc->name);
            
            result_msg.add_tool_result(tc->id, tc->name, 
                truncated.content, result.is_error);
            
            Bus::instance().publish(events::ToolCallCompleted{
                id_, tc->id, tc->name, !result.is_error
            });
            
        } catch (const std::exception& e) {
            result_msg.add_tool_result(tc->id, tc->name,
                std::string("Error: ") + e.what(), true);
            
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
