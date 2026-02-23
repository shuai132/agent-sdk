#include "message.hpp"

#include <algorithm>

namespace agent {

std::string to_string(Role role) {
  switch (role) {
    case Role::System:
      return "system";
    case Role::User:
      return "user";
    case Role::Assistant:
      return "assistant";
  }
  return "user";
}

Role role_from_string(const std::string& str) {
  if (str == "system") return Role::System;
  if (str == "user") return Role::User;
  if (str == "assistant") return Role::Assistant;
  return Role::User;
}

Message::Message(Role role, const std::string& content) : role_(role) {
  if (!content.empty()) {
    parts_.push_back(TextPart{content});
  }
}

Message Message::system(const std::string& content) {
  return Message(Role::System, content);
}

Message Message::user(const std::string& content) {
  return Message(Role::User, content);
}

Message Message::assistant(const std::string& content) {
  return Message(Role::Assistant, content);
}

void Message::add_part(MessagePart part) {
  parts_.push_back(std::move(part));
}

void Message::add_text(const std::string& text) {
  parts_.push_back(TextPart{text});
}

void Message::add_tool_call(const std::string& id, const std::string& name, const json& args) {
  parts_.push_back(ToolCallPart{id, name, args, false, false});
}

void Message::add_tool_result(const std::string& call_id, const std::string& name, const std::string& output, bool is_error) {
  parts_.push_back(ToolResultPart{call_id, name, output, is_error, std::nullopt, json::object(), false, std::nullopt});
}

std::string Message::text() const {
  std::string result;
  for (const auto& part : parts_) {
    if (auto* text = std::get_if<TextPart>(&part)) {
      if (!result.empty()) result += "\n";
      result += text->text;
    }
  }
  return result;
}

std::vector<ToolCallPart*> Message::tool_calls() {
  std::vector<ToolCallPart*> result;
  for (auto& part : parts_) {
    if (auto* tc = std::get_if<ToolCallPart>(&part)) {
      result.push_back(tc);
    }
  }
  return result;
}

std::vector<const ToolCallPart*> Message::tool_calls() const {
  std::vector<const ToolCallPart*> result;
  for (const auto& part : parts_) {
    if (auto* tc = std::get_if<ToolCallPart>(&part)) {
      result.push_back(tc);
    }
  }
  return result;
}

std::vector<ToolResultPart*> Message::tool_results() {
  std::vector<ToolResultPart*> result;
  for (auto& part : parts_) {
    if (auto* tr = std::get_if<ToolResultPart>(&part)) {
      result.push_back(tr);
    }
  }
  return result;
}

std::vector<const ToolResultPart*> Message::tool_results() const {
  std::vector<const ToolResultPart*> result;
  for (const auto& part : parts_) {
    if (auto* tr = std::get_if<ToolResultPart>(&part)) {
      result.push_back(tr);
    }
  }
  return result;
}

json Message::to_json() const {
  json j;
  j["id"] = id_;
  j["role"] = to_string(role_);
  j["finished"] = finished_;
  j["finish_reason"] = to_string(finish_reason_);
  j["is_summary"] = is_summary_;
  j["is_synthetic"] = is_synthetic_;

  if (parent_id_) {
    j["parent_id"] = *parent_id_;
  }

  j["session_id"] = session_id_;

  json parts_json = json::array();
  for (const auto& part : parts_) {
    json part_json;
    if (auto* text = std::get_if<TextPart>(&part)) {
      part_json["type"] = "text";
      part_json["text"] = text->text;
    } else if (auto* tc = std::get_if<ToolCallPart>(&part)) {
      part_json["type"] = "tool_call";
      part_json["id"] = tc->id;
      part_json["name"] = tc->name;
      part_json["arguments"] = tc->arguments;
      part_json["started"] = tc->started;
      part_json["completed"] = tc->completed;
    } else if (auto* tr = std::get_if<ToolResultPart>(&part)) {
      part_json["type"] = "tool_result";
      part_json["tool_call_id"] = tr->tool_call_id;
      part_json["tool_name"] = tr->tool_name;
      part_json["output"] = tr->output;
      part_json["is_error"] = tr->is_error;
      part_json["compacted"] = tr->compacted;
    }
    parts_json.push_back(part_json);
  }
  j["parts"] = parts_json;

  j["usage"] = {{"input_tokens", usage_.input_tokens},
                {"output_tokens", usage_.output_tokens},
                {"cache_read_tokens", usage_.cache_read_tokens},
                {"cache_write_tokens", usage_.cache_write_tokens}};

  j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(created_at_.time_since_epoch()).count();

  return j;
}

Message Message::from_json(const json& j) {
  Message msg;
  msg.id_ = j.value("id", UUID::generate());
  msg.role_ = role_from_string(j.value("role", "user"));
  msg.finished_ = j.value("finished", false);
  msg.finish_reason_ = finish_reason_from_string(j.value("finish_reason", "stop"));
  msg.is_summary_ = j.value("is_summary", false);
  msg.is_synthetic_ = j.value("is_synthetic", false);

  if (j.contains("parent_id")) {
    msg.parent_id_ = j["parent_id"].get<std::string>();
  }

  msg.session_id_ = j.value("session_id", "");

  if (j.contains("parts")) {
    for (const auto& part_json : j["parts"]) {
      std::string type = part_json.value("type", "");
      if (type == "text") {
        msg.parts_.push_back(TextPart{part_json["text"]});
      } else if (type == "tool_call") {
        msg.parts_.push_back(ToolCallPart{part_json["id"], part_json["name"], part_json["arguments"], part_json.value("started", false),
                                          part_json.value("completed", false)});
      } else if (type == "tool_result") {
        msg.parts_.push_back(ToolResultPart{part_json["tool_call_id"], part_json["tool_name"], part_json["output"],
                                            part_json.value("is_error", false), std::nullopt, json::object(), part_json.value("compacted", false),
                                            std::nullopt});
      }
    }
  }

  if (j.contains("usage")) {
    const auto& u = j["usage"];
    msg.usage_.input_tokens = u.value("input_tokens", 0);
    msg.usage_.output_tokens = u.value("output_tokens", 0);
    msg.usage_.cache_read_tokens = u.value("cache_read_tokens", 0);
    msg.usage_.cache_write_tokens = u.value("cache_write_tokens", 0);
  }

  if (j.contains("created_at")) {
    msg.created_at_ = Timestamp(std::chrono::seconds(j["created_at"].get<int64_t>()));
  }

  return msg;
}

json Message::to_api_format() const {
  // Convert to OpenAI-style format (also works with Anthropic via adapter)
  json msg;
  msg["role"] = to_string(role_);

  // Collect text content (OpenAI format)
  std::string text_content;
  bool has_tool_calls = false;

  for (const auto& part : parts_) {
    if (auto* text = std::get_if<TextPart>(&part)) {
      if (!text_content.empty()) {
        text_content += "\n";
      }
      text_content += text->text;
    } else if (auto* tc = std::get_if<ToolCallPart>(&part)) {
      // Tool calls go into a separate field in the message
      has_tool_calls = true;
      if (!msg.contains("tool_calls")) {
        msg["tool_calls"] = json::array();
      }
      msg["tool_calls"].push_back({{"id", tc->id}, {"type", "function"}, {"function", {{"name", tc->name}, {"arguments", tc->arguments.dump()}}}});
    } else if (auto* img = std::get_if<ImagePart>(&part)) {
      // For images, we need to use array format
      if (!msg.contains("content") || !msg["content"].is_array()) {
        json content_array = json::array();
        if (!text_content.empty()) {
          content_array.push_back({{"type", "text"}, {"text", text_content}});
          text_content.clear();
        }
        msg["content"] = content_array;
      }
      msg["content"].push_back({{"type", "image_url"}, {"image_url", {{"url", img->url}}}});
    }
    // Note: ToolResultPart is handled separately in to_openai_format() as role="tool" messages
  }

  // Set content field
  if (msg.contains("content") && msg["content"].is_array()) {
    // Already has array content (from images), add remaining text if any
    if (!text_content.empty()) {
      // Insert text at beginning
      json text_part = {{"type", "text"}, {"text", text_content}};
      msg["content"].insert(msg["content"].begin(), text_part);
    }
  } else if (!text_content.empty()) {
    // Simple string content
    msg["content"] = text_content;
  } else if (has_tool_calls) {
    // OpenAI requires content field for assistant messages with tool_calls
    // Content should be null or empty string
    msg["content"] = nullptr;
  }

  return msg;
}

// InMemoryMessageStore implementation
void InMemoryMessageStore::save(const Message& msg) {
  std::lock_guard lock(mutex_);
  messages_[msg.id()] = msg;
  session_messages_[msg.session_id()].push_back(msg.id());
}

std::optional<Message> InMemoryMessageStore::get(const MessageId& id) {
  std::lock_guard lock(mutex_);
  auto it = messages_.find(id);
  if (it != messages_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::vector<Message> InMemoryMessageStore::list(const SessionId& session_id) {
  std::lock_guard lock(mutex_);
  std::vector<Message> result;
  auto it = session_messages_.find(session_id);
  if (it != session_messages_.end()) {
    for (const auto& id : it->second) {
      auto msg_it = messages_.find(id);
      if (msg_it != messages_.end()) {
        result.push_back(msg_it->second);
      }
    }
  }
  return result;
}

void InMemoryMessageStore::update(const Message& msg) {
  std::lock_guard lock(mutex_);
  messages_[msg.id()] = msg;
}

void InMemoryMessageStore::remove(const MessageId& id) {
  std::lock_guard lock(mutex_);
  auto it = messages_.find(id);
  if (it != messages_.end()) {
    // Remove from session list
    auto& session_msgs = session_messages_[it->second.session_id()];
    session_msgs.erase(std::remove(session_msgs.begin(), session_msgs.end(), id), session_msgs.end());
    messages_.erase(it);
  }
}

}  // namespace agent
