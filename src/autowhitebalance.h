#pragma once
#include "isp.h"
#include <atomic>

class AutoWhiteBalance {
public:
    AutoWhiteBalance();

    // Process a frame's bayer data (already WB-corrected).
    void analyze(const uint8_t* bayer, int width, int height, BayerPattern pattern);

    float r_gain() const { return r_gain_; }
    float b_gain() const { return b_gain_; }

    void set_auto(bool on) { auto_mode_ = on; }
    bool is_auto() const { return auto_mode_; }

    void set_gains(float r, float b);
    void reset();

private:
    std::atomic<float> r_gain_{1.0f};
    std::atomic<float> b_gain_{1.0f};
    std::atomic<bool> auto_mode_{true};
};