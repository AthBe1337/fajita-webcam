#include "autowhitebalance.h"
#include <algorithm>
#include <cstdio>

AutoWhiteBalance::AutoWhiteBalance() {}

void AutoWhiteBalance::analyze(const uint8_t* bayer, int width, int height,
                                BayerPattern pattern) {
    if (!auto_mode_) return;

    // Use center ROI for statistics
    int roi_w = std::min(512, width);
    int roi_h = std::min(512, height);
    int roi_x = (width - roi_w) / 2;
    int roi_y = (height - roi_h) / 2;

    auto stats = isp_compute_stats(bayer, width, height,
                                   roi_x, roi_y, roi_w, roi_h, pattern);

    if (stats.r_mean < 1.0f || stats.b_mean < 1.0f) return;

    // Grey World: make R and B averages match G average
    float new_r = stats.g_mean / stats.r_mean;
    float new_b = stats.g_mean / stats.b_mean;

    new_r = std::clamp(new_r, 0.5f, 4.0f);
    new_b = std::clamp(new_b, 0.5f, 4.0f);

    // EMA smoothing
    float alpha = 0.2f;
    float cur_r = r_gain_.load();
    float cur_b = b_gain_.load();
    r_gain_ = cur_r * (1.0f - alpha) + new_r * alpha;
    b_gain_ = cur_b * (1.0f - alpha) + new_b * alpha;
}
