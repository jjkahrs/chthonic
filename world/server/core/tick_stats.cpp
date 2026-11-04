#include "tick_stats.h"

#include <algorithm>
#include <cmath>

namespace chthonic {

void TickStats::add(double ms) {
    const int b = std::clamp(static_cast<int>(ms * 100.0), 0, kBuckets - 1);
    ++buckets_[b];
    ++count_;
    max_ = std::max(max_, ms);
}

double TickStats::percentile(double p) const {
    if (count_ == 0) return 0;
    const auto target = static_cast<uint64_t>(std::ceil(p * static_cast<double>(count_)));
    uint64_t seen = 0;
    for (int b = 0; b < kBuckets; ++b) {
        seen += buckets_[b];
        if (seen >= target && seen > 0) return (b + 1) / 100.0; // bucket upper edge: never under-reports
    }
    return max_;
}

} // namespace chthonic
