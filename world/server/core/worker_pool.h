// A fixed set of worker threads that run one batch of jobs at a time: run() hands job(0) .. job(n - 1) out to them
// and returns once every one is done (a barrier). The caller only waits. JobThread (one thread, a queue) does not
// fit: run needs every worker and a join point. World runs its map context updates here (MAP_THREADS).
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace chthonic {

class WorkerPool {
public:
    explicit WorkerPool(size_t threads) {
        for (size_t i = 0; i < threads; ++i) threads_.emplace_back([this](std::stop_token stop) { work(stop); });
    }
    ~WorkerPool() {
        for (auto& t : threads_) t.request_stop();
        wake_.notify_all(); // the jthreads join here
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    size_t size() const { return threads_.size(); }

    // Runs job(i) for every i in [0, n) on the workers, each i once, in no set order; returns when all are done. The
    // first exception a job threw is rethrown here, after the others finished.
    void run(size_t n, const std::function<void(size_t)>& job) {
        {
            std::lock_guard lk(mu_);
            job_ = &job;
            n_ = n;
            next_ = 0;
            busy_ = threads_.size();
            error_ = nullptr;
            ++batch_;
        }
        wake_.notify_all();
        spin([&] { return busy_ == 0; });
        std::unique_lock lk(mu_);
        done_.wait(lk, [&] { return busy_ == 0; });
        job_ = nullptr;
        if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
    }

private:
    // Polls done() for up to 100 us before the caller sleeps on a condition variable: a tick's update steps and
    // serial points follow each other closer than that, and waking a sleeping thread takes about that long (more in
    // a VM). Between ticks the workers sleep.
    template <class Done> static void spin(Done done) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
        while (!done() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    }

    void work(std::stop_token stop) {
        uint64_t seen = 0;
        for (;;) {
            size_t n = 0;
            const std::function<void(size_t)>* job = nullptr;
            spin([&] { return batch_ != seen || stop.stop_requested(); }); // a tick's next step is that close
            {
                std::unique_lock lk(mu_);
                if (!wake_.wait(lk, stop, [&] { return batch_ != seen; })) return; // stop requested
                seen = batch_;
                n = n_;
                job = job_;
            }
            for (size_t i = next_++; i < n; i = next_++) {
                try {
                    (*job)(i);
                } catch (...) {
                    std::lock_guard lk(mu_);
                    if (!error_) error_ = std::current_exception();
                }
            }
            std::lock_guard lk(mu_);
            if (--busy_ == 0) done_.notify_one();
        }
    }

    std::mutex mu_;
    std::condition_variable_any wake_;
    std::condition_variable done_;
    const std::function<void(size_t)>* job_ = nullptr;
    size_t n_ = 0;
    std::atomic<size_t> busy_{0};    // workers still in the batch: changed under mu_, read by spin without it
    std::atomic<size_t> next_{0};    // the next job index to hand out
    std::atomic<uint64_t> batch_{0}; // like busy_
    std::exception_ptr error_;
    std::vector<std::jthread> threads_; // last: the workers start once the members above exist
};

} // namespace chthonic
