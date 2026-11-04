// Rate limits for untrusted senders.
#pragma once
#include "clock.h"

#include <algorithm>
#include <deque>
#include <optional>

namespace chthonic {

// Token bucket: refills `rate` tokens per second up to `burst`.
class TokenBucket {
public:
    TokenBucket(double rate, double burst) : rate_(rate), burst_(burst), tokens_(burst) {}
    bool take(TimePoint now) {
        if (last_) tokens_ = std::min(burst_, tokens_ + rate_ * seconds_between(*last_, now));
        last_ = now;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }
    double tokens() const { return tokens_; }

private:
    double rate_, burst_, tokens_;
    std::optional<TimePoint> last_;
};

// Per-connection message limit: excess messages are dropped; excess sustained for `grace` means disconnect.
class MessageRateLimiter {
public:
    enum class Verdict { accept, drop, disconnect };
    MessageRateLimiter(double rate, double burst, Millis grace) : bucket_(rate, burst), grace_(grace) {}
    Verdict check(TimePoint now) {
        if (bucket_.take(now)) {
            // Only a sender that has regained headroom stops counting as over; a flood that rides the refill does not.
            if (bucket_.tokens() >= 1.0) overSince_.reset();
            return Verdict::accept;
        }
        if (!overSince_) overSince_ = now;
        return now - *overSince_ >= grace_ ? Verdict::disconnect : Verdict::drop;
    }

private:
    TokenBucket bucket_;
    Millis grace_;
    std::optional<TimePoint> overSince_;
};

// At most `count` events per sliding `window`.
class WindowLimiter {
public:
    WindowLimiter(size_t count, Millis window) : count_(count), window_(window) {}
    bool allow(TimePoint now) {
        while (!events_.empty() && now - events_.front() >= window_) events_.pop_front();
        if (events_.size() >= count_) return false;
        events_.push_back(now);
        return true;
    }

private:
    size_t count_;
    Millis window_;
    std::deque<TimePoint> events_;
};

} // namespace chthonic
