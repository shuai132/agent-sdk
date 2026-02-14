#include "net/sse_client.hpp"

#include <regex>
#include <sstream>

namespace agent::net {

class SseClient::Impl {
 public:
  explicit Impl(asio::io_context &io_ctx) : io_ctx_(io_ctx), ssl_ctx_(asio::ssl::context::tlsv12_client), resolver_(io_ctx) {
    ssl_ctx_.set_default_verify_paths();
  }

  void connect(const std::string &url, const std::map<std::string, std::string> &headers, std::function<void(const SseEvent &)> on_event,
               std::function<void(const std::string &)> on_error, std::function<void()> on_complete) {
    on_event_ = std::move(on_event);
    on_error_ = std::move(on_error);
    on_complete_ = std::move(on_complete);

    // Parse URL
    std::regex url_regex(R"(^(https?):\/\/([^:\/\s]+)(?::(\d+))?(\/[^\?\s]*)?(\?[^\s]*)?)");
    std::smatch match;

    if (!std::regex_match(url, match, url_regex)) {
      if (on_error_) on_error_("Invalid URL");
      return;
    }

    bool is_https = match[1].str() == "https";
    std::string host = match[2].str();
    std::string port = match[3].str().empty() ? (is_https ? "443" : "80") : match[3].str();
    std::string path = match[4].str().empty() ? "/" : match[4].str();
    std::string query = match[5].str();

    // Build request
    std::ostringstream req;
    req << "GET " << path << query << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "Accept: text/event-stream\r\n";
    req << "Cache-Control: no-cache\r\n";
    req << "Connection: keep-alive\r\n";

    for (const auto &[key, value] : headers) {
      req << key << ": " << value << "\r\n";
    }

    req << "\r\n";
    request_ = req.str();

    if (is_https) {
      connect_https(host, port);
    } else {
      connect_http(host, port);
    }
  }

  void stop() {
    stopped_ = true;
    if (ssl_socket_) {
      asio::error_code ec;
      ssl_socket_->lowest_layer().cancel(ec);
      ssl_socket_->lowest_layer().close(ec);
    }
    if (tcp_socket_) {
      asio::error_code ec;
      tcp_socket_->cancel(ec);
      tcp_socket_->close(ec);
    }
  }

  bool is_connected() const {
    return connected_ && !stopped_;
  }

 private:
  void connect_https(const std::string &host, const std::string &port) {
    ssl_socket_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(io_ctx_, ssl_ctx_);

    SSL_set_tlsext_host_name(ssl_socket_->native_handle(), host.c_str());

    resolver_.async_resolve(host, port, [this](const asio::error_code &ec, auto results) {
      if (ec || stopped_) {
        if (on_error_) on_error_("DNS resolution failed: " + ec.message());
        return;
      }

      asio::async_connect(ssl_socket_->lowest_layer(), results, [this](const asio::error_code &ec, auto) {
        if (ec || stopped_) {
          if (on_error_) on_error_("Connection failed: " + ec.message());
          return;
        }

        ssl_socket_->async_handshake(asio::ssl::stream_base::client, [this](const asio::error_code &ec) {
          if (ec || stopped_) {
            if (on_error_) on_error_("SSL handshake failed: " + ec.message());
            return;
          }

          connected_ = true;
          send_request_ssl();
        });
      });
    });
  }

  void connect_http(const std::string &host, const std::string &port) {
    tcp_socket_ = std::make_unique<asio::ip::tcp::socket>(io_ctx_);

    resolver_.async_resolve(host, port, [this](const asio::error_code &ec, auto results) {
      if (ec || stopped_) {
        if (on_error_) on_error_("DNS resolution failed: " + ec.message());
        return;
      }

      asio::async_connect(*tcp_socket_, results, [this](const asio::error_code &ec, auto) {
        if (ec || stopped_) {
          if (on_error_) on_error_("Connection failed: " + ec.message());
          return;
        }

        connected_ = true;
        send_request_tcp();
      });
    });
  }

  void send_request_ssl() {
    asio::async_write(*ssl_socket_, asio::buffer(request_), [this](const asio::error_code &ec, size_t) {
      if (ec || stopped_) {
        if (on_error_) on_error_("Write failed: " + ec.message());
        return;
      }
      read_headers_ssl();
    });
  }

  void send_request_tcp() {
    asio::async_write(*tcp_socket_, asio::buffer(request_), [this](const asio::error_code &ec, size_t) {
      if (ec || stopped_) {
        if (on_error_) on_error_("Write failed: " + ec.message());
        return;
      }
      read_headers_tcp();
    });
  }

  void read_headers_ssl() {
    asio::async_read_until(*ssl_socket_, buffer_, "\r\n\r\n", [this](const asio::error_code &ec, size_t) {
      if (ec || stopped_) {
        if (on_error_) on_error_("Read headers failed: " + ec.message());
        return;
      }
      process_headers();
      read_events_ssl();
    });
  }

  void read_headers_tcp() {
    asio::async_read_until(*tcp_socket_, buffer_, "\r\n\r\n", [this](const asio::error_code &ec, size_t) {
      if (ec || stopped_) {
        if (on_error_) on_error_("Read headers failed: " + ec.message());
        return;
      }
      process_headers();
      read_events_tcp();
    });
  }

  void process_headers() {
    std::istream stream(&buffer_);
    std::string line;
    while (std::getline(stream, line) && line != "\r") {
      // Skip headers for now
    }
  }

  void read_events_ssl() {
    asio::async_read_until(*ssl_socket_, buffer_, "\n\n", [this](const asio::error_code &ec, size_t bytes) {
      if (stopped_) return;

      if (ec == asio::error::eof) {
        if (on_complete_) on_complete_();
        return;
      }

      if (ec) {
        if (on_error_) on_error_("Read failed: " + ec.message());
        return;
      }

      process_event();
      read_events_ssl();
    });
  }

  void read_events_tcp() {
    asio::async_read_until(*tcp_socket_, buffer_, "\n\n", [this](const asio::error_code &ec, size_t bytes) {
      if (stopped_) return;

      if (ec == asio::error::eof) {
        if (on_complete_) on_complete_();
        return;
      }

      if (ec) {
        if (on_error_) on_error_("Read failed: " + ec.message());
        return;
      }

      process_event();
      read_events_tcp();
    });
  }

  void process_event() {
    std::istream stream(&buffer_);
    SseEvent event;
    std::string line;
    std::ostringstream data;

    while (std::getline(stream, line)) {
      if (line.empty() || line == "\r") {
        // End of event
        if (!data.str().empty()) {
          event.data = data.str();
          // Remove trailing newline
          if (!event.data.empty() && event.data.back() == '\n') {
            event.data.pop_back();
          }
          if (on_event_) on_event_(event);
        }
        break;
      }

      // Remove carriage return if present
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.starts_with("event:")) {
        event.event = line.substr(6);
        // Trim leading space
        if (!event.event.empty() && event.event[0] == ' ') {
          event.event = event.event.substr(1);
        }
      } else if (line.starts_with("data:")) {
        std::string d = line.substr(5);
        if (!d.empty() && d[0] == ' ') {
          d = d.substr(1);
        }
        data << d << "\n";
      } else if (line.starts_with("id:")) {
        event.id = line.substr(3);
        if (!event.id.empty() && event.id[0] == ' ') {
          event.id = event.id.substr(1);
        }
      }
    }
  }

  asio::io_context &io_ctx_;
  asio::ssl::context ssl_ctx_;
  asio::ip::tcp::resolver resolver_;

  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket>> ssl_socket_;
  std::unique_ptr<asio::ip::tcp::socket> tcp_socket_;

  asio::streambuf buffer_;
  std::string request_;

  std::function<void(const SseEvent &)> on_event_;
  std::function<void(const std::string &)> on_error_;
  std::function<void()> on_complete_;

  std::atomic<bool> connected_{false};
  std::atomic<bool> stopped_{false};
};

SseClient::SseClient(asio::io_context &io_ctx) : impl_(std::make_unique<Impl>(io_ctx)) {}

SseClient::~SseClient() = default;

void SseClient::connect(const std::string &url, const std::map<std::string, std::string> &headers, std::function<void(const SseEvent &)> on_event,
                        std::function<void(const std::string &)> on_error, std::function<void()> on_complete) {
  impl_->connect(url, headers, std::move(on_event), std::move(on_error), std::move(on_complete));
}

void SseClient::stop() {
  stopped_ = true;
  impl_->stop();
}

bool SseClient::is_connected() const {
  return impl_->is_connected();
}

}  // namespace agent::net
