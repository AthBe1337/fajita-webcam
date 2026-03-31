#pragma once
#include "camera.h"
#include "stream.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

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

private:
    void capture_loop();
    void encode_jpeg(const uint8_t* rgb, int width, int height,
                     int quality, std::vector<uint8_t>& out);

    Camera& camera_;
    MjpegStream& stream_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex config_mutex_;
    PipelineConfig config_;

    AnalysisCallback analysis_cb_;
    int analysis_interval_ = 5;

    std::atomic<float> fps_{0};
    std::atomic<size_t> last_jpeg_size_{0};
};
