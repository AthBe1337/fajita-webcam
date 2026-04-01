#pragma once
#include <vector>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Lightweight barrier-based parallel_for for splitting row work across cores.
// Workers are persistent — created once, reused every frame.
class ThreadPool {
public:
    explicit ThreadPool(int num_threads = 0) {
        if (num_threads <= 0)
            num_threads = std::max(1, (int)std::thread::hardware_concurrency());
        num_threads_ = num_threads;
        workers_.resize(num_threads);
        for (int i = 0; i < num_threads; i++) {
            workers_[i] = std::thread([this, i] { worker_loop(i); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        start_cv_.notify_all();
        for (auto& w : workers_)
            if (w.joinable()) w.join();
    }

    int num_threads() const { return num_threads_; }

    // Execute fn(thread_id) for thread_id in [0, num_threads).
    // Blocks until all threads complete. fn must be thread-safe.
    void run(std::function<void(int)> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = std::move(fn);
            remaining_ = num_threads_;
            generation_++;
        }
        start_cv_.notify_all();

        // Wait for all workers to finish
        std::unique_lock<std::mutex> lock(done_mutex_);
        done_cv_.wait(lock, [&] { return remaining_ == 0; });
    }

    // Convenience: split [0, count) across threads, calling fn(start, end) per chunk.
    void parallel_for(int count, std::function<void(int, int)> fn) {
        if (count <= 0) return;
        int n = num_threads_;
        run([&](int tid) {
            int chunk = (count + n - 1) / n;
            int start = tid * chunk;
            int end = std::min(start + chunk, count);
            if (start < count)
                fn(start, end);
        });
    }

private:
    void worker_loop(int tid) {
        uint64_t my_gen = 0;
        while (true) {
            std::function<void(int)> fn;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                start_cv_.wait(lock, [&] {
                    return shutdown_ || generation_ > my_gen;
                });
                if (shutdown_) return;
                my_gen = generation_;
                fn = task_;
            }

            fn(tid);

            {
                std::lock_guard<std::mutex> lock(done_mutex_);
                remaining_--;
            }
            if (remaining_ == 0)
                done_cv_.notify_one();
        }
    }

    int num_threads_;
    std::vector<std::thread> workers_;

    std::mutex mutex_;
    std::condition_variable start_cv_;
    std::function<void(int)> task_;
    uint64_t generation_ = 0;
    bool shutdown_ = false;

    std::mutex done_mutex_;
    std::condition_variable done_cv_;
    std::atomic<int> remaining_{0};
};
