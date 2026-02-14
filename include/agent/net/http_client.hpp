#pragma once

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <asio.hpp>
#include <asio/ssl.hpp>

namespace agent::net {

// HTTP response
struct HttpResponse {
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error;
    
    bool ok() const { return status_code >= 200 && status_code < 300; }
};

// HTTP request options
struct HttpOptions {
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::seconds timeout{30};
};

// Async HTTP client using ASIO
class HttpClient {
public:
    explicit HttpClient(asio::io_context& io_ctx);
    ~HttpClient();
    
    // Async request with callback
    void request(
        const std::string& url,
        const HttpOptions& options,
        std::function<void(HttpResponse)> callback
    );
    
    // Async request returning future
    std::future<HttpResponse> request(
        const std::string& url,
        const HttpOptions& options
    );
    
    // Convenience methods
    std::future<HttpResponse> get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    );
    
    std::future<HttpResponse> post(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers = {}
    );

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
    
    bool is_https() const { return scheme == "https"; }
    std::string port_or_default() const;
    
    static std::optional<ParsedUrl> parse(const std::string& url);
};

}  // namespace agent::net
