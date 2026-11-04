// NET_THREADS io_contexts, one thread each, for the world's sessions (TLS, framing, socket I/O). Each accepted or
// connected socket is made on next(); its Session then lives on that one thread. The game thread never runs these.
#pragma once
#include <asio.hpp>

#include <memory>
#include <thread>
#include <vector>

namespace chthonic::net {

class NetThreads {
public:
    explicit NetThreads(size_t threads); // starts them; each runs until stop()
    ~NetThreads() { stop(); }
    NetThreads(const NetThreads&) = delete;
    NetThreads& operator=(const NetThreads&) = delete;

    // ponytail: round-robin at connect, no rebalancing; a thread that ends up with the heavy clients stays busier.
    // Rebalance (move sessions) or pick the least-loaded context if net stats show uneven busy %. One caller thread.
    asio::io_context& next() { return *contexts_[next_++ % contexts_.size()]; }
    size_t size() const { return contexts_.size(); }
    // Stops every context (pending handlers are dropped) and joins every thread. Idempotent.
    void stop();
    // CPU time each thread has used so far, ms (it only runs handlers: busy time). Any thread; valid until stop().
    std::vector<double> cpu_ms();

private:
    std::vector<std::unique_ptr<asio::io_context>> contexts_;
    std::vector<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::vector<std::thread> threads_;
    size_t next_ = 0;
};

} // namespace chthonic::net
