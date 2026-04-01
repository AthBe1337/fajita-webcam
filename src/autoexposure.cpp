#include "autoexposure.h"
#include <algorithm>
#include <cmath>

AutoExposure::AutoExposure() {}

void AutoExposure::reset() {
}

float AutoExposure::compute_brightness(const uint8_t* bayer, int width, int height,
                                        BayerPattern pattern) {
    int roi_w = std::min(320, width);
    int roi_h = std::min(240, height);
    int roi_x = (width - roi_w) / 2;
    int roi_y = (height - roi_h) / 2;

    auto stats = isp_compute_stats(bayer, width, height,
                                   roi_x, roi_y, roi_w, roi_h, pattern);
    return stats.brightness;
}

void AutoExposure::analyze(const uint8_t* bayer, int width, int height,
                           BayerPattern pattern, Camera& camera) {
    float brightness = compute_brightness(bayer, width, height, pattern);
    current_brightness_ = brightness;

    if (!auto_mode_) return;

    auto* cfg = camera.active_config();
    if (!cfg) return;

    int cur_exp = camera.get_exposure();
    int cur_analogue = camera.get_analogue_gain();
    int cur_digital = camera.get_digital_gain();

    if (cur_exp < 0 || cur_analogue < 0 || cur_digital < 0) return;

    float target = target_brightness_.load();

    // === Emergency recovery for dark frames ===
    if (brightness < 3.0f) {
        // Set reasonable minimums
        int safe_exp = std::max(cfg->exposure.def, 500);
        int safe_analogue = std::max(200, cfg->analogue_gain.min + 100);
        int safe_digital = std::max(1024, cfg->digital_gain.min + 512);

        if (cur_exp < safe_exp) {
            camera.set_exposure(safe_exp);
            return;
        }
        if (cur_analogue < safe_analogue) {
            camera.set_analogue_gain(safe_analogue);
            return;
        }
        if (cur_digital < safe_digital) {
            camera.set_digital_gain(safe_digital);
            return;
        }
        // Keep increasing if still dark
        if (cur_exp < cfg->exposure.max) {
            camera.set_exposure(std::min(cur_exp + 500, cfg->exposure.max));
        } else if (cur_analogue < cfg->analogue_gain.max) {
            camera.set_analogue_gain(std::min(cur_analogue + 100, cfg->analogue_gain.max));
        } else if (cur_digital < cfg->digital_gain.max) {
            camera.set_digital_gain(std::min(cur_digital + 512, cfg->digital_gain.max));
        }
        return;
    }

    // === Normal adjustment ===
    float error = (target - brightness) / target;

    // Dead zone - small errors ignored
    if (std::abs(error) < 0.05f) return;

    // Calculate step size proportional to error magnitude
    float strength = std::min(std::abs(error), 0.5f);  // Cap at 50% error

    if (error > 0) {
        // Too dark - increase exposure/gain
        // Priority: exposure first (better quality), then analogue gain, then digital

        if (cur_exp < cfg->exposure.max) {
            // Increase exposure proportional to error
            int step = std::max(50, (int)(cur_exp * strength * 0.5f));
            camera.set_exposure(std::min(cur_exp + step, cfg->exposure.max));
        } else if (cur_analogue < cfg->analogue_gain.max) {
            int step = std::max(20, (int)(cur_analogue * strength * 0.3f));
            camera.set_analogue_gain(std::min(cur_analogue + step, cfg->analogue_gain.max));
        } else if (cur_digital < cfg->digital_gain.max) {
            int step = std::max(128, (int)(cur_digital * strength * 0.3f));
            camera.set_digital_gain(std::min(cur_digital + step, cfg->digital_gain.max));
        }
    } else {
        // Too bright - decrease exposure/gain
        // Reverse priority: digital gain first, then analogue, then exposure

        int min_digital = std::max(256, cfg->digital_gain.min);
        int min_analogue = std::max(32, cfg->analogue_gain.min);  // Never go to 0!

        if (cur_digital > min_digital) {
            int step = std::max(64, (int)(cur_digital * strength * 0.3f));
            camera.set_digital_gain(std::max(cur_digital - step, min_digital));
        } else if (cur_analogue > min_analogue) {
            int step = std::max(10, (int)(cur_analogue * strength * 0.3f));
            camera.set_analogue_gain(std::max(cur_analogue - step, min_analogue));
        } else if (cur_exp > cfg->exposure.min) {
            int step = std::max(30, (int)(cur_exp * strength * 0.3f));
            camera.set_exposure(std::max(cur_exp - step, cfg->exposure.min));
        }
    }
}