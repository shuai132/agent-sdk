#include "http_client.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include <regex>
#include <sstream>
#include <thread>

namespace agent::net {

// URL parsing
std::optional<ParsedUrl> ParsedUrl::parse(const std::string& url) {
  // Simple regex-based URL parser
  std::regex url_regex(R"(^(https?):\/\/([^:\/\s]+)(?::(\d+))?(\/[^\?\s]*)?(\?[^\s]*)?)");
  std::smatch match;

  if (!std::regex_match(url, match, url_regex)) {
    return std::nullopt;
  }

  ParsedUrl result;
  result.scheme = match[1].str();
  result.host = match[2].str();
  result.port = match[3].str();
  result.path = match[4].str().empty() ? "/" : match[4].str();
  result.query = match[5].str();

  return result;
}

std::string ParsedUrl::port_or_default() const {
  if (!port.empty()) return port;
  return is_https() ? "443" : "80";
}

// HTTP Client implementation
class HttpClient::Impl {
 public:
  explicit Impl(asio::io_context& io_ctx) : io_ctx_(io_ctx), ssl_ctx_(asio::ssl::context::tlsv12_client), resolver_(io_ctx) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(asio::ssl::verify_peer);
  }

  void request(const std::string& url, const HttpOptions& options, std::function<void(HttpResponse)> callback) {
    auto parsed = ParsedUrl::parse(url);
    if (!parsed) {
      callback(HttpResponse{0, {}, "", "Invalid URL"});
      return;
    }

    if (parsed->is_https()) {
      request_https(*parsed, options, std::move(callback));
    } else {
      request_http(*parsed, options, std::move(callback));
    }
  }

  void request_stream(const std::string& url, const HttpOptions& options, StreamDataCallback on_data,
                      std::function<void(int, const std::string&)> on_complete) {
    auto parsed = ParsedUrl::parse(url);
    if (!parsed) {
      on_complete(0, "Invalid URL");
      return;
    }

    if (parsed->is_https()) {
      request_stream_https(*parsed, options, std::move(on_data), std::move(on_complete));
    } else {
      request_stream_http(*parsed, options, std::move(on_data), std::move(on_complete));
    }
  }

 private:
  // Helper: close the lowest-layer socket, ignoring errors
  template <typename Socket>
  static void close_socket(std::shared_ptr<Socket> socket) {
    asio::error_code ignored;
    socket->lowest_layer().close(ignored);
  }

  static void close_socket(std::shared_ptr<asio::ip::tcp::socket> socket) {
    asio::error_code ignored;
    socket->close(ignored);
  }

  // Start a timeout timer. When it fires, set the timed_out flag and close the socket.
  template <typename Socket>
  static std::shared_ptr<asio::steady_timer> start_timeout(asio::io_context& io_ctx, std::chrono::seconds timeout, std::shared_ptr<Socket> socket,
                                                           std::shared_ptr<bool> timed_out) {
    auto timer = std::make_shared<asio::steady_timer>(io_ctx);
    timer->expires_after(timeout);
    timer->async_wait([socket, timed_out, timer](const asio::error_code& ec) {
      if (!ec) {
        // Timer fired (not cancelled) — mark as timed out and close socket
        *timed_out = true;
        close_socket(socket);
      }
    });
    return timer;
  }

