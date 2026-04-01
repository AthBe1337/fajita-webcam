#include "http.h"
#include "embedded_assets.h"
#include "logging.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <thread>
#include <algorithm>
#include <fstream>
#include <sstream>

HttpServer::HttpServer() {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& path,
                       RouteHandler handler) {
    routes_.push_back({method, path, std::move(handler)});
}

void HttpServer::stream_route(const std::string& path, StreamHandler handler) {
    stream_routes_.push_back({path, std::move(handler)});
}

bool HttpServer::start(int port) {
    listen_fd_ = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        // Fallback to IPv4
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) { perror("socket"); return false; }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind"); ::close(listen_fd_); listen_fd_ = -1; return false;
        }
    } else {
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int no = 0;
        setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port);
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind"); ::close(listen_fd_); listen_fd_ = -1; return false;
        }
    }

    if (listen(listen_fd_, 16) < 0) {
        perror("listen"); ::close(listen_fd_); listen_fd_ = -1; return false;
    }

    running_ = true;
    LOG_INFO("HTTP server listening on port %d", port);

    // Accept loop in current thread (blocking)
    accept_loop();
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    std::vector<int> clients;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        clients.assign(client_fds_.begin(), client_fds_.end());
    }
    for (int fd : clients)
        ::shutdown(fd, SHUT_RDWR);

    join_client_threads();
}

void HttpServer::accept_loop() {
    while (running_) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        int client = accept(listen_fd_, (sockaddr*)&addr, &len);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (!running_) break;
            perror("accept");
            continue;
        }

        if (!running_) {
            ::close(client);
            break;
        }

        // Get client IP address
        char client_ip[INET6_ADDRSTRLEN] = "unknown";
        if (addr.ss_family == AF_INET) {
            struct sockaddr_in* s = (struct sockaddr_in*)&addr;
            inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6* s = (struct sockaddr_in6*)&addr;
            inet_ntop(AF_INET6, &s->sin6_addr, client_ip, sizeof(client_ip));
        }

        // Handle each client in a worker thread; track it for clean shutdown.
        std::lock_guard<std::mutex> lock(client_mutex_);
        client_threads_.emplace_back([this, client, client_ip]() {
            handle_client(client, client_ip);
        });
    }

    join_client_threads();
}

void HttpServer::handle_client(int fd, const std::string& client_ip) {
    register_client_fd(fd);
    auto cleanup = [&]() {
        unregister_client_fd(fd);
        ::close(fd);
    };

    HttpRequest req;
    if (!parse_request(fd, req)) {
        cleanup();
        return;
    }

    int response_status = 200;

    // Check stream routes first
    for (auto& sr : stream_routes_) {
        if (req.path == sr.path && req.method == "GET") {
            LOG_INFO("HTTP %s %s %s %d", client_ip.c_str(), req.method.c_str(), req.path.c_str(), 200);
            sr.handler(fd, req);
            cleanup();
            return;
        }
    }

    // Check API routes
    for (auto& r : routes_) {
        if (r.method == req.method && r.path == req.path) {
            auto resp = r.handler(req);
            response_status = resp.status;
            send_response(fd, resp);
            LOG_INFO("HTTP %s %s %s %d", client_ip.c_str(), req.method.c_str(), req.path.c_str(), response_status);
            cleanup();
            return;
        }
    }

    // Try embedded static files first, then optional on-disk fallback.
    if (req.method == "GET") {
        std::string file_path = req.path;
        if (file_path == "/") file_path = "/index.html";
        if (serve_embedded_static(fd, file_path) ||
            (!static_dir_.empty() && serve_static(fd, file_path))) {
            LOG_INFO("HTTP %s %s %s %d", client_ip.c_str(), req.method.c_str(), req.path.c_str(), 200);
            cleanup();
            return;
        }
    }

    // 404
    response_status = 404;
    send_response(fd, HttpResponse::error(404, "Not Found"));
    LOG_INFO("HTTP %s %s %s %d", client_ip.c_str(), req.method.c_str(), req.path.c_str(), response_status);
    cleanup();
}

void HttpServer::register_client_fd(int fd) {
    std::lock_guard<std::mutex> lock(client_mutex_);
    client_fds_.insert(fd);
}

void HttpServer::unregister_client_fd(int fd) {
    std::lock_guard<std::mutex> lock(client_mutex_);
    client_fds_.erase(fd);
}

void HttpServer::join_client_threads() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        threads.swap(client_threads_);
    }

    for (auto& t : threads) {
        if (t.joinable())
            t.join();
    }
}

bool HttpServer::parse_request(int fd, HttpRequest& req) {
    // Read request header (up to 8KB)
    char buf[8192];
    int total = 0;
    int header_end = -1;

    while (total < (int)sizeof(buf) - 1) {
        int n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) return false;
        total += n;
        buf[total] = '\0';

        // Look for end of headers
        char* hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            header_end = hdr_end - buf + 4;
            break;
        }
    }
    if (header_end < 0) return false;

    // Parse request line
    char* line_end = strstr(buf, "\r\n");
    if (!line_end) return false;
    *line_end = '\0';

    char method[16], path[4096], version[16];
    if (sscanf(buf, "%15s %4095s %15s", method, path, version) < 2)
        return false;

    req.method = method;
    // Split path and query
    char* q = strchr(path, '?');
    if (q) {
        *q = '\0';
        req.query = q + 1;
    }
    req.path = path;

    // Parse headers
    char* pos = line_end + 2;
    while (pos < buf + header_end - 2) {
        char* next = strstr(pos, "\r\n");
        if (!next) break;
        *next = '\0';
        char* colon = strchr(pos, ':');
        if (colon) {
            *colon = '\0';
            std::string key = pos;
            std::string val = colon + 1;
            // Trim leading space from value
            if (!val.empty() && val[0] == ' ') val = val.substr(1);
            // Lowercase key
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = val;
        }
        pos = next + 2;
    }

    // Read body if Content-Length present
    auto cl_it = req.headers.find("content-length");
    if (cl_it != req.headers.end()) {
        int content_len = std::stoi(cl_it->second);
        int body_already = total - header_end;
        req.body.assign(buf + header_end, body_already);

        while ((int)req.body.size() < content_len) {
            int n = recv(fd, buf, std::min((int)sizeof(buf), content_len - (int)req.body.size()), 0);
            if (n <= 0) break;
            req.body.append(buf, n);
        }
    }

    return true;
}

void HttpServer::send_response(int fd, const HttpResponse& resp) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status << " ";
    switch (resp.status) {
    case 200: ss << "OK"; break;
    case 400: ss << "Bad Request"; break;
    case 404: ss << "Not Found"; break;
    case 500: ss << "Internal Server Error"; break;
    default: ss << "Unknown"; break;
    }
    ss << "\r\n";
    ss << "Content-Type: " << resp.content_type << "\r\n";
    ss << "Content-Length: " << resp.body.size() << "\r\n";
    ss << "Access-Control-Allow-Origin: *\r\n";
    for (auto& [k, v] : resp.extra_headers)
        ss << k << ": " << v << "\r\n";
    ss << "Connection: close\r\n";
    ss << "\r\n";
    ss << resp.body;

    std::string data = ss.str();
    send_raw(fd, data.data(), data.size());
}

bool HttpServer::serve_embedded_static(int fd, const std::string& path) {
    if (path.find("..") != std::string::npos) return false;

    const auto& assets = embedded_assets();
    auto it = std::find_if(assets.begin(), assets.end(), [&](const EmbeddedAsset& asset) {
        return asset.path == path;
    });
    if (it == assets.end()) return false;

    HttpResponse resp;
    resp.status = 200;
    resp.content_type = guess_content_type(path);
    resp.body.assign(reinterpret_cast<const char*>(it->data), it->size);
    send_response(fd, resp);
    return true;
}

bool HttpServer::serve_static(int fd, const std::string& path) {
    // Prevent directory traversal
    if (path.find("..") != std::string::npos) return false;

    std::string full = static_dir_ + path;
    struct stat st;
    if (stat(full.c_str(), &st) < 0 || !S_ISREG(st.st_mode))
        return false;

    std::ifstream f(full, std::ios::binary);
    if (!f) return false;

    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    HttpResponse resp;
    resp.status = 200;
    resp.content_type = guess_content_type(path);
    resp.body = std::move(content);
    send_response(fd, resp);
    return true;
}

std::string HttpServer::guess_content_type(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js")) return "application/javascript; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

bool HttpServer::send_raw(int fd, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += n;
    }
    return true;
}

bool HttpServer::send_string(int fd, const std::string& s) {
    return send_raw(fd, s.data(), s.size());
}
