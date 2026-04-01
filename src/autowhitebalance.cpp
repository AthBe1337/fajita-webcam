#include "autowhitebalance.h"
#include <algorithm>
#include <cmath>

AutoWhiteBalance::AutoWhiteBalance() {}

void AutoWhiteBalance::set_gains(float r, float b) {
    r_gain_ = std::clamp(r, 0.5f, 4.0f);
    b_gain_ = std::clamp(b, 0.5f, 4.0f);
}

void AutoWhiteBalance::reset() {
    r_gain_ = 1.0f;
    b_gain_ = 1.0f;
}

void AutoWhiteBalance::analyze(const uint8_t* bayer, int width, int height, BayerPattern pattern) {
    if (!auto_mode_) return;

    // Use center region for statistics
    int roi_w = std::min(480, width);
    int roi_h = std::min(360, height);
    int roi_x = (width - roi_w) / 2;
    int roi_y = (height - roi_h) / 2;

    auto stats = isp_compute_stats(bayer, width, height,
                                   roi_x, roi_y, roi_w, roi_h, pattern);

    // Skip if too dark
    if (stats.g_mean < 10.0f) return;

    float cur_r = r_gain_.load();
    float cur_b = b_gain_.load();

    // The bayer data has been corrected with current gains.
    // If corrected R < G, we need MORE R gain.
    // The correction factor tells us how much to multiply the current gain.

    float r_correction = stats.g_mean / std::max(1.0f, stats.r_mean);
    float b_correction = stats.g_mean / std::max(1.0f, stats.b_mean);

    // Limit correction to prevent oscillation
    r_correction = std::clamp(r_correction, 0.7f, 1.4f);
    b_correction = std::clamp(b_correction, 0.7f, 1.4f);

    // Calculate new gains - NO neutral bias, trust the measurement
    float target_r = cur_r * r_correction;
    float target_b = cur_b * b_correction;

    // Clamp to range
    target_r = std::clamp(target_r, 0.5f, 4.0f);
    target_b = std::clamp(target_b, 0.5f, 4.0f);

    // Faster convergence - 25% of error per frame
    float step_r = (target_r - cur_r) * 0.25f;
    float step_b = (target_b - cur_b) * 0.25f;

    // Limit step size
    step_r = std::clamp(step_r, -0.1f, 0.1f);
    step_b = std::clamp(step_b, -0.1f, 0.1f);

    r_gain_ = std::clamp(cur_r + step_r, 0.5f, 4.0f);
    b_gain_ = std::clamp(cur_b + step_b, 0.5f, 4.0f);
}