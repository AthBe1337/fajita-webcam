#include "autoexposure.h"
#include <algorithm>
#include <cmath>

AutoExposure::AutoExposure() {}

void AutoExposure::reset() {
    prev_brightness_ = -1.0f;
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

    // === Emergency recovery for very dark frames ===
    if (brightness < 3.0f) {
        int safe_exp = std::min(cfg->exposure.max, std::max(cfg->exposure.def, 1000));
        int safe_analogue = std::min(cfg->analogue_gain.max, std::max(300, cfg->analogue_gain.min + 200));
        int safe_digital = std::min(cfg->digital_gain.max, std::max(1024, cfg->digital_gain.min + 512));

        if (cur_exp < safe_exp) { camera.set_exposure(safe_exp); return; }
        if (cur_analogue < safe_analogue) { camera.set_analogue_gain(safe_analogue); return; }
        if (cur_digital < safe_digital) { camera.set_digital_gain(safe_digital); return; }

        // Still dark, max everything
        if (cur_exp < cfg->exposure.max) { camera.set_exposure(cfg->exposure.max); return; }
        if (cur_analogue < cfg->analogue_gain.max) { camera.set_analogue_gain(cfg->analogue_gain.max); return; }
        if (cur_digital < cfg->digital_gain.max) { camera.set_digital_gain(cfg->digital_gain.max); return; }
        return;
    }

    // === Detect rapid brightness change ===
    bool scene_change = false;
    if (prev_brightness_ > 0.0f) {
        float change = std::abs(brightness - prev_brightness_) / std::max(prev_brightness_, 1.0f);
        if (change > 0.4f) {  // 40% change
            scene_change = true;
        }
    }
    prev_brightness_ = brightness;

    // === Normal adjustment ===
    float error = (target - brightness) / target;

    // Dead zone
    if (std::abs(error) < 0.05f && !scene_change) return;

    // Scale adjustment by error magnitude and scene change
    float strength = std::min(std::abs(error) * 2.0f, 1.0f);  // 0-1
    if (scene_change) strength = std::max(strength, 0.5f);    // At least 50% for scene changes

    int min_analogue = std::max(32, cfg->analogue_gain.min);
    int min_digital = std::max(256, cfg->digital_gain.min);

    if (error > 0) {
        // === TOO DARK - increase aggressively ===
        // Calculate how much total "exposure value" we need
        // EV ≈ exposure * gain. We want to increase EV by (1 + error).

        // Step 1: Maximize exposure first (best quality)
        if (cur_exp < cfg->exposure.max) {
            int headroom = cfg->exposure.max - cur_exp;
            int step = std::max(100, (int)(headroom * strength));
            camera.set_exposure(std::min(cur_exp + step, cfg->exposure.max));
        }

        // Step 2: Also increase analogue gain if exposure is near max or error is large
        if ((cur_exp >= cfg->exposure.max * 0.7f || error > 0.3f) && cur_analogue < cfg->analogue_gain.max) {
            int headroom = cfg->analogue_gain.max - cur_analogue;
            int step = std::max(30, (int)(headroom * strength * 0.5f));
            camera.set_analogue_gain(std::min(cur_analogue + step, cfg->analogue_gain.max));
        }

        // Step 3: Digital gain as last resort
        if (cur_exp >= cfg->exposure.max * 0.9f &&
            cur_analogue >= cfg->analogue_gain.max * 0.9f &&
            cur_digital < cfg->digital_gain.max) {
            int step = std::max(200, (int)(cur_digital * strength * 0.3f));
            camera.set_digital_gain(std::min(cur_digital + step, cfg->digital_gain.max));
        }
    } else {
        // === TOO BRIGHT - decrease ===
        // More conservative to avoid flicker

        // Step 1: Reduce digital gain first
        if (cur_digital > min_digital) {
            int step = std::max(100, (int)(cur_digital * strength * 0.3f));
            camera.set_digital_gain(std::max(cur_digital - step, min_digital));
        }

        // Step 2: Reduce analogue gain
        if (cur_digital <= min_digital + 100 && cur_analogue > min_analogue) {
            int step = std::max(20, (int)(cur_analogue * strength * 0.25f));
            camera.set_analogue_gain(std::max(cur_analogue - step, min_analogue));
        }

        // Step 3: Reduce exposure last
        if (cur_digital <= min_digital + 100 &&
            cur_analogue <= min_analogue + 30 &&
            cur_exp > cfg->exposure.min) {
            int step = std::max(50, (int)(cur_exp * strength * 0.25f));
            camera.set_exposure(std::max(cur_exp - step, cfg->exposure.min));
        }
    }
}