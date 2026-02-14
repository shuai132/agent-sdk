#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace agent::net {

// HTTP response
struct HttpResponse {
  int status_code = 0;
  std::map<std::string, std::string> headers;
  std::string body;
  std::string error;

  bool ok() const {
    return status_code >= 200 && status_code < 300;
  }
};

// HTTP request options
struct HttpOptions {
  std::string method = "GET";
  std::map<std::string, std::string> headers;
  std::string body;
  std::chrono::seconds timeout{30};
};

// Streaming data callback
using StreamDataCallback = std::function<void(const std::string &chunk)>;

// Async HTTP client using ASIO
class HttpClient {
 public:
  explicit HttpClient(asio::io_context &io_ctx);

  ~HttpClient();

  // Async request with callback
  void request(const std::string &url, const HttpOptions &options, std::function<void(HttpResponse)> callback);

  // Async request returning future
  std::future<HttpResponse> request(const std::string &url, const HttpOptions &options);

  // Streaming request - calls on_data for each chunk received
  void request_stream(const std::string &url, const HttpOptions &options, StreamDataCallback on_data,
                      std::function<void(int status_code, const std::string &error)> on_complete);

  // Convenience methods
  std::future<HttpResponse> get(const std::string &url, const std::map<std::string, std::string> &headers = {});

  std::future<HttpResponse> post(const std::string &url, const std::string &body, const std::map<std::string, std::string> &headers = {});

 private:
  class Impl;

  std::unique_ptr<Impl> impl_;
};

// URL parsing helper
struct ParsedUrl {
  std::string scheme;
  std::string host;
  std::string port;
  std::string path;
  std::string query;

  bool is_https() const {
    return scheme == "https";
  }

  std::string port_or_default() const;

  static std::optional<ParsedUrl> parse(const std::string &url);
};

}  // namespace agent::net
