#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <asio.hpp>

#include "agent/core/types.hpp"
#include "agent/core/message.hpp"
#include "agent/core/config.hpp"
#include "agent/llm/provider.hpp"

namespace agent {

// Session state
enum class SessionState {
    Idle,
    Running,
    WaitingForTool,
    WaitingForUser,
    Compacting,
    Completed,
    Failed,
    Cancelled
};

std::string to_string(SessionState state);

// Session class - manages a conversation with an agent
class Session : public std::enable_shared_from_this<Session> {
public:
    // Factory method
    static std::shared_ptr<Session> create(
        asio::io_context& io_ctx,
        const Config& config,
        AgentType agent_type = AgentType::Build
    );
    
    // Create child session (for Task tool)
    std::shared_ptr<Session> create_child(AgentType agent_type);
    
    ~Session();
    
    // Session identification
    const SessionId& id() const { return id_; }
    const std::optional<SessionId>& parent_id() const { return parent_id_; }
    
    // State management
    SessionState state() const { return state_.load(); }
    bool is_running() const { return state_ == SessionState::Running; }
    
    // Message management
    void add_message(Message msg);
    const std::vector<Message>& messages() const { return messages_; }
    std::vector<Message> get_context_messages() const;  // Filtered for LLM
    
    // Token tracking
    TokenUsage total_usage() const { return total_usage_; }
    int64_t estimated_context_tokens() const;
    
    // Agent config
    const AgentConfig& agent_config() const { return agent_config_; }
    
    // Send user message and run agent loop
    void prompt(const std::string& text);
    void prompt(Message user_msg);
    
    // Cancel current operation
    void cancel();
    
    // Event callbacks
    using OnMessageCallback = std::function<void(const Message&)>;
    using OnStreamCallback = std::function<void(const std::string& text)>;
    using OnToolCallCallback = std::function<void(const std::string& tool, const json& args)>;
    using OnToolResultCallback = std::function<void(const std::string& tool, const std::string& result, bool is_error)>;
    using OnCompleteCallback = std::function<void(FinishReason)>;
    using OnErrorCallback = std::function<void(const std::string& error)>;
    
    void on_message(OnMessageCallback cb) { on_message_ = std::move(cb); }
    void on_stream(OnStreamCallback cb) { on_stream_ = std::move(cb); }
    void on_tool_call(OnToolCallCallback cb) { on_tool_call_ = std::move(cb); }
    void on_tool_result(OnToolResultCallback cb) { on_tool_result_ = std::move(cb); }
    void on_complete(OnCompleteCallback cb) { on_complete_ = std::move(cb); }
    void on_error(OnErrorCallback cb) { on_error_ = std::move(cb); }
    
    // Permission handling
    using PermissionHandler = std::function<std::future<bool>(
        const std::string& permission, const std::string& description)>;
    void set_permission_handler(PermissionHandler handler) { 
        permission_handler_ = std::move(handler); 
    }

private:
    Session(asio::io_context& io_ctx, const Config& config, AgentType agent_type);
    
    // The main agent loop
    void run_loop();
    void process_stream();
    void execute_tool_calls();
    void handle_compaction();
    
    // Context management
    bool needs_compaction() const;
    void trigger_compaction();
    void prune_old_outputs();
    
    // Doom loop detection
    bool detect_doom_loop(const std::string& tool_name, const json& args);
    
    asio::io_context& io_ctx_;
    Config config_;
    AgentConfig agent_config_;
    
    SessionId id_;
    std::optional<SessionId> parent_id_;
    
    std::atomic<SessionState> state_{SessionState::Idle};
    std::shared_ptr<std::atomic<bool>> abort_signal_;
    
    std::vector<Message> messages_;
    TokenUsage total_usage_;
    
    std::shared_ptr<llm::Provider> provider_;
    
    // Callbacks
    OnMessageCallback on_message_;
    OnStreamCallback on_stream_;
    OnToolCallCallback on_tool_call_;
    OnToolResultCallback on_tool_result_;
    OnCompleteCallback on_complete_;
    OnErrorCallback on_error_;
    PermissionHandler permission_handler_;
    
    // Doom loop tracking
    struct ToolCallRecord {
        std::string tool_name;
        std::string args_hash;
    };
    std::vector<ToolCallRecord> recent_tool_calls_;
    
    // Child sessions
    std::vector<std::weak_ptr<Session>> children_;
};

}  // namespace agent
