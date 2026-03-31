#include "autoexposure.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

AutoExposure::AutoExposure() {}

void AutoExposure::analyze(const uint8_t* bayer, int width, int height,
                            BayerPattern pattern, Camera& camera) {
    // Compute brightness from center ROI
    int roi_w = std::min(512, width);
    int roi_h = std::min(512, height);
    int roi_x = (width - roi_w) / 2;
    int roi_y = (height - roi_h) / 2;

    auto stats = isp_compute_stats(bayer, width, height,
                                   roi_x, roi_y, roi_w, roi_h, pattern);
    current_brightness_ = stats.brightness;

    if (!auto_mode_) return;

    auto* cfg = camera.active_config();
    if (!cfg) return;

    float target = target_brightness_.load();
    float actual = stats.brightness;
    if (actual < 1.0f) actual = 1.0f;

    float ratio = target / actual;
    // Clamp ratio to avoid wild swings
    ratio = std::clamp(ratio, 0.5f, 2.0f);

    // EMA smoothing
    float alpha = 0.3f;
    static float smoothed_ratio = 1.0f;
    smoothed_ratio = smoothed_ratio * (1.0f - alpha) + ratio * alpha;

    if (std::abs(smoothed_ratio - 1.0f) < 0.05f) return;  // close enough

    // Adjust exposure first, then gain
    int cur_exp = camera.get_exposure();
    if (cur_exp < 0) return;

    int new_exp = (int)(cur_exp * smoothed_ratio);
    new_exp = std::clamp(new_exp, cfg->exposure.min, cfg->exposure.max);

    if (new_exp != cur_exp) {
        camera.set_exposure(new_exp);
        smoothed_ratio = 1.0f;
        return;
    }

    // Exposure is at limit, adjust analogue gain
    int cur_gain = camera.get_analogue_gain();
    if (cur_gain < 0) return;

    // For gain, be more conservative
    int new_gain;
    if (smoothed_ratio > 1.0f)
        new_gain = cur_gain + std::max(1, (int)(cur_gain * 0.1f));
    else
        new_gain = cur_gain - std::max(1, (int)(cur_gain * 0.1f));

    new_gain = std::clamp(new_gain, cfg->analogue_gain.min + 1, cfg->analogue_gain.max);
    if (new_gain != cur_gain)
        camera.set_analogue_gain(new_gain);

    smoothed_ratio = 1.0f;
}
