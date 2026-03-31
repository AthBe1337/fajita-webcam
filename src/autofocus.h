#pragma once
#include "camera.h"
#include "isp.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

enum class AFMode { Manual, OneShot, Continuous };
enum class AFState { Idle, Scanning, Locked, Failed };

class AutoFocus {
public:
    AutoFocus();
    ~AutoFocus();

    // Trigger one-shot AF. Non-blocking, runs in background.
    void trigger(Camera& camera, std::function<float()> get_metric);

    void set_mode(AFMode mode) { mode_ = mode; }
    AFMode mode() const { return mode_; }
    AFState state() const { return state_; }

    // For continuous AF: call periodically with current focus metric
    void update_continuous(Camera& camera, float metric);

    int best_position() const { return best_pos_; }

private:
    void scan_thread(Camera& camera, std::function<float()> get_metric);

    std::atomic<AFMode> mode_{AFMode::Manual};
    std::atomic<AFState> state_{AFState::Idle};
    std::atomic<int> best_pos_{0};
    std::thread thread_;
    std::mutex thread_mutex_;

    // Continuous AF state
    float locked_metric_ = 0;
    int check_counter_ = 0;
};
