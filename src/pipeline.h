#pragma once
#include "camera.h"
#include "stream.h"
#include "threadpool.h"
#if HAS_VULKAN_ISP
#include "vulkan_isp.h"
#endif
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <condition_variable>

// Callback for frame analysis (AWB/AE/AF)
struct AnalysisFrame {
    const uint8_t* bayer;  // unpacked 8-bit bayer
    int width, height;
    BayerPattern pattern;
};
using AnalysisCallback = std::function<void(const AnalysisFrame&)>;

struct PipelineConfig {
    int downsample = 4;      // 1, 2, or 4
    int jpeg_quality = 80;   // 1-100
    int target_fps = 15;     // 0 = unlimited
    float r_gain = 1.0f;     // WB red gain
    float b_gain = 1.0f;     // WB blue gain
    int rotation = 0;        // 0, 90, 180, 270 degrees
};

class Pipeline {
public:
    Pipeline(Camera& camera, MjpegStream& stream);
    ~Pipeline();

    bool start();
    void stop();
    bool is_running() const { return running_; }

    void set_config(const PipelineConfig& cfg);
    PipelineConfig get_config() const;

    // Register analysis callback (called every N frames from capture thread)
    void set_analysis_callback(AnalysisCallback cb, int interval = 5);

    // Stats
    float current_fps() const { return fps_; }
    size_t last_jpeg_size() const { return last_jpeg_size_; }

    // Get current output dimensions (after rotation)
    void get_output_size(int& width, int& height) const;

private:
    void capture_loop();
    void analysis_loop();
    void encode_jpeg(const uint8_t* rgb, int width, int height,
                     int quality, std::vector<uint8_t>& out);

    // Rotate RGB image in-place
    void rotate_rgb(std::vector<uint8_t>& rgb, int& width, int& height, int rotation);

    Camera& camera_;
    MjpegStream& stream_;
    std::thread thread_;
    std::thread analysis_thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex config_mutex_;
    PipelineConfig config_;

    AnalysisCallback analysis_cb_;
    int analysis_interval_ = 5;

    // Thread pool for parallel ISP processing (CPU fallback)
    ThreadPool isp_pool_;

#if HAS_VULKAN_ISP
    // Vulkan compute-shader ISP (preferred when available)
    VulkanIsp gpu_isp_;
    bool gpu_configured_ = false;
#endif

    // Async analysis: capture thread copies bayer data here, analysis thread
    // picks it up without blocking capture.
    std::mutex analysis_mutex_;
    std::condition_variable analysis_cv_;
    std::vector<uint8_t> analysis_bayer_buf_;
    int analysis_width_ = 0;
    int analysis_height_ = 0;
    BayerPattern analysis_pattern_ = BayerPattern::RGGB;
    bool analysis_ready_ = false;

    std::atomic<float> fps_{0};
    std::atomic<size_t> last_jpeg_size_{0};

    // Per-stage timing (ms), updated once per second
    std::atomic<float> time_unpack_ms_{0};
    std::atomic<float> time_demosaic_ms_{0};
    std::atomic<float> time_jpeg_ms_{0};

public:
    float time_unpack() const { return time_unpack_ms_; }
    float time_demosaic() const { return time_demosaic_ms_; }
    float time_jpeg() const { return time_jpeg_ms_; }
};