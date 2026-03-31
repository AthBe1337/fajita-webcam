#pragma once
#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <condition_variable>

// MJPEG stream broadcaster
// One producer pushes JPEG frames, multiple consumers (HTTP clients) receive them
class MjpegStream {
public:
    MjpegStream();
    ~MjpegStream();

    // Producer: push a new JPEG frame (thread-safe)
    void push_frame(const uint8_t* jpeg_data, size_t jpeg_size);

    // Consumer: called from HTTP stream handler
    // Blocks until a new frame is available, then sends it.
    // Returns false if client disconnected.
    bool serve_client(int fd);

    // Get latest frame (for snapshot)
    struct JpegFrame {
        std::vector<uint8_t> data;
        uint64_t seq;
    };
    JpegFrame get_latest();

    uint64_t frame_count() const { return frame_seq_.load(); }

private:
    std::mutex mutex_;
    std::vector<uint8_t> current_frame_;
    std::atomic<uint64_t> frame_seq_{0};

    // Simple broadcast: condition variable for waiting consumers
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};
