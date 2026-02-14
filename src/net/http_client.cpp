#include "agent/net/http_client.hpp"

#include <sstream>
#include <regex>
#include <openssl/err.h>
#include <openssl/ssl.h>

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
    explicit Impl(asio::io_context& io_ctx)
        : io_ctx_(io_ctx)
        , ssl_ctx_(asio::ssl::context::tlsv12_client)
        , resolver_(io_ctx) {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(asio::ssl::verify_peer);
    }
    
    void request(
        const std::string& url,
        const HttpOptions& options,
        std::function<void(HttpResponse)> callback
    ) {
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
    
private:
    void request_https(
        const ParsedUrl& url,
        const HttpOptions& options,
        std::function<void(HttpResponse)> callback
    ) {
        auto socket = std::make_shared<asio::ssl::stream<asio::ip::tcp::socket>>(io_ctx_, ssl_ctx_);
        auto response = std::make_shared<HttpResponse>();
        auto request_str = std::make_shared<std::string>();
        auto buffer = std::make_shared<asio::streambuf>();
        
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
            [this, socket, request_str, response, buffer, callback, url](
                const asio::error_code& ec,
                asio::ip::tcp::resolver::results_type results
            ) {
                if (ec) {
                    response->error = "DNS resolution failed: " + ec.message();
                    callback(*response);
                    return;
                }
                
                asio::async_connect(
                    socket->lowest_layer(), results,
                    [this, socket, request_str, response, buffer, callback](
                        const asio::error_code& ec,
                        const asio::ip::tcp::endpoint&
                    ) {
                        if (ec) {
                            response->error = "Connection failed: " + ec.message();
                            callback(*response);
                            return;
                        }
                        
                        // SSL handshake
                        socket->async_handshake(
                            asio::ssl::stream_base::client,
                            [this, socket, request_str, response, buffer, callback](
                                const asio::error_code& ec
                            ) {
                                if (ec) {
                                    response->error = "SSL handshake failed: " + ec.message();
                                    callback(*response);
                                    return;
                                }
                                
                                // Send request
                                asio::async_write(
                                    *socket,
                                    asio::buffer(*request_str),
                                    [this, socket, response, buffer, callback](
                                        const asio::error_code& ec,
                                        size_t
                                    ) {
                                        if (ec) {
                                            response->error = "Write failed: " + ec.message();
                                            callback(*response);
                                            return;
                                        }
                                        
                                        // Read response
                                        read_response(socket, response, buffer, callback);
                                    }
                                );
                            }
                        );
                    }
                );
            }
        );
    }
    
    void request_http(
        const ParsedUrl& url,
        const HttpOptions& options,
        std::function<void(HttpResponse)> callback
    ) {
        auto socket = std::make_shared<asio::ip::tcp::socket>(io_ctx_);
        auto response = std::make_shared<HttpResponse>();
        auto request_str = std::make_shared<std::string>();
        auto buffer = std::make_shared<asio::streambuf>();
        
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
            [this, socket, request_str, response, buffer, callback](
                const asio::error_code& ec,
                asio::ip::tcp::resolver::results_type results
            ) {
                if (ec) {
                    response->error = "DNS resolution failed: " + ec.message();
                    callback(*response);
                    return;
                }
                
                asio::async_connect(
                    *socket, results,
                    [this, socket, request_str, response, buffer, callback](
                        const asio::error_code& ec,
                        const asio::ip::tcp::endpoint&
                    ) {
                        if (ec) {
                            response->error = "Connection failed: " + ec.message();
                            callback(*response);
                            return;
                        }
                        
                        asio::async_write(
                            *socket,
                            asio::buffer(*request_str),
                            [this, socket, response, buffer, callback](
                                const asio::error_code& ec,
                                size_t
                            ) {
                                if (ec) {
                                    response->error = "Write failed: " + ec.message();
                                    callback(*response);
                                    return;
                                }
                                
                                read_response(socket, response, buffer, callback);
                            }
                        );
                    }
                );
            }
        );
    }
    
    template<typename Socket>
    void read_response(
        std::shared_ptr<Socket> socket,
        std::shared_ptr<HttpResponse> response,
        std::shared_ptr<asio::streambuf> buffer,
        std::function<void(HttpResponse)> callback
    ) {
        asio::async_read_until(
            *socket, *buffer, "\r\n\r\n",
            [this, socket, response, buffer, callback](
                const asio::error_code& ec,
                size_t bytes_transferred
            ) {
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
                    response->status_code = std::stoi(match[1].str());
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
            }
        );
    }
    
    template<typename Socket>
    void read_body(
        std::shared_ptr<Socket> socket,
        std::shared_ptr<HttpResponse> response,
        std::shared_ptr<asio::streambuf> buffer,
        std::function<void(HttpResponse)> callback
    ) {
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
            size_t content_length = std::stoull(it->second);
            if (response->body.size() >= content_length) {
                // We have all the data
                callback(*response);
                return;
            }
        }
        
        // Continue reading until EOF or we have all data
        asio::async_read(
            *socket, *buffer, asio::transfer_at_least(1),
            [this, socket, response, buffer, callback](
                const asio::error_code& ec,
                size_t bytes_transferred
            ) {
                // SSL connections may return various errors on close
                // Treat any SSL category error as potential EOF
                bool is_eof = (ec == asio::error::eof) || 
                             (ec.category() == asio::error::get_ssl_category());
                
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
                        size_t content_length = std::stoull(it->second);
                        if (response->body.size() >= content_length) {
                            callback(*response);
                            return;
                        }
                    }
                    read_body(socket, response, buffer, callback);
                }
            }
        );
    }
    
    asio::io_context& io_ctx_;
    asio::ssl::context ssl_ctx_;
    asio::ip::tcp::resolver resolver_;
};

HttpClient::HttpClient(asio::io_context& io_ctx)
    : impl_(std::make_unique<Impl>(io_ctx)) {}

HttpClient::~HttpClient() = default;

void HttpClient::request(
    const std::string& url,
    const HttpOptions& options,
    std::function<void(HttpResponse)> callback
) {
    impl_->request(url, options, std::move(callback));
}

std::future<HttpResponse> HttpClient::request(
    const std::string& url,
    const HttpOptions& options
) {
    auto promise = std::make_shared<std::promise<HttpResponse>>();
    auto future = promise->get_future();
    
    impl_->request(url, options, [promise](HttpResponse response) {
        promise->set_value(std::move(response));
    });
    
    return future;
}

std::future<HttpResponse> HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers
) {
    HttpOptions options;
    options.method = "GET";
    options.headers = headers;
    return request(url, options);
}

std::future<HttpResponse> HttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& headers
) {
    HttpOptions options;
    options.method = "POST";
    options.body = body;
    options.headers = headers;
    return request(url, options);
}

}  // namespace agent::net
