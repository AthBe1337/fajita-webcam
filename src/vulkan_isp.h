#pragma once
#include "camera.h"
#include <cstdint>

// GPU-accelerated ISP pipeline using Vulkan compute shaders.
// Targets the Adreno 630 / turnip driver path on aarch64 Linux.
//
// Performs MIPI 10-bit unpack + WB gains + downsample + bilinear demosaic
// entirely on GPU. init() returns false if no compute-capable Vulkan
// device is available; pipeline.cpp transparently falls back to CPU.
class VulkanIsp {
public:
    VulkanIsp();
    ~VulkanIsp();

    bool init();
    void shutdown();
    bool is_available() const { return available_; }

    // Configure for a specific raw resolution + downsample. Re-records the
    // command buffer, so do not call from the hot loop.
    bool configure(int raw_width, int raw_height, int stride,
                   int downsample, BayerPattern pattern);

    // Process one frame. raw_data → rgb_out (RGB888, out_w*out_h*3 bytes).
    bool process(const uint8_t* raw_data, size_t raw_size,
                 float r_gain, float b_gain,
                 uint8_t* rgb_out);

    int output_width()  const { return out_w_; }
    int output_height() const { return out_h_; }

private:
    struct Impl;
    Impl* d_ = nullptr;

    int  raw_w_ = 0, raw_h_ = 0, stride_ = 0;
    int  out_w_ = 0, out_h_ = 0;
    int  downsample_ = 1;
    BayerPattern pattern_ = BayerPattern::RGGB;
    bool available_ = false;
    bool configured_ = false;
};
