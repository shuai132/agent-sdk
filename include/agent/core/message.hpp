#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/core/types.hpp"
#include "agent/core/uuid.hpp"

namespace agent {

// Message part types
struct TextPart {
    std::string text;
};

struct ToolCallPart {
    std::string id;
    std::string name;
    json arguments;
    
    // Execution state
    bool started = false;
    bool completed = false;
};

struct ToolResultPart {
    std::string tool_call_id;
    std::string tool_name;
    std::string output;
    bool is_error = false;
    
    // Metadata
    std::optional<std::string> title;
    json metadata;
    
    // Context management
    bool compacted = false;  // Content cleared during pruning
    std::optional<Timestamp> compacted_at;
};

struct ImagePart {
    std::string url;  // data: URL or file path
    std::string media_type;
};

struct FilePart {
    std::string path;
    std::string content;
    bool truncated = false;
};

// Compaction marker
struct CompactionPart {
    MessageId parent_id;
    bool completed = false;
};

// Subtask marker
struct SubtaskPart {
    std::string task_id;
    std::string prompt;
    AgentType agent_type;
    bool completed = false;
    std::optional<std::string> result;
};

using MessagePart = std::variant<
    TextPart,
    ToolCallPart,
    ToolResultPart,
    ImagePart,
    FilePart,
    CompactionPart,
    SubtaskPart
>;

// Message role
enum class Role {
    System,
    User,
    Assistant
};

std::string to_string(Role role);
Role role_from_string(const std::string& str);

// Message class
class Message {
public:
    Message() = default;
    Message(Role role, const std::string& content);
    
    // Factory methods
    static Message system(const std::string& content);
    static Message user(const std::string& content);
    static Message assistant(const std::string& content);
    
    // Accessors
    const MessageId& id() const { return id_; }
    Role role() const { return role_; }
    const std::vector<MessagePart>& parts() const { return parts_; }
    std::vector<MessagePart>& parts() { return parts_; }
    
    // Parent message (for threading)
    const std::optional<MessageId>& parent_id() const { return parent_id_; }
    void set_parent_id(const MessageId& id) { parent_id_ = id; }
    
    // Session association
    const SessionId& session_id() const { return session_id_; }
    void set_session_id(const SessionId& id) { session_id_ = id; }
    
    // Completion state (for assistant messages)
    bool is_finished() const { return finished_; }
    void set_finished(bool finished) { finished_ = finished; }
    
    FinishReason finish_reason() const { return finish_reason_; }
    void set_finish_reason(FinishReason reason) { finish_reason_ = reason; }
    
    // Token tracking
    const TokenUsage& usage() const { return usage_; }
    void set_usage(const TokenUsage& usage) { usage_ = usage; }
    
    // Summary flag (for compaction summaries)
    bool is_summary() const { return is_summary_; }
    void set_summary(bool summary) { is_summary_ = summary; }
    
    // Synthetic flag (system-generated messages)
    bool is_synthetic() const { return is_synthetic_; }
    void set_synthetic(bool synthetic) { is_synthetic_ = synthetic; }
    
    // Timestamps
    Timestamp created_at() const { return created_at_; }
    
    // Part manipulation
    void add_part(MessagePart part);
    void add_text(const std::string& text);
    void add_tool_call(const std::string& id, const std::string& name, const json& args);
    void add_tool_result(const std::string& call_id, const std::string& name, 
                        const std::string& output, bool is_error = false);
    
    // Get text content (concatenated)
    std::string text() const;
    
    // Get tool calls from this message
    std::vector<ToolCallPart*> tool_calls();
    std::vector<const ToolCallPart*> tool_calls() const;
    
    // Get tool results from this message
    std::vector<ToolResultPart*> tool_results();
    std::vector<const ToolResultPart*> tool_results() const;
    
    // Serialization
    json to_json() const;
    static Message from_json(const json& j);
    
    // Convert to LLM API format
    json to_api_format() const;

private:
    MessageId id_ = UUID::generate();
    Role role_ = Role::User;
    std::vector<MessagePart> parts_;
    
    std::optional<MessageId> parent_id_;
    SessionId session_id_;
    
    bool finished_ = false;
    FinishReason finish_reason_ = FinishReason::Stop;
    TokenUsage usage_;
    
    bool is_summary_ = false;
    bool is_synthetic_ = false;
    
    Timestamp created_at_ = std::chrono::system_clock::now();
};

// Message storage interface
class MessageStore {
public:
    virtual ~MessageStore() = default;
    
    virtual void save(const Message& msg) = 0;
    virtual std::optional<Message> get(const MessageId& id) = 0;
    virtual std::vector<Message> list(const SessionId& session_id) = 0;
    virtual void update(const Message& msg) = 0;
    virtual void remove(const MessageId& id) = 0;
};

// In-memory message store
class InMemoryMessageStore : public MessageStore {
public:
    void save(const Message& msg) override;
    std::optional<Message> get(const MessageId& id) override;
    std::vector<Message> list(const SessionId& session_id) override;
    void update(const Message& msg) override;
    void remove(const MessageId& id) override;

private:
    mutable std::mutex mutex_;
    std::map<MessageId, Message> messages_;
    std::map<SessionId, std::vector<MessageId>> session_messages_;
};

}  // namespace agent
