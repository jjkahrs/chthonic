// A bot's game connection. Coroutine-based: a reader loop answers Ping and queues every other message; scripts
// `co_await next()` / `wait_for<T>()`, or set onMessage to consume messages as they arrive (load mode).
#pragma once
#include <codec.h>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace chthonic::bot {

using Millis = std::chrono::milliseconds;

// Always held by shared_ptr: the reader coroutine and pending writes keep it alive.
class WorldClient : public std::enable_shared_from_this<WorldClient> {
public:
    WorldClient(asio::any_io_executor ex, asio::ssl::context& tls);
    ~WorldClient() { close(); }

    // TCP connect, then (if tls) the TLS handshake verifying the server certificate for `host`.
    asio::awaitable<bool> connect(const std::string& host, uint16_t port, bool tls = true);
    // Hello + Auth; true once AuthOk arrives (stored in authOk).
    asio::awaitable<bool> login(const std::string& token, uint16_t version = proto::kProtocolVersion);

    // A Move goes out with the next seq and the epoch of the last Correction / MapChange (whatever the caller put in
    // them), like a game client.
    void send(const proto::Message& m);
    void send_raw(std::vector<uint8_t> bytes);
    void close();

    // Next queued message; nullopt on timeout or when the connection is closed and the queue is empty.
    asio::awaitable<std::optional<proto::Message>> next(Millis timeout);
    // Discards other messages (passing them to onSkipped) until a T arrives.
    template <typename T>
    asio::awaitable<std::optional<T>> wait_for(Millis timeout);
    // Waits for the server to close the connection (disconnectReason holds the reason, if one was sent).
    asio::awaitable<bool> wait_closed(Millis timeout);

    bool closed() const { return closed_; }
    std::optional<proto::Code> disconnectReason;
    std::optional<proto::AuthOk> authOk;
    std::string lastError;
    uint32_t moveSeq = 0;   // the last Move's seq
    uint32_t moveEpoch = 0; // from the last Correction or MapChange (the reader loop takes it as soon as it arrives)

    std::function<void(const proto::Message&)> onMessage; // when set, messages bypass the queue
    std::function<void(const proto::Message&)> onSkipped;

private:
    asio::awaitable<void> read_loop();
    void flush();
    void signal() { signal_.cancel(); }

    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    bool tls_ = true;
    bool closed_ = false;
    std::deque<proto::Message> inbox_;
    asio::steady_timer signal_;
    std::vector<uint8_t> pending_, writing_;
    bool writing_now_ = false;
};

template <typename T>
asio::awaitable<std::optional<T>> WorldClient::wait_for(Millis timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) co_return std::nullopt;
        auto m = co_await next(left);
        if (!m) co_return std::nullopt;
        if (auto* t = std::get_if<T>(&*m)) co_return *t;
        if (onSkipped) onSkipped(*m);
    }
}

} // namespace chthonic::bot
