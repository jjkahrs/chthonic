// Little-endian byte writer/reader for message payloads. Strings are u16 length + bytes.
// The reader never throws: any short read or bad value sets ok() to false and further reads return zero.
#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace chthonic::proto {

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
    // n bytes appended at once, for a caller that fills many fixed-size fields (le32): EntityMoves writes MBs a tick,
    // and a push_back per byte is most of that.
    uint8_t* grow(size_t n) {
        out_.resize(out_.size() + n);
        return out_.data() + out_.size() - n;
    }
    static void le32(uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
    void str(const std::string& s) {
        // Callers keep strings far below 64 KB (frame limit); truncate rather than corrupt the frame.
        const auto n = static_cast<uint16_t>(s.size() > 0xFFFF ? 0xFFFF : s.size());
        u16(n);
        out_.insert(out_.end(), s.begin(), s.begin() + n);
    }

private:
    void put(uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    std::vector<uint8_t>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> in) : in_(in) {}
    uint8_t u8() { return static_cast<uint8_t>(get(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get(4)); }
    uint64_t u64() { return get(8); }
    float f32() {
        const float v = std::bit_cast<float>(u32());
        if (!std::isfinite(v)) ok_ = false; // NaN/inf never valid on the wire
        return ok_ ? v : 0.0f;
    }
    std::string str() {
        const uint16_t n = u16();
        if (!ok_ || in_.size() - pos_ < n) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(in_.data() + pos_), n);
        pos_ += n;
        return s;
    }
    void fail() { ok_ = false; }
    bool ok() const { return ok_; }
    bool done() const { return ok_ && pos_ == in_.size(); }

private:
    uint64_t get(int bytes) {
        if (!ok_ || in_.size() - pos_ < static_cast<size_t>(bytes)) { ok_ = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= uint64_t{in_[pos_ + i]} << (8 * i);
        pos_ += bytes;
        return v;
    }
    std::span<const uint8_t> in_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace chthonic::proto
