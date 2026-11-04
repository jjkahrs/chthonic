// Event bus: named events with plain-data JSON payloads, queued on publish and delivered by drain(). Same
// behaviour as the client's. One per lua::Runtime; Lua systems use it through
// chthonic.publish/subscribe/unsubscribe (docs/world/lua.html).
#pragma once
#include <entt/signal/dispatcher.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chthonic {

class EventBus {
public:
    using Callback = std::function<void(const nlohmann::json& payload)>;

    // Unsubscribes when destroyed. Must not outlive its bus. Unsubscribing twice does nothing.
    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        ~Subscription() { unsubscribe(); }
        void unsubscribe();
        uint64_t id() const { return id_; }

    private:
        friend class EventBus;
        Subscription(EventBus* bus, uint64_t id) : bus_(bus), id_(id) {}
        EventBus* bus_ = nullptr;
        uint64_t id_ = 0;
    };

    EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Queues an event for the next drain().
    void publish(std::string name, nlohmann::json payload, std::string publisher = "C++");
    // Subscribers of an event are called in subscription order. One added during a drain gets events from the next
    // one on; one removed during a drain gets nothing more, even from the current event.
    [[nodiscard]] Subscription subscribe(std::string name, Callback callback);
    // Delivers the events queued before this call, in publish order; events published meanwhile wait for the next
    // drain. A subscriber that throws is logged and the rest still run.
    void drain();
    void dropQueued();

private:
    struct Event {
        std::string name;
        nlohmann::json payload;
        std::string publisher;
    };
    struct Subscriber {
        uint64_t id;
        Callback callback;
        bool active;
    };

    void deliver(Event& event);
    void unsubscribe(uint64_t id);

    entt::dispatcher dispatcher_;
    // Subscribers by event name, each in subscription order, so a delivery touches only its name's. A callback may
    // subscribe while it runs and that must not move the running callback: deque push_back keeps existing elements in
    // place, and an unordered_map rehash keeps its nodes (the deques) in place. Removed entries, and buckets left
    // empty, are erased outside drains only.
    std::unordered_map<std::string, std::deque<Subscriber>> subscribers_;
    std::unordered_map<uint64_t, std::string> names_; // a live subscription's id -> its bucket
    std::vector<std::string> pruned_;                 // buckets with entries removed during this drain
    uint64_t nextId_ = 1; // never reused, so a stale handle can't remove a newer subscription
    bool draining_ = false;
};

} // namespace chthonic
