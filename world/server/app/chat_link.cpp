#include "chat_link.h"
#include "../core/log.h"
#include "../net/tls_context.h"

#include <algorithm>

namespace chthonic {
namespace {

// The link is trusted traffic between two servers: no per-message rate limit worth the name.
net::SessionLimits link_limits() {
    net::SessionLimits l;
    l.maxFrameBytes = 4096;
    l.msgRate = 100000;
    l.msgBurst = 100000;
    return l;
}

} // namespace

ChatLink::ChatLink(asio::io_context& io, net::NetThreads& net, Options options, Snapshot snapshot, OnMute onMute,
                   OnReset onReset)
    : io_(io), net_(net), opts_(std::move(options)), snapshot_(std::move(snapshot)), onMute_(std::move(onMute)),
      onReset_(std::move(onReset)), tls_(net::make_client_tls(opts_.caFile, opts_.host)), retry_(io),
      limits_(link_limits()), backoff_(opts_.retryMin) {}

void ChatLink::start() {
    log::info("chat link: connecting", {{"host", opts_.host}, {"port", opts_.port}});
    connect();
}

void ChatLink::stop() {
    stopped_ = true;
    retry_.cancel();
    if (session_) session_->disconnect(proto::Code::server_shutdown);
}

void ChatLink::connect() {
    if (stopped_) return;
    auto resolver = std::make_shared<asio::ip::tcp::resolver>(io_);
    resolver->async_resolve(opts_.host, std::to_string(opts_.port), [this, resolver](asio::error_code ec, auto endpoints) {
        if (stopped_) return;
        if (ec) {
            log::debug("chat link: resolve failed", {{"host", opts_.host}, {"error", ec.message()}});
            return retry_later();
        }
        // The socket (and so the Session) on a net thread; the connect's handlers here.
        auto socket = std::make_shared<asio::ip::tcp::socket>(net_.next());
        asio::async_connect(*socket, endpoints, asio::bind_executor(io_, [this, socket](asio::error_code ec2, const auto&) {
            if (stopped_) return;
            if (ec2) {
                log::debug("chat link: connect failed", {{"error", ec2.message()}});
                return retry_later();
            }
            socket->set_option(asio::ip::tcp::no_delay(true), ec2);
            const uint32_t serial = ++serial_;
            session_ = std::make_shared<net::Session>(
                serial, std::move(*socket), tls_, limits_,
                [this, serial](net::Session&, proto::Message&& m) {
                    asio::post(io_, [this, serial, m = std::move(m)]() mutable {
                        if (serial == serial_ && session_) on_message(std::move(m));
                    });
                },
                [this, serial](net::Session&) {
                    asio::post(io_, [this, serial] {
                        if (serial == serial_ && session_) on_closed();
                    });
                },
                asio::ssl::stream_base::client);
            session_->start();
            session_->send(proto::LinkHello{proto::kProtocolVersion, opts_.secret}); // goes once TLS is up; never logged
        }));
    });
}

void ChatLink::retry_later() {
    if (stopped_) return;
    retry_.expires_after(backoff_);
    backoff_ = std::min(backoff_ * 2, opts_.retryMax);
    retry_.async_wait([this](asio::error_code ec) {
        if (!ec) connect();
    });
}

void ChatLink::on_message(proto::Message&& m) {
    if (std::holds_alternative<proto::LinkOk>(m)) {
        up_ = true;
        backoff_ = opts_.retryMin;
        session_->authenticated();
        onReset_();
        const auto chars = snapshot_();
        for (const auto& c : chars) send(c);
        log::info("chat link: up", {{"host", opts_.host}, {"port", opts_.port}, {"characters", chars.size()}});
    } else if (auto* mute = std::get_if<proto::MuteState>(&m)) {
        if (up_) onMute_(*mute);
    } else if (auto* d = std::get_if<proto::Disconnect>(&m)) {
        log::error("chat link: refused by the chat service", {{"reason", proto::code_name(d->reason)}});
    } else {
        log::warn("chat link: unexpected message", {{"opcode", proto::opcode_name(proto::opcode_of(m))}});
        session_->disconnect(proto::Code::unexpected_message);
    }
}

void ChatLink::on_closed() {
    if (up_) log::warn("chat link: down", {{"host", opts_.host}, {"port", opts_.port}});
    up_ = false;
    session_.reset();
    retry_later();
}

void ChatLink::send(const proto::Message& m) {
    if (up_ && session_) session_->send(m);
}

void ChatLink::char_online(const proto::CharOnline& c) { send(c); }
void ChatLink::map_changed(uint64_t charId, uint16_t map) { send(proto::MapChanged{charId, map}); }
void ChatLink::char_offline(uint64_t charId) { send(proto::CharOffline{charId}); }

} // namespace chthonic
