#include "core/json_store.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

namespace agent {

namespace fs = std::filesystem;

// --- Timestamp helpers ---

static int64_t timestamp_to_epoch(const Timestamp &ts) {
  return std::chrono::duration_cast<std::chrono::seconds>(ts.time_since_epoch()).count();
}

static Timestamp epoch_to_timestamp(int64_t epoch) {
  return Timestamp(std::chrono::seconds(epoch));
}

// --- SessionMeta ---

json SessionMeta::to_json() const {
  json j;
  j["id"] = id;
  j["title"] = title;
  if (parent_id) {
    j["parent_id"] = *parent_id;
  }
  j["agent_type"] = to_string(agent_type);
  j["created_at"] = timestamp_to_epoch(created_at);
  j["updated_at"] = timestamp_to_epoch(updated_at);
  j["total_usage"] = {{"input_tokens", total_usage.input_tokens},
                      {"output_tokens", total_usage.output_tokens},
                      {"cache_read_tokens", total_usage.cache_read_tokens},
                      {"cache_write_tokens", total_usage.cache_write_tokens}};
  return j;
}

SessionMeta SessionMeta::from_json(const json &j) {
  SessionMeta meta;
  meta.id = j.value("id", "");
  meta.title = j.value("title", "");
  if (j.contains("parent_id")) {
    meta.parent_id = j["parent_id"].get<std::string>();
  }
  meta.agent_type = agent_type_from_string(j.value("agent_type", "build"));
  meta.created_at = epoch_to_timestamp(j.value("created_at", int64_t(0)));
  meta.updated_at = epoch_to_timestamp(j.value("updated_at", int64_t(0)));

  if (j.contains("total_usage")) {
    const auto &u = j["total_usage"];
    meta.total_usage.input_tokens = u.value("input_tokens", int64_t(0));
    meta.total_usage.output_tokens = u.value("output_tokens", int64_t(0));
    meta.total_usage.cache_read_tokens = u.value("cache_read_tokens", int64_t(0));
    meta.total_usage.cache_write_tokens = u.value("cache_write_tokens", int64_t(0));
  }

  return meta;
}

// --- JsonMessageStore ---

JsonMessageStore::JsonMessageStore(const fs::path &base_dir) : base_dir_(base_dir) {
  std::error_code ec;
  fs::create_directories(base_dir_, ec);
  if (ec) {
    spdlog::warn("Failed to create sessions directory {}: {}", base_dir_.string(), ec.message());
  }
}

// --- Path helpers ---

fs::path JsonMessageStore::session_dir(const SessionId &id) const {
  return base_dir_ / id;
}

fs::path JsonMessageStore::messages_file(const SessionId &id) const {
  return session_dir(id) / "messages.json";
}

fs::path JsonMessageStore::sessions_index_file() const {
  return base_dir_ / "sessions.json";
}

// --- Atomic write ---

void JsonMessageStore::atomic_write(const fs::path &path, const std::string &content) {
  auto tmp_path = path;
  tmp_path += ".tmp";

  std::ofstream file(tmp_path, std::ios::trunc);
  if (!file.is_open()) {
    spdlog::warn("Failed to open temp file for writing: {}", tmp_path.string());
    return;
  }

  file << content;
  file.close();

  if (file.fail()) {
    spdlog::warn("Failed to write temp file: {}", tmp_path.string());
    std::error_code ec;
    fs::remove(tmp_path, ec);
    return;
  }

  std::error_code ec;
  fs::rename(tmp_path, path, ec);
  if (ec) {
    spdlog::warn("Failed to rename temp file {} -> {}: {}", tmp_path.string(), path.string(), ec.message());
    fs::remove(tmp_path, ec);
  }
}

// --- Internal: messages.json ---

std::vector<Message> JsonMessageStore::load_messages(const SessionId &session_id) {
  auto path = messages_file(session_id);
  if (!fs::exists(path)) {
    return {};
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    spdlog::warn("Failed to open messages file: {}", path.string());
    return {};
  }

  try {
    json j = json::parse(file);
    std::vector<Message> messages;
    for (const auto &msg_json : j) {
      messages.push_back(Message::from_json(msg_json));
    }
    return messages;
  } catch (const std::exception &e) {
    spdlog::warn("Failed to parse messages file {}: {}", path.string(), e.what());
    return {};
  }
}

void JsonMessageStore::save_messages(const SessionId &session_id, const std::vector<Message> &messages) {
  // Ensure session directory exists
  auto dir = session_dir(session_id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    spdlog::warn("Failed to create session directory {}: {}", dir.string(), ec.message());
    return;
  }

  json j = json::array();
  for (const auto &msg : messages) {
    j.push_back(msg.to_json());
  }

  atomic_write(messages_file(session_id), j.dump(2));
}

// --- Internal: sessions.json index ---

std::vector<SessionMeta> JsonMessageStore::load_sessions_index() {
  auto path = sessions_index_file();
  if (!fs::exists(path)) {
    return {};
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    return {};
  }

  try {
    json j = json::parse(file);
    std::vector<SessionMeta> sessions;
    for (const auto &s : j) {
      sessions.push_back(SessionMeta::from_json(s));
    }
    return sessions;
  } catch (const std::exception &e) {
    spdlog::warn("Failed to parse sessions index: {}", e.what());
    return {};
  }
}

void JsonMessageStore::save_sessions_index(const std::vector<SessionMeta> &sessions) {
  json j = json::array();
  for (const auto &s : sessions) {
    j.push_back(s.to_json());
  }
  atomic_write(sessions_index_file(), j.dump(2));
}

// --- MessageStore interface ---

void JsonMessageStore::save(const Message &msg) {
  std::lock_guard lock(mutex_);

  auto session_id = msg.session_id();
  auto messages = load_messages(session_id);
  messages.push_back(msg);
  save_messages(session_id, messages);
}

std::optional<Message> JsonMessageStore::get(const MessageId &id) {
  std::lock_guard lock(mutex_);

  // Scan all session directories for the message
  if (!fs::exists(base_dir_)) {
    return std::nullopt;
  }

  for (const auto &entry : fs::directory_iterator(base_dir_)) {
    if (!entry.is_directory()) continue;

    auto session_id = entry.path().filename().string();
    auto messages = load_messages(session_id);
    for (const auto &msg : messages) {
      if (msg.id() == id) {
        return msg;
      }
    }
  }

  return std::nullopt;
}

std::vector<Message> JsonMessageStore::list(const SessionId &session_id) {
  std::lock_guard lock(mutex_);
  return load_messages(session_id);
}

void JsonMessageStore::update(const Message &msg) {
  std::lock_guard lock(mutex_);

  auto session_id = msg.session_id();
  auto messages = load_messages(session_id);

  for (auto &existing : messages) {
    if (existing.id() == msg.id()) {
      existing = msg;
      break;
    }
  }

  save_messages(session_id, messages);
}

void JsonMessageStore::remove(const MessageId &id) {
  std::lock_guard lock(mutex_);

  // Scan all session directories for the message
  if (!fs::exists(base_dir_)) {
    return;
  }

  for (const auto &entry : fs::directory_iterator(base_dir_)) {
    if (!entry.is_directory()) continue;

    auto session_id = entry.path().filename().string();
    auto messages = load_messages(session_id);
    auto it = std::remove_if(messages.begin(), messages.end(), [&id](const Message &msg) {
      return msg.id() == id;
    });

    if (it != messages.end()) {
      messages.erase(it, messages.end());
      save_messages(session_id, messages);
      return;
    }
  }
}

// --- Session management ---

void JsonMessageStore::save_session(const SessionMeta &meta) {
  std::lock_guard lock(mutex_);

  auto sessions = load_sessions_index();

  // Update existing or append
  bool found = false;
  for (auto &s : sessions) {
    if (s.id == meta.id) {
      s = meta;
      found = true;
      break;
    }
  }

  if (!found) {
    sessions.push_back(meta);
  }

  save_sessions_index(sessions);
}

std::optional<SessionMeta> JsonMessageStore::get_session(const SessionId &id) {
  std::lock_guard lock(mutex_);

  auto sessions = load_sessions_index();
  for (const auto &s : sessions) {
    if (s.id == id) {
      return s;
    }
  }

  return std::nullopt;
}

std::vector<SessionMeta> JsonMessageStore::list_sessions() {
  std::lock_guard lock(mutex_);
  return load_sessions_index();
}

void JsonMessageStore::remove_session(const SessionId &id) {
  std::lock_guard lock(mutex_);

  // Remove from index
  auto sessions = load_sessions_index();
  sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                [&id](const SessionMeta &s) {
                                  return s.id == id;
                                }),
                 sessions.end());
  save_sessions_index(sessions);

  // Remove session directory
  auto dir = session_dir(id);
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (ec) {
    spdlog::warn("Failed to remove session directory {}: {}", dir.string(), ec.message());
  }
}

}  // namespace agent
