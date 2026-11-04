// A single background thread running queued jobs in order.
#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace chthonic {

class JobThread {
public:
    JobThread() : thread_([this] { run(); }) {}
    ~JobThread() { stop(); }

    void push(std::function<void()> job) {
        {
            std::lock_guard lk(mu_);
            if (stopping_) return;
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    // Drops queued jobs, waits for the running one.
    void stop() {
        {
            std::lock_guard lk(mu_);
            stopping_ = true;
            jobs_.clear();
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [&] { return stopping_ || !jobs_.empty(); });
                if (stopping_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    std::thread thread_; // last: starts after the other members exist
};

} // namespace chthonic
