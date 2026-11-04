#include "event_bus.h"
#include "../core/log.h"

#include <algorithm>
#include <format>
#include <utility>

namespace chthonic {

EventBus::Subscription::Subscription(Subscription&& other) noexcept
    : bus_(std::exchange(other.bus_, nullptr)), id_(std::exchange(other.id_, 0)) {}

EventBus::Subscription& EventBus::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        bus_ = std::exchange(other.bus_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void EventBus::Subscription::unsubscribe() {
    if (bus_) bus_->unsubscribe(id_);
    bus_ = nullptr;
}

EventBus::EventBus() { dispatcher_.sink<Event>().connect<&EventBus::deliver>(*this); }

void EventBus::publish(std::string name, nlohmann::json payload, std::string publisher) {
    dispatcher_.enqueue(Event{std::move(name), std::move(payload), std::move(publisher)});
}

EventBus::Subscription EventBus::subscribe(std::string name, Callback callback) {
    names_.emplace(nextId_, name);
    subscribers_[std::move(name)].push_back({nextId_, std::move(callback), true});
    return Subscription(this, nextId_++);
}

void EventBus::unsubscribe(uint64_t id) {
    const auto named = names_.find(id);
    if (named == names_.end()) return;
    const auto bucket = subscribers_.find(named->second);
    const auto it = std::ranges::find(bucket->second, id, &Subscriber::id);
    if (draining_) {
        it->active = false; // erased after the drain
        pruned_.push_back(named->second);
    } else {
        bucket->second.erase(it);
        if (bucket->second.empty()) subscribers_.erase(bucket);
    }
    names_.erase(named);
}

void EventBus::drain() {
    draining_ = true;
    dispatcher_.update<Event>(); // only the events queued before this call
    draining_ = false;
    for (const std::string& name : pruned_) {
        const auto bucket = subscribers_.find(name);
        if (bucket == subscribers_.end()) continue; // listed twice, already erased
        std::erase_if(bucket->second, [](const Subscriber& s) { return !s.active; });
        if (bucket->second.empty()) subscribers_.erase(bucket);
    }
    pruned_.clear();
}

void EventBus::deliver(Event& queued) {
    // queued lives in the dispatcher's vector: a subscriber's publish can grow it and move queued away mid-loop.
    const Event event = std::move(queued);
    const auto bucket = subscribers_.find(event.name);
    if (bucket == subscribers_.end()) return;
    // Buckets are not erased during a drain. Index loop over the count at the start: subscriptions added by a callback
    // wait for the next event.
    const std::deque<Subscriber>& subscribers = bucket->second;
    const size_t count = subscribers.size();
    for (size_t i = 0; i < count; ++i) {
        const Subscriber& subscriber = subscribers[i];
        if (!subscriber.active) continue;
        try {
            subscriber.callback(event.payload);
        } catch (const std::exception& e) { // nothing may escape to the tick
            log::error(std::format("Event '{}': C++ subscriber failed: {}", event.name, e.what()));
        } catch (...) {
            log::error(std::format("Event '{}': C++ subscriber failed", event.name));
        }
    }
}

void EventBus::dropQueued() { dispatcher_.clear<Event>(); }

} // namespace chthonic
