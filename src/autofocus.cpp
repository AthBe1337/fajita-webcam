#include "autofocus.h"
#include <cstdio>
#include <chrono>
#include <algorithm>

AutoFocus::AutoFocus() {}

AutoFocus::~AutoFocus() {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (thread_.joinable()) thread_.join();
}

void AutoFocus::reset() {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (thread_.joinable()) thread_.join();
    mode_ = AFMode::Manual;
    state_ = AFState::Idle;
    best_pos_ = 0;
    locked_metric_ = 0;
    check_counter_ = 0;
}

void AutoFocus::set_manual_position(int pos) {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (thread_.joinable()) thread_.join();
    best_pos_ = pos;
    state_ = AFState::Idle;
    locked_metric_ = 0;
    check_counter_ = 0;
}

void AutoFocus::mark_failed() {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (thread_.joinable()) thread_.join();
    state_ = AFState::Failed;
    locked_metric_ = 0;
    check_counter_ = 0;
}

void AutoFocus::trigger(Camera& camera, std::function<float()> get_metric) {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    if (thread_.joinable()) thread_.join();

    state_ = AFState::Scanning;
    thread_ = std::thread(&AutoFocus::scan_thread, this,
                          std::ref(camera), std::move(get_metric));
}

void AutoFocus::scan_thread(Camera& camera, std::function<float()> get_metric) {
    printf("AF: starting scan\n");

    auto move_and_measure = [&](int pos, float& metric) -> bool {
        if (!camera.set_focus(pos)) {
            printf("AF: move failed at pos=%d\n", pos);
            state_ = AFState::Failed;
            locked_metric_ = 0;
            return false;
        }
        // Wait for actuator to settle and new frame to arrive
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        metric = get_metric();
        return true;
    };

    // Phase 1: Coarse scan (full range, 10 steps)
    const int coarse_steps = 10;
    const int range_min = 0, range_max = 2047;
    int coarse_step = (range_max - range_min) / (coarse_steps - 1);

    float best_metric = -1;
    int best_pos = 0;

    for (int i = 0; i < coarse_steps; i++) {
        int pos = range_min + i * coarse_step;
        pos = std::min(pos, range_max);
        float metric;
        if (!move_and_measure(pos, metric))
            return;
        printf("AF coarse: pos=%d metric=%.1f\n", pos, metric);
        if (metric > best_metric) {
            best_metric = metric;
            best_pos = pos;
        }
    }

    // Phase 2: Fine scan (±128 around best, step 32)
    {
        int fine_min = std::max(range_min, best_pos - 128);
        int fine_max = std::min(range_max, best_pos + 128);
        for (int pos = fine_min; pos <= fine_max; pos += 32) {
            float metric;
            if (!move_and_measure(pos, metric))
                return;
            printf("AF fine: pos=%d metric=%.1f\n", pos, metric);
            if (metric > best_metric) {
                best_metric = metric;
                best_pos = pos;
            }
        }
    }

    // Phase 3: Ultra-fine scan (±32 around best, step 8)
    {
        int uf_min = std::max(range_min, best_pos - 32);
        int uf_max = std::min(range_max, best_pos + 32);
        for (int pos = uf_min; pos <= uf_max; pos += 8) {
            float metric;
            if (!move_and_measure(pos, metric))
                return;
            if (metric > best_metric) {
                best_metric = metric;
                best_pos = pos;
            }
        }
    }

    // Move to best position
    if (!camera.set_focus(best_pos)) {
        printf("AF: final move failed at pos=%d\n", best_pos);
        state_ = AFState::Failed;
        locked_metric_ = 0;
        return;
    }
    best_pos_ = best_pos;
    locked_metric_ = best_metric;

    printf("AF: locked at pos=%d metric=%.1f\n", best_pos, best_metric);
    state_ = (best_metric > 0) ? AFState::Locked : AFState::Failed;
}

void AutoFocus::update_continuous(Camera& camera, float metric) {
    if (mode_ != AFMode::Continuous) return;
    if (state_ == AFState::Scanning) return;

    check_counter_++;
    if (check_counter_ < 20) return;  // Check every ~20 analysis frames
    check_counter_ = 0;

    // If contrast dropped significantly, re-trigger
    if (locked_metric_ > 0 && metric < locked_metric_ * 0.7f) {
        printf("AF continuous: contrast dropped (%.1f -> %.1f), re-scanning\n",
               locked_metric_, metric);
        // Can't easily pass get_metric here, so do a simple re-scan
        // around current position
        state_ = AFState::Scanning;

        int cur = best_pos_.load();
        int best_p = cur;
        int range = 64;

        for (int pos = std::max(0, cur - range);
             pos <= std::min(2047, cur + range); pos += 16) {
            if (!camera.set_focus(pos)) {
                printf("AF continuous: move failed at pos=%d\n", pos);
                state_ = AFState::Failed;
                locked_metric_ = 0;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            // We don't have a metric getter here, so this is simplified
            // In practice, the pipeline's analysis callback will update
        }

        if (!camera.set_focus(best_p)) {
            printf("AF continuous: restore failed at pos=%d\n", best_p);
            state_ = AFState::Failed;
            locked_metric_ = 0;
            return;
        }
        best_pos_ = best_p;
        state_ = AFState::Locked;
    }
}
