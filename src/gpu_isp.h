#pragma once
#include "camera.h"
#include <cstdint>
#include <vector>

// GPU-accelerated ISP pipeline using OpenGL ES 3.1 compute shaders.
// Performs MIPI 10-bit unpack + WB gains + downsample + demosaic entirely on GPU.
// Falls back gracefully: init() returns false if GPU is unavailable.
class GpuIsp {
public:
    GpuIsp();
    ~GpuIsp();

    // Initialize EGL/GLES context. Returns false if GPU is not available.
    bool init();
    void shutdown();
    bool is_available() const { return available_; }

    // Configure for a specific camera resolution and processing mode.
    // Must be called before process(), and whenever resolution/downsample changes.
    bool configure(int raw_width, int raw_height, int stride,
                   int downsample, BayerPattern pattern);

    // Process a raw MIPI frame → RGB output.
    // raw_data: MIPI 10-bit packed input (stride * raw_height bytes)
    // rgb_out: pre-allocated RGB888 buffer (out_w * out_h * 3 bytes)
    // Returns false on GPU error.
    bool process(const uint8_t* raw_data, size_t raw_size,
                 float r_gain, float b_gain,
                 uint8_t* rgb_out);

    int output_width() const { return out_w_; }
    int output_height() const { return out_h_; }

private:
    bool compile_shader(unsigned int& shader, const char* source);
    bool create_program(unsigned int& program, unsigned int shader);

    // EGL handles (stored as void* to avoid header pollution)
    void* egl_display_ = nullptr;
    void* egl_context_ = nullptr;
    void* gbm_device_ = nullptr;
    int drm_fd_ = -1;

    // Shader programs
    unsigned int unpack_program_ = 0;
    unsigned int demosaic_program_ = 0;

    // Buffers (SSBOs)
    unsigned int raw_ssbo_ = 0;      // input MIPI data
    unsigned int bayer_ssbo_ = 0;    // intermediate 8-bit bayer
    unsigned int rgb_ssbo_ = 0;      // output RGB
    unsigned int read_buf_ = 0;      // CPU readback buffer

    // Configuration
    int raw_w_ = 0, raw_h_ = 0, stride_ = 0;
    int out_w_ = 0, out_h_ = 0;
    int downsample_ = 1;
    BayerPattern pattern_ = BayerPattern::RGGB;
    bool available_ = false;
    bool configured_ = false;

    size_t raw_buf_size_ = 0;
    size_t bayer_buf_size_ = 0;
    size_t rgb_buf_size_ = 0;
};
