#pragma once

#include <any>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <vector>

namespace agent {

// Type-safe event bus for internal communication
class Bus {
 public:
  using SubscriptionId = uint64_t;

  static Bus &instance();

  // Subscribe to events of type T
  template <typename T>
  SubscriptionId subscribe(std::function<void(const T &)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = next_id_++;
    auto type_idx = std::type_index(typeid(T));

    handlers_[type_idx].push_back({id, [handler](const std::any &event) {
                                     handler(std::any_cast<const T &>(event));
                                   }});

    return id;
  }

  // Unsubscribe
  void unsubscribe(SubscriptionId id);

  // Publish an event
  template <typename T>
  void publish(const T &event) {
    std::vector<std::function<void(const std::any &)>> to_call;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto type_idx = std::type_index(typeid(T));
      auto it = handlers_.find(type_idx);
      if (it != handlers_.end()) {
        for (const auto &entry : it->second) {
          to_call.push_back(entry.handler);
        }
      }
    }

    // Call handlers outside the lock
    std::any wrapped = event;
    for (const auto &handler : to_call) {
      handler(wrapped);
    }
  }

 private:
  Bus() = default;

  struct HandlerEntry {
    SubscriptionId id;
    std::function<void(const std::any &)> handler;
  };

  std::mutex mutex_;
  SubscriptionId next_id_ = 1;
  std::map<std::type_index, std::vector<HandlerEntry>> handlers_;
};

// Common events
namespace events {

struct SessionCreated {
  std::string session_id;
};

struct SessionEnded {
  std::string session_id;
};

struct MessageAdded {
  std::string session_id;
  std::string message_id;
};

struct ToolCallStarted {
  std::string session_id;
  std::string tool_id;
  std::string tool_name;
};

struct ToolCallCompleted {
  std::string session_id;
  std::string tool_id;
  std::string tool_name;
  bool success;
};

struct StreamDelta {
  std::string session_id;
  std::string text;
};

struct TokensUsed {
  std::string session_id;
  int64_t input_tokens;
  int64_t output_tokens;
};

struct ContextCompacted {
  std::string session_id;
  int64_t tokens_before;
  int64_t tokens_after;
};

struct PermissionRequested {
  std::string session_id;
  std::string tool_name;
  std::string description;
  std::function<void(bool)> respond;
};

struct McpToolsChanged {
  std::string server_name;
};

}  // namespace events

}  // namespace agent
