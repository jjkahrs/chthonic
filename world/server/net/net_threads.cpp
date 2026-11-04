#include "net_threads.h"
#include "../core/log.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace chthonic::net {
namespace {

// The thread's own CPU clock (user + kernel), read from any thread: no cost on the thread itself.
double thread_cpu_ms(std::thread& t) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(t.native_handle(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }; // 100 ns
    return double(ticks(kernel) + ticks(user)) / 1e4;
#else
    clockid_t clock;
    timespec ts{};
    if (pthread_getcpuclockid(t.native_handle(), &clock) != 0 || clock_gettime(clock, &ts) != 0) return 0;
    return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) / 1e6;
#endif
}

} // namespace

NetThreads::NetThreads(size_t threads) {
    for (size_t i = 0; i < threads; ++i) {
        contexts_.push_back(std::make_unique<asio::io_context>(1));
        work_.push_back(asio::make_work_guard(*contexts_.back()));
    }
    for (auto& c : contexts_)
        threads_.emplace_back([&io = *c] {
            try {
                io.run();
            } catch (const std::exception& e) { // a handler threw: same end as on the game thread, minus the cleanup
                log::error("fatal: net thread", {{"error", e.what()}});
                std::abort();
            }
        });
}

void NetThreads::stop() {
    work_.clear();
    for (auto& c : contexts_) c->stop();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
}

std::vector<double> NetThreads::cpu_ms() {
    std::vector<double> out;
    for (auto& t : threads_) out.push_back(t.joinable() ? thread_cpu_ms(t) : 0);
    return out;
}

} // namespace chthonic::net
