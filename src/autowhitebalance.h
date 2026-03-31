#pragma once
#include "isp.h"
#include <atomic>
#include <mutex>
#include <algorithm>

class AutoWhiteBalance {
public:
    AutoWhiteBalance();

    // Process a frame's bayer data. Call from analysis callback.
    void analyze(const uint8_t* bayer, int width, int height, BayerPattern pattern);

    // Get current WB gains
    float r_gain() const { return r_gain_; }
    float b_gain() const { return b_gain_; }

    void set_auto(bool on) { auto_mode_ = on; }
    bool is_auto() const { return auto_mode_; }

    // Manual override
    void set_gains(float r, float b) {
        r_gain_ = std::clamp(r, 0.5f, 4.0f);
        b_gain_ = std::clamp(b, 0.5f, 4.0f);
    }

private:
    std::atomic<float> r_gain_{1.0f};
    std::atomic<float> b_gain_{1.0f};
    std::atomic<bool> auto_mode_{true};
};