  void request_https(const ParsedUrl& url, const HttpOptions& options, std::function<void(HttpResponse)> callback) {
    auto socket = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(io_ctx_, ssl_ctx_);
    auto response = std::make_shared<HttpResponse>();
    auto request_str = std::make_shared<std::string>();
    auto buffer = std::make_shared<asio::streambuf>();
    auto timed_out = std::make_shared<bool>(false);

    // Start timeout timer
    auto timer = start_timeout(io_ctx_, options.timeout, socket, timed_out);

    // Wrap callback to cancel timer and check timeout
    auto guarded_callback = [timer, timed_out, callback](HttpResponse resp) {
      timer->cancel();
      if (*timed_out) {
        resp.error = "Request timed out";
        resp.status_code = 0;
      }
      callback(std::move(resp));
    };

    // Build HTTP request
    std::ostringstream req;
    req << options.method << " " << url.path << url.query << " HTTP/1.1\r\n";
    req << "Host: " << url.host << "\r\n";
    req << "Connection: close\r\n";

    for (const auto& [key, value] : options.headers) {
      req << key << ": " << value << "\r\n";
    }

    if (!options.body.empty()) {
      req << "Content-Length: " << options.body.size() << "\r\n";
    }

    req << "\r\n";
    req << options.body;
    *request_str = req.str();

    // Set SNI hostname
    SSL_set_tlsext_host_name(socket->native_handle(), url.host.c_str());

    // Resolve and connect
    resolver_.async_resolve(
        url.host, url.port_or_default(),
        [this, socket, request_str, response, buffer, guarded_callback, url](const asio::error_code& ec,
                                                                             asio::ip::tcp::resolver::results_type results) {
          if (ec) {
            response->error = "DNS resolution failed: " + ec.message();
            guarded_callback(*response);
            return;
          }

          asio::async_connect(
              socket->lowest_layer(), results,
              [this, socket, request_str, response, buffer, guarded_callback](const asio::error_code& ec, const asio::ip::tcp::endpoint&) {
                if (ec) {
                  response->error = "Connection failed: " + ec.message();
                  guarded_callback(*response);
                  return;
                }

                // SSL handshake
                socket->async_handshake(asio::ssl::stream_base::client,
                                        [this, socket, request_str, response, buffer, guarded_callback](const asio::error_code& ec) {
                                          if (ec) {
                                            response->error = "SSL handshake failed: " + ec.message();
                                            guarded_callback(*response);
                                            return;
                                          }

                                          // Send request
                                          asio::async_write(*socket, asio::buffer(*request_str),
                                                            [this, socket, response, buffer, guarded_callback](const asio::error_code& ec, size_t) {
                                                              if (ec) {
                                                                response->error = "Write failed: " + ec.message();
                                                                guarded_callback(*response);
                                                                return;
                                                              }

                                                              // Read response
                                                              read_response(socket, response, buffer, guarded_callback);
                                                            });
                                        });
              });
        });
  }

  void request_http(const ParsedUrl& url, const HttpOptions& options, std::function<void(HttpResponse)> callback) {
    auto socket = std::make_shared<asio::ip::tcp::socket>(io_ctx_);
    auto response = std::make_shared<HttpResponse>();
    auto request_str = std::make_shared<std::string>();
    auto buffer = std::make_shared<asio::streambuf>();
    auto timed_out = std::make_shared<bool>(false);

    // Start timeout timer
    auto timer = start_timeout(io_ctx_, options.timeout, socket, timed_out);

    // Wrap callback to cancel timer and check timeout
    auto guarded_callback = [timer, timed_out, callback](HttpResponse resp) {
      timer->cancel();
      if (*timed_out) {
        resp.error = "Request timed out";
        resp.status_code = 0;
      }
      callback(std::move(resp));
    };

    // Build HTTP request
    std::ostringstream req;
    req << options.method << " " << url.path << url.query << " HTTP/1.1\r\n";
    req << "Host: " << url.host << "\r\n";
    req << "Connection: close\r\n";

    for (const auto& [key, value] : options.headers) {
      req << key << ": " << value << "\r\n";
    }

    if (!options.body.empty()) {
      req << "Content-Length: " << options.body.size() << "\r\n";
    }

    req << "\r\n";
    req << options.body;
    *request_str = req.str();

    resolver_.async_resolve(
        url.host, url.port_or_default(),
        [this, socket, request_str, response, buffer, guarded_callback](const asio::error_code& ec, asio::ip::tcp::resolver::results_type results) {
          if (ec) {
            response->error = "DNS resolution failed: " + ec.message();
            guarded_callback(*response);
            return;
          }

          asio::async_connect(
              *socket, results,
              [this, socket, request_str, response, buffer, guarded_callback](const asio::error_code& ec, const asio::ip::tcp::endpoint&) {
                if (ec) {
                  response->error = "Connection failed: " + ec.message();
                  guarded_callback(*response);
                  return;
                }

                asio::async_write(*socket, asio::buffer(*request_str),
                                  [this, socket, response, buffer, guarded_callback](const asio::error_code& ec, size_t) {
                                    if (ec) {
                                      response->error = "Write failed: " + ec.message();
                                      guarded_callback(*response);
                                      return;
                                    }

                                    read_response(socket, response, buffer, guarded_callback);
                                  });
              });
        });
  }

