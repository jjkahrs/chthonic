// MAX_PREAUTH_PER_IP: sessions per remote address that have not authenticated yet (no AuthOk), so one address can't
// hold every MAX_CONNECTIONS slot with sockets that never log in. Not thread-safe: the thread that owns the server's
// client map calls it (GameServer: the game thread; ChatServer: its io thread).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace chthonic::net {

class PreauthLimit {
public:
    // Counts a new session from ip; false (and not counted) when ip already has max. max 0 = no limit (not counted).
    bool try_add(const std::string& ip, uint32_t max) {
        if (max == 0) return true;
        uint32_t& n = count_[ip];
        if (n >= max) return false;
        ++n;
        return true;
    }
    // The session authenticated or closed: once per successful try_add.
    void release(const std::string& ip) {
        auto it = count_.find(ip);
        if (it == count_.end()) return;
        if (--it->second == 0) count_.erase(it);
    }
    uint32_t count(const std::string& ip) const {
        auto it = count_.find(ip);
        return it == count_.end() ? 0 : it->second;
    }

private:
    std::unordered_map<std::string, uint32_t> count_;
};

} // namespace chthonic::net
