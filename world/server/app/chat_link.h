// The world server's end of the internal link to the chat service (CHAT_LINK_*; docs/chat/protocol.html#link):
// a TLS connection that starts with LinkHello{secret}, then tells the chat service which characters are in the world
// and on which map, and takes the mutes it sends back. Reconnects with exponential backoff (CHAT_LINK_RETRY_MIN_MS ..
// MAX_MS); while it is down the world runs as before and nothing is queued: each (re)connect sends the full character
// list and gets the full mute list. All calls, and the callbacks, on the game thread; its Session lives on a net thread
// and posts back here.
#pragma once
#include "../core/clock.h"
#include "../net/net_threads.h"
#include "../net/session.h"

#include <messages.h>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace chthonic {

class ChatLink {
public:
    struct Options {
        std::string host;
        uint16_t port = 7779;
        std::string secret;
        std::string caFile; // PEM the chat service's certificate must chain to
        Millis retryMin{500}, retryMax{30000};
    };
    using Snapshot = std::function<std::vector<proto::CharOnline>()>; // every character in the world now
    using OnMute = std::function<void(const proto::MuteState&)>;
    using OnReset = std::function<void()>; // the link is up: forget every mute, the full list follows

    ChatLink(asio::io_context& io, net::NetThreads& net, Options options, Snapshot snapshot, OnMute onMute, OnReset onReset);

    void start();
    void stop(); // no reconnects after this

    // Sent while the link is up; dropped otherwise (the next connect sends the snapshot instead).
    void char_online(const proto::CharOnline& c);
    void map_changed(uint64_t charId, uint16_t map);
    void char_offline(uint64_t charId);
    bool up() const { return up_; }

private:
    void connect();
    void retry_later();
    void on_message(proto::Message&& m);
    void on_closed();
    void send(const proto::Message& m);

    asio::io_context& io_; // the game thread's
    net::NetThreads& net_;
    Options opts_;
    Snapshot snapshot_;
    OnMute onMute_;
    OnReset onReset_;
    asio::ssl::context tls_;
    asio::steady_timer retry_;
    std::shared_ptr<net::Session> session_;
    net::SessionLimits limits_;
    Millis backoff_;
    uint32_t serial_ = 0; // the session id of each attempt; a post from an older one is ignored
    bool up_ = false;
    bool stopped_ = false;
};

} // namespace chthonic
