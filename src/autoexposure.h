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

private:
    std::atomic<bool> auto_mode_{false};
    std::atomic<float> target_brightness_{50.0f};  // target green mean (0-255)
    std::atomic<float> current_brightness_{0.0f};
};