  template <typename Socket>
  void read_response(std::shared_ptr<Socket> socket, std::shared_ptr<HttpResponse> response, std::shared_ptr<asio::streambuf> buffer,
                     std::function<void(HttpResponse)> callback) {
    asio::async_read_until(*socket, *buffer, "\r\n\r\n",
                           [this, socket, response, buffer, callback](const asio::error_code& ec, size_t bytes_transferred) {
                             if (ec && ec != asio::error::eof) {
                               response->error = "Read headers failed: " + ec.message();
                               callback(*response);
                               return;
                             }

                             // Parse status line and headers
                             std::istream stream(buffer.get());
                             std::string status_line;
                             std::getline(stream, status_line);

                             // Parse status code
                             std::regex status_regex(R"(HTTP/[\d.]+ (\d+))");
                             std::smatch match;
                             if (std::regex_search(status_line, match, status_regex)) {
                               try {
                                 response->status_code = std::stoi(match[1].str());
                               } catch (const std::exception&) {
                                 response->error = "Invalid HTTP response: cannot parse status code";
                                 callback(*response);
                                 return;
                               }
                             }

                             // Parse headers
                             std::string header_line;
                             while (std::getline(stream, header_line) && header_line != "\r") {
                               auto colon = header_line.find(':');
                               if (colon != std::string::npos) {
                                 std::string key = header_line.substr(0, colon);
                                 std::string value = header_line.substr(colon + 1);
                                 // Trim whitespace
                                 value.erase(0, value.find_first_not_of(" \t"));
                                 value.erase(value.find_last_not_of(" \t\r\n") + 1);
                                 response->headers[key] = value;
                               }
                             }

                             // Read body
                             read_body(socket, response, buffer, callback);
                           });
  }

  template <typename Socket>
  void read_body(std::shared_ptr<Socket> socket, std::shared_ptr<HttpResponse> response, std::shared_ptr<asio::streambuf> buffer,
                 std::function<void(HttpResponse)> callback) {
    // First, add any remaining data in buffer to body
    if (buffer->size() > 0) {
      std::istream stream(buffer.get());
      std::ostringstream body;
      body << stream.rdbuf();
      response->body += body.str();
    }

    // Check if we have Content-Length and already have all data
    auto it = response->headers.find("Content-Length");
    if (it != response->headers.end()) {
      try {
        size_t content_length = std::stoull(it->second);
        if (response->body.size() >= content_length) {
          // We have all the data
          callback(*response);
          return;
        }
      } catch (const std::exception&) {
        // Invalid Content-Length, ignore and continue reading
      }
    }

    // Continue reading until EOF or we have all data
    asio::async_read(*socket, *buffer, asio::transfer_at_least(1),
                     [this, socket, response, buffer, callback](const asio::error_code& ec, size_t bytes_transferred) {
                       // SSL connections may return various errors on close
                       // Treat any SSL category error as potential EOF
                       bool is_eof =
                           (ec == asio::error::eof) || (ec.category() == asio::error::get_ssl_category()) || ec == asio::ssl::error::stream_truncated;

                       if (ec && !is_eof) {
                         // Some error, but we may have partial data
                         callback(*response);
                         return;
                       }

                       // Append to body
                       if (buffer->size() > 0) {
                         std::istream stream(buffer.get());
                         std::ostringstream more;
                         more << stream.rdbuf();
                         response->body += more.str();
                       }

                       if (is_eof) {
                         callback(*response);
                       } else {
                         // Check Content-Length again
                         auto it = response->headers.find("Content-Length");
                         if (it != response->headers.end()) {
                           try {
                             size_t content_length = std::stoull(it->second);
                             if (response->body.size() >= content_length) {
                               callback(*response);
                               return;
                             }
                           } catch (const std::exception&) {
                             // Invalid Content-Length, ignore and continue reading
                           }
                         }
                         read_body(socket, response, buffer, callback);
                       }
                     });
  }

  // Streaming request implementations
  void request_stream_https(const ParsedUrl& url, const HttpOptions& options, StreamDataCallback on_data,
                            std::function<void(int, const std::string&)> on_complete) {
    auto socket = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(io_ctx_, ssl_ctx_);
    auto request_str = std::make_shared<std::string>();
    auto buffer = std::make_shared<asio::streambuf>();
    auto status_code = std::make_shared<int>(0);
    auto shared_on_data = std::make_shared<StreamDataCallback>(std::move(on_data));
    auto timed_out = std::make_shared<bool>(false);

    // Start timeout timer
    auto timer = start_timeout(io_ctx_, options.timeout, socket, timed_out);

    // Wrap on_complete to cancel timer and check timeout
    auto shared_on_complete = std::make_shared<std::function<void(int, const std::string&)>>(
        [timer, timed_out, on_complete = std::move(on_complete)](int code, const std::string& err) {
          timer->cancel();
          if (*timed_out) {
            on_complete(0, "Request timed out");
          } else {
            on_complete(code, err);
          }
        });

    // Build HTTP request
    std::ostringstream req;
    req << options.method << " " << url.path << url.query << " HTTP/1.1\r\n";
    req << "Host: " << url.host << "\r\n";
    req << "Connection: close\r\n";

    for (const auto& [key, value] : options.headers) {
      req << key << ": " << value << "\r\n";
    }

    if (!options.body.empty()) {
      req << "Content-Length: " << options.body.size() << "\r\n";
    }

    req << "\r\n";
    req << options.body;
    *request_str = req.str();

    // Set SNI hostname
    SSL_set_tlsext_host_name(socket->native_handle(), url.host.c_str());

    // Resolve and connect
    resolver_.async_resolve(
        url.host, url.port_or_default(),
        [this, socket, request_str, buffer, status_code, shared_on_data, shared_on_complete](const asio::error_code& ec,
                                                                                             asio::ip::tcp::resolver::results_type results) {
          if (ec) {
            (*shared_on_complete)(0, "DNS resolution failed: " + ec.message());
            return;
          }

          asio::async_connect(
              socket->lowest_layer(), results,
              [this, socket, request_str, buffer, status_code, shared_on_data, shared_on_complete](const asio::error_code& ec,
                                                                                                   const asio::ip::tcp::endpoint&) {
                if (ec) {
                  (*shared_on_complete)(0, "Connection failed: " + ec.message());
                  return;
                }

                socket->async_handshake(asio::ssl::stream_base::client, [this, socket, request_str, buffer, status_code, shared_on_data,
                                                                         shared_on_complete](const asio::error_code& ec) {
                  if (ec) {
                    (*shared_on_complete)(0, "SSL handshake failed: " + ec.message());
                    return;
                  }

                  asio::async_write(*socket, asio::buffer(*request_str),
                                    [this, socket, buffer, status_code, shared_on_data, shared_on_complete](const asio::error_code& ec, size_t) {
                                      if (ec) {
                                        (*shared_on_complete)(0, "Write failed: " + ec.message());
                                        return;
                                      }

                                      read_stream_headers(socket, buffer, status_code, shared_on_data, shared_on_complete);
                                    });
                });
              });
        });
  }

  void request_stream_http(const ParsedUrl& url, const HttpOptions& options, StreamDataCallback on_data,
                           std::function<void(int, const std::string&)> on_complete) {
    auto socket = std::make_shared<asio::ip::tcp::socket>(io_ctx_);
    auto request_str = std::make_shared<std::string>();
    auto buffer = std::make_shared<asio::streambuf>();
    auto status_code = std::make_shared<int>(0);
    auto shared_on_data = std::make_shared<StreamDataCallback>(std::move(on_data));
    auto timed_out = std::make_shared<bool>(false);

    // Start timeout timer
    auto timer = start_timeout(io_ctx_, options.timeout, socket, timed_out);

    // Wrap on_complete to cancel timer and check timeout
    auto shared_on_complete = std::make_shared<std::function<void(int, const std::string&)>>(
        [timer, timed_out, on_complete = std::move(on_complete)](int code, const std::string& err) {
          timer->cancel();
          if (*timed_out) {
            on_complete(0, "Request timed out");
          } else {
            on_complete(code, err);
          }
        });

    // Build HTTP request
    std::ostringstream req;
    req << options.method << " " << url.path << url.query << " HTTP/1.1\r\n";
    req << "Host: " << url.host << "\r\n";
    req << "Connection: close\r\n";

    for (const auto& [key, value] : options.headers) {
      req << key << ": " << value << "\r\n";
    }

    if (!options.body.empty()) {
      req << "Content-Length: " << options.body.size() << "\r\n";
    }

    req << "\r\n";
    req << options.body;
    *request_str = req.str();

    resolver_.async_resolve(url.host, url.port_or_default(),
                            [this, socket, request_str, buffer, status_code, shared_on_data, shared_on_complete](
                                const asio::error_code& ec, asio::ip::tcp::resolver::results_type results) {
                              if (ec) {
                                (*shared_on_complete)(0, "DNS resolution failed: " + ec.message());
                                return;
                              }

                              asio::async_connect(
                                  *socket, results,
                                  [this, socket, request_str, buffer, status_code, shared_on_data, shared_on_complete](
                                      const asio::error_code& ec, const asio::ip::tcp::endpoint&) {
                                    if (ec) {
                                      (*shared_on_complete)(0, "Connection failed: " + ec.message());
                                      return;
                                    }

                                    asio::async_write(
                                        *socket, asio::buffer(*request_str),
                                        [this, socket, buffer, status_code, shared_on_data, shared_on_complete](const asio::error_code& ec, size_t) {
                                          if (ec) {
                                            (*shared_on_complete)(0, "Write failed: " + ec.message());
                                            return;
                                          }

                                          read_stream_headers(socket, buffer, status_code, shared_on_data, shared_on_complete);
                                        });
                                  });
                            });
  }

  template <typename Socket>
  void read_stream_headers(std::shared_ptr<Socket> socket, std::shared_ptr<asio::streambuf> buffer, std::shared_ptr<int> status_code,
                           std::shared_ptr<StreamDataCallback> on_data, std::shared_ptr<std::function<void(int, const std::string&)>> on_complete) {
    asio::async_read_until(*socket, *buffer, "\r\n\r\n",
                           [this, socket, buffer, status_code, on_data, on_complete](const asio::error_code& ec, size_t bytes_transferred) {
                             if (ec && ec != asio::error::eof) {
                               (*on_complete)(0, "Read headers failed: " + ec.message());
                               return;
                             }

                             // Parse status line
                             std::istream stream(buffer.get());
                             std::string status_line;
                             std::getline(stream, status_line);

                             std::regex status_regex(R"(HTTP/[\d.]+ (\d+))");
                             std::smatch match;
                             if (std::regex_search(status_line, match, status_regex)) {
                               try {
                                 *status_code = std::stoi(match[1].str());
                               } catch (const std::exception&) {
                                 (*on_complete)(0, "Invalid HTTP response: cannot parse status code");
                                 return;
                               }
                             }

                             // Skip headers
                             std::string header_line;
                             while (std::getline(stream, header_line) && header_line != "\r") {
                               // Just consume headers
                             }

                             // Check status code
                             if (*status_code < 200 || *status_code >= 300) {
                               // Read error body
                               std::ostringstream error_body;
                               error_body << stream.rdbuf();
                               (*on_complete)(*status_code, "HTTP error " + std::to_string(*status_code) + ": " + error_body.str());
                               return;
                             }

                             // Process any remaining data in buffer as first chunk
                             if (buffer->size() > 0) {
                               std::ostringstream chunk;
                               chunk << stream.rdbuf();
                               std::string chunk_str = chunk.str();
                               if (!chunk_str.empty()) {
                                 (*on_data)(chunk_str);
                               }
                             }

                             // Continue reading stream data
                             read_stream_data(socket, buffer, status_code, on_data, on_complete);
                           });
  }

