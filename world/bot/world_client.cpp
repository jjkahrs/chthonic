#include "world_client.h"

namespace chthonic::bot {

using asio::use_awaitable;

WorldClient::WorldClient(asio::any_io_executor ex, asio::ssl::context& tls) : stream_(ex, tls), signal_(ex) {}

asio::awaitable<bool> WorldClient::connect(const std::string& host, uint16_t port, bool tls) {
    tls_ = tls;
    try {
        asio::ip::tcp::resolver resolver(stream_.get_executor());
        auto endpoints = co_await resolver.async_resolve(host, std::to_string(port), use_awaitable);
        co_await asio::async_connect(stream_.next_layer(), endpoints, use_awaitable);
        stream_.next_layer().set_option(asio::ip::tcp::no_delay(true));
        if (tls) {
            stream_.set_verify_mode(asio::ssl::verify_peer);
            stream_.set_verify_callback(asio::ssl::host_name_verification(host));
            SSL_set_tlsext_host_name(stream_.native_handle(), host.c_str());
            co_await stream_.async_handshake(asio::ssl::stream_base::client, use_awaitable);
        }
    } catch (const std::exception& e) {
        lastError = e.what();
        closed_ = true;
        co_return false;
    }
    asio::co_spawn(stream_.get_executor(), [self = shared_from_this()] { return self->read_loop(); }, asio::detached);
    co_return true;
}

asio::awaitable<bool> WorldClient::login(const std::string& token, uint16_t version) {
    send(proto::Hello{version});
    send(proto::Auth{token});
    authOk = co_await wait_for<proto::AuthOk>(Millis(15000));
    co_return authOk.has_value();
}

asio::awaitable<void> WorldClient::read_loop() {
    std::array<uint8_t, proto::kHeaderSize> header{};
    std::vector<uint8_t> payload;
    try {
        for (;;) {
            if (tls_) co_await asio::async_read(stream_, asio::buffer(header), use_awaitable);
            else co_await asio::async_read(stream_.next_layer(), asio::buffer(header), use_awaitable);
            const auto h = proto::parse_header(header);
            payload.resize(h.length);
            if (h.length > 0 && tls_) co_await asio::async_read(stream_, asio::buffer(payload), use_awaitable);
            else if (h.length > 0) co_await asio::async_read(stream_.next_layer(), asio::buffer(payload), use_awaitable);
            proto::Message m;
            if (proto::decode_payload(h.opcode, payload, m) != proto::DecodeStatus::ok) {
                lastError = "undecodable frame from server, opcode " + std::to_string(static_cast<int>(h.opcode));
                break;
            }
            if (auto* ping = std::get_if<proto::Ping>(&m)) { send(proto::Pong{ping->nonce}); continue; }
            if (auto* d = std::get_if<proto::Disconnect>(&m)) disconnectReason = d->reason;
            if (auto* c = std::get_if<proto::Correction>(&m)) moveEpoch = c->epoch;
            if (auto* mc = std::get_if<proto::MapChange>(&m)) moveEpoch = mc->epoch;
            if (onMessage) onMessage(m);
            else inbox_.push_back(std::move(m));
            signal();
        }
    } catch (const std::exception&) {
        // EOF or reset: the server closed the connection
    }
    closed_ = true;
    asio::error_code ec;
    stream_.next_layer().close(ec);
    signal();
}

void WorldClient::send(const proto::Message& m) {
    if (closed_) return;
    if (const auto* move = std::get_if<proto::Move>(&m)) {
        proto::Move stamped = *move;
        stamped.seq = ++moveSeq;
        stamped.epoch = moveEpoch;
        proto::encode(pending_, proto::Message{stamped});
    } else {
        proto::encode(pending_, m);
    }
    flush();
}

void WorldClient::send_raw(std::vector<uint8_t> bytes) {
    if (closed_) return;
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    flush();
}

void WorldClient::flush() {
    if (writing_now_ || pending_.empty() || closed_) return;
    writing_.swap(pending_);
    pending_.clear();
    writing_now_ = true;
    auto done = [self = shared_from_this()](asio::error_code ec, size_t) {
        self->writing_now_ = false;
        self->writing_.clear();
        if (!ec) self->flush();
    };
    if (tls_) asio::async_write(stream_, asio::buffer(writing_), done);
    else asio::async_write(stream_.next_layer(), asio::buffer(writing_), done);
}

void WorldClient::close() {
    if (closed_) return;
    closed_ = true;
    asio::error_code ec;
    stream_.next_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    stream_.next_layer().close(ec);
    signal();
}

asio::awaitable<std::optional<proto::Message>> WorldClient::next(Millis timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (!inbox_.empty()) {
            proto::Message m = std::move(inbox_.front());
            inbox_.pop_front();
            co_return m;
        }
        if (closed_ || std::chrono::steady_clock::now() >= deadline) co_return std::nullopt;
        signal_.expires_at(deadline);
        asio::error_code ec;
        co_await signal_.async_wait(asio::redirect_error(use_awaitable, ec));
    }
}

asio::awaitable<bool> WorldClient::wait_closed(Millis timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!closed_) {
        const auto left = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) co_return false;
        auto m = co_await next(left);
        if (m && onSkipped) onSkipped(*m);
    }
    co_return true;
}

} // namespace chthonic::bot
