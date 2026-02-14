#pragma once

#include <filesystem>
#include <mutex>
#include <vector>

#include "core/message.hpp"
#include "core/types.hpp"

namespace agent {

// Session metadata for index
struct SessionMeta {
  SessionId id;
  std::string title;
  std::optional<SessionId> parent_id;
  AgentType agent_type = AgentType::Build;
  Timestamp created_at = std::chrono::system_clock::now();
  Timestamp updated_at = std::chrono::system_clock::now();
  TokenUsage total_usage;

  json to_json() const;
  static SessionMeta from_json(const json &j);
};

// JSON file-based message store
// Storage layout:
//   base_dir/
//     sessions.json                  — session index
//     {session_id}/
//       messages.json                — messages for that session
class JsonMessageStore : public MessageStore {
 public:
  explicit JsonMessageStore(const std::filesystem::path &base_dir);

  // MessageStore interface
  void save(const Message &msg) override;
  std::optional<Message> get(const MessageId &id) override;
  std::vector<Message> list(const SessionId &session_id) override;
  void update(const Message &msg) override;
  void remove(const MessageId &id) override;

  // Session management (extra methods beyond MessageStore)
  void save_session(const SessionMeta &meta);
  std::optional<SessionMeta> get_session(const SessionId &id);
  std::vector<SessionMeta> list_sessions();
  void remove_session(const SessionId &id);

 private:
  std::filesystem::path base_dir_;
  mutable std::mutex mutex_;

  // Path helpers
  std::filesystem::path session_dir(const SessionId &id) const;
  std::filesystem::path messages_file(const SessionId &id) const;
  std::filesystem::path sessions_index_file() const;

  // Atomic write: write to .tmp then rename
  void atomic_write(const std::filesystem::path &path, const std::string &content);

  // Internal: load/save messages.json for a session
  std::vector<Message> load_messages(const SessionId &session_id);
  void save_messages(const SessionId &session_id, const std::vector<Message> &messages);

  // Internal: load/save sessions.json index
  std::vector<SessionMeta> load_sessions_index();
  void save_sessions_index(const std::vector<SessionMeta> &sessions);
};

}  // namespace agent