  template <typename Socket>
  void read_stream_data(std::shared_ptr<Socket> socket, std::shared_ptr<asio::streambuf> buffer, std::shared_ptr<int> status_code,
                        std::shared_ptr<StreamDataCallback> on_data, std::shared_ptr<std::function<void(int, const std::string&)>> on_complete) {
    asio::async_read(*socket, *buffer, asio::transfer_at_least(1),
                     [this, socket, buffer, status_code, on_data, on_complete](const asio::error_code& ec, size_t bytes_transferred) {
                       bool is_eof =
                           (ec == asio::error::eof) || (ec.category() == asio::error::get_ssl_category()) || ec == asio::ssl::error::stream_truncated;

                       if (ec && !is_eof) {
                         (*on_complete)(*status_code, "Read failed: " + ec.message());
                         return;
                       }

                       // Send chunk to callback
                       if (buffer->size() > 0) {
                         std::istream stream(buffer.get());
                         std::ostringstream chunk;
                         chunk << stream.rdbuf();
                         std::string chunk_str = chunk.str();
                         if (!chunk_str.empty()) {
                           (*on_data)(chunk_str);
                         }
                       }

                       if (is_eof) {
                         (*on_complete)(*status_code, "");
                       } else {
                         read_stream_data(socket, buffer, status_code, on_data, on_complete);
                       }
                     });
  }

  asio::io_context& io_ctx_;
  asio::ssl::context ssl_ctx_;
  asio::ip::tcp::resolver resolver_;
};

HttpClient::HttpClient(asio::io_context& io_ctx) : impl_(std::make_unique<Impl>(io_ctx)) {}

HttpClient::~HttpClient() = default;

void HttpClient::request(const std::string& url, const HttpOptions& options, std::function<void(HttpResponse)> callback) {
  impl_->request(url, options, std::move(callback));
}

std::future<HttpResponse> HttpClient::request(const std::string& url, const HttpOptions& options) {
  if (options.max_retries <= 0) {
    // No retry — preserve original behavior
    auto promise = std::make_shared<std::promise<HttpResponse>>();
    auto future = promise->get_future();

    impl_->request(url, options, [promise](HttpResponse response) {
      promise->set_value(std::move(response));
    });

    return future;
  }

  // With retry — use std::async to drive the retry loop
  return std::async(std::launch::async, [this, url, options]() -> HttpResponse {
    auto is_retryable = [](const HttpResponse& resp) -> bool {
      // Connection/timeout errors (status_code == 0 means no HTTP response received)
      if (resp.status_code == 0) return true;
      // 429 Too Many Requests — but not for quota exhaustion
      if (resp.status_code == 429) {
        // Check if this is a quota exhaustion error (not retryable)
        if (resp.body.find("insufficient_quota") != std::string::npos) return false;
        if (resp.body.find("quota_exceeded") != std::string::npos) return false;
        if (resp.body.find("billing") != std::string::npos) return false;
        // Rate limit errors are retryable
        return true;
      }
      // 5xx server errors worth retrying
      if (resp.status_code == 500 || resp.status_code == 502 || resp.status_code == 503 || resp.status_code == 504) return true;
      return false;
    };

    HttpResponse last_response;
    int max_attempts = 1 + options.max_retries;

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      auto promise = std::make_shared<std::promise<HttpResponse>>();
      auto future = promise->get_future();

      impl_->request(url, options, [promise](HttpResponse response) {
        promise->set_value(std::move(response));
      });

      last_response = future.get();

      // Success — return immediately
      if (last_response.ok()) {
        return last_response;
      }

      // Not retryable — return immediately
      if (!is_retryable(last_response)) {
        return last_response;
      }

      // Last attempt — don't sleep, just return
      if (attempt + 1 >= max_attempts) {
        break;
      }

      spdlog::warn("HTTP request to {} failed (status={}, error={}), retrying {}/{}...", url, last_response.status_code, last_response.error,
                   attempt + 1, options.max_retries);
      std::this_thread::sleep_for(options.retry_delay);
    }

    return last_response;
  });
}

void HttpClient::request_stream(const std::string& url, const HttpOptions& options, StreamDataCallback on_data,
                                std::function<void(int status_code, const std::string& error)> on_complete) {
  impl_->request_stream(url, options, std::move(on_data), std::move(on_complete));
}

std::future<HttpResponse> HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
  HttpOptions options;
  options.method = "GET";
  options.headers = headers;
  return request(url, options);
}

std::future<HttpResponse> HttpClient::post(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
  HttpOptions options;
  options.method = "POST";
  options.body = body;
  options.headers = headers;
  return request(url, options);
}

}  // namespace agent::net
