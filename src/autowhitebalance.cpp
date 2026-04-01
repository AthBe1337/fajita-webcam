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

    int roi_w = std::min(480, width);
    int roi_h = std::min(360, height);
    int roi_x = (width - roi_w) / 2;
    int roi_y = (height - roi_h) / 2;

    auto stats = isp_compute_stats(bayer, width, height,
                                   roi_x, roi_y, roi_w, roi_h, pattern);

    if (stats.g_mean < 10.0f) return;

    float cur_r = r_gain_.load();
    float cur_b = b_gain_.load();

    // WB-corrected data: if r_mean < g_mean, need more r_gain
    float ratio_r = stats.g_mean / std::max(1.0f, stats.r_mean);
    float ratio_b = stats.g_mean / std::max(1.0f, stats.b_mean);

    ratio_r = std::clamp(ratio_r, 0.7f, 1.5f);
    ratio_b = std::clamp(ratio_b, 0.7f, 1.5f);

    float target_r = cur_r * ratio_r;
    float target_b = cur_b * ratio_b;

    // Estimate color temperature from R/B ratio of the scene
    // Low R/B ratio (R < B) = cool scene (high color temp) = need more R gain
    // High R/B ratio (R > B) = warm scene (low color temp) = need less R gain
    float r_b_ratio = stats.r_mean / std::max(1.0f, stats.b_mean);

    // Dynamic R gain compensation based on color temperature
    // Cool light (R/B < 0.9): boost R gain more (up to 25%)
    // Neutral light (R/B ≈ 1.0): mild boost (~10%)
    // Warm light (R/B > 1.1): no boost (scene already reddish)
    float r_compensation;
    if (r_b_ratio < 0.85f) {
        // Very cool light (fluorescent, overcast) - strong R boost
        r_compensation = 1.25f;
    } else if (r_b_ratio < 0.95f) {
        // Slightly cool - moderate R boost
        r_compensation = 1.15f;
    } else if (r_b_ratio < 1.1f) {
        // Neutral light - mild R boost
        r_compensation = 1.08f;
    } else {
        // Warm light (incandescent, candle) - no boost
        r_compensation = 1.0f;
    }

    target_r *= r_compensation;

    // Clamp
    target_r = std::clamp(target_r, 0.5f, 4.0f);
    target_b = std::clamp(target_b, 0.5f, 4.0f);

    // Converge
    float step_r = (target_r - cur_r) * 0.3f;
    float step_b = (target_b - cur_b) * 0.3f;

    step_r = std::clamp(step_r, -0.15f, 0.15f);
    step_b = std::clamp(step_b, -0.15f, 0.15f);

    r_gain_ = std::clamp(cur_r + step_r, 0.5f, 4.0f);
    b_gain_ = std::clamp(cur_b + step_b, 0.5f, 4.0f);
}