#pragma once
#include <cstdint>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

struct HttpRequest {
    std::string method;  // GET, POST
    std::string path;    // /api/cameras
    std::string query;   // after ?
    std::string body;
    std::unordered_map<std::string, std::string> headers;

    std::string header(const std::string& key) const {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : "";
    }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra_headers;

    static HttpResponse json(const std::string& body) {
        return {200, "application/json", body, {}};
    }
    static HttpResponse text(const std::string& body) {
        return {200, "text/plain", body, {}};
    }
    static HttpResponse error(int status, const std::string& msg) {
        return {status, "text/plain", msg, {}};
    }
};

// Callback for streaming responses (MJPEG)
// Return false to stop streaming to this client
using StreamWriter = std::function<bool(int fd)>;

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;
using StreamHandler = std::function<void(int fd, const HttpRequest&)>;

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    void route(const std::string& method, const std::string& path, RouteHandler handler);
    void stream_route(const std::string& path, StreamHandler handler);

    // Serve static files from a directory
    void set_static_dir(const std::string& dir) { static_dir_ = dir; }

    bool start(int port);
    void stop();

    // Send raw data to a streaming client fd
    // Returns false if client disconnected
    static bool send_raw(int fd, const void* data, size_t len);
    static bool send_string(int fd, const std::string& s);

private:
    void accept_loop();
    void handle_client(int fd);
    bool parse_request(int fd, HttpRequest& req);
    void send_response(int fd, const HttpResponse& resp);
    bool serve_embedded_static(int fd, const std::string& path);
    bool serve_static(int fd, const std::string& path);
    std::string guess_content_type(const std::string& path);
    void register_client_fd(int fd);
    void unregister_client_fd(int fd);
    void join_client_threads();

    struct Route {
        std::string method;
        std::string path;
        RouteHandler handler;
    };
    struct StreamRoute {
        std::string path;
        StreamHandler handler;
    };

    std::vector<Route> routes_;
    std::vector<StreamRoute> stream_routes_;
    std::string static_dir_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    int epoll_fd_ = -1;
    std::mutex client_mutex_;
    std::vector<std::thread> client_threads_;
    std::unordered_set<int> client_fds_;
};
