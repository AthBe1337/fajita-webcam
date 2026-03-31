#include "stream.h"
#include "http.h"
#include <cstdio>
#include <condition_variable>

MjpegStream::MjpegStream() {}
MjpegStream::~MjpegStream() { stop(); }

void MjpegStream::stop() {
    running_ = false;
    wait_cv_.notify_all();
}

void MjpegStream::push_frame(const uint8_t* jpeg_data, size_t jpeg_size) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_frame_.assign(jpeg_data, jpeg_data + jpeg_size);
    }
    frame_seq_++;
    wait_cv_.notify_all();
}

MjpegStream::JpegFrame MjpegStream::get_latest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {current_frame_, frame_seq_.load()};
}

bool MjpegStream::serve_client(int fd) {
    // Send MJPEG multipart header
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (!HttpServer::send_string(fd, header))
        return false;

    uint64_t last_seq = 0;

    while (running_) {
        // Wait for new frame
        {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            wait_cv_.wait(lock, [&]() {
                return !running_ || frame_seq_.load() > last_seq;
            });
        }

        if (!running_)
            return false;

        // Get current frame
        std::vector<uint8_t> frame;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_frame_.empty()) continue;
            frame = current_frame_;
        }
        last_seq = frame_seq_.load();

        // Send multipart frame
        char part_header[128];
        int hlen = snprintf(part_header, sizeof(part_header),
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", frame.size());

        if (!HttpServer::send_raw(fd, part_header, hlen))
            return false;
        if (!HttpServer::send_raw(fd, frame.data(), frame.size()))
            return false;
        if (!HttpServer::send_string(fd, "\r\n"))
            return false;
    }

    return false;
}
