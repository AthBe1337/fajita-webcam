#pragma once
#include "camera.h"
#include "isp.h"
#include <atomic>

class AutoExposure {
public:
    AutoExposure();

    // Process a frame. Call from analysis callback.
    void analyze(const uint8_t* bayer, int width, int height,
                 BayerPattern pattern, Camera& camera);

    void set_auto(bool on) { auto_mode_ = on; }
    bool is_auto() const { return auto_mode_; }

    void set_target_brightness(float target) { target_brightness_ = target; }
    float target_brightness() const { return target_brightness_; }
    float current_brightness() const { return current_brightness_; }

    void reset();

private:
    float compute_brightness(const uint8_t* bayer, int width, int height, BayerPattern pattern);

    std::atomic<bool> auto_mode_{false};
    std::atomic<float> target_brightness_{80.0f};
    std::atomic<float> current_brightness_{0.0f};

    float integral_ = 0.0f;
    float prev_error_ = 0.0f;
    int stable_count_ = 0;
};