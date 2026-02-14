#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace agent::net {

// SSE event
struct SseEvent {
  std::string event;  // Event type (empty for default "message")
  std::string data;   // Event data
  std::string id;     // Event ID (optional)
};

// SSE client for streaming responses
class SseClient {
 public:
  explicit SseClient(asio::io_context &io_ctx);

  ~SseClient();

  // Connect and start streaming
  void connect(const std::string &url, const std::map<std::string, std::string> &headers, std::function<void(const SseEvent &)> on_event,
               std::function<void(const std::string &error)> on_error, std::function<void()> on_complete);

  // Stop streaming
  void stop();

  // Check if connected
  bool is_connected() const;

 private:
  class Impl;

  std::unique_ptr<Impl> impl_;
  std::atomic<bool> stopped_{false};
};

}  // namespace agent::net
