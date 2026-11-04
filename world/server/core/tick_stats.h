// Tick duration histogram (10 us buckets up to 1 s) for p50/p99/max without storing samples.
#pragma once
#include <array>
#include <cstdint>

namespace chthonic {

class TickStats {
public:
    void add(double ms);
    uint64_t count() const { return count_; }
    double percentile(double p) const; // p in [0,1], in ms
    double max() const { return max_; }
    void reset() { buckets_.fill(0); count_ = 0; max_ = 0; }

private:
    static constexpr int kBuckets = 100000; // 10 us each
    std::array<uint32_t, kBuckets> buckets_{};
    uint64_t count_ = 0;
    double max_ = 0;
};

} // namespace chthonic
