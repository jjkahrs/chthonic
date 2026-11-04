#include "mute_store.h"

#include <core/log.h>
#include <core/utc.h>

namespace chthonic::chat {
namespace {

uint64_t unix_ms(SysTime t) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count());
}

} // namespace

std::vector<std::pair<std::string, SysTime>> MuteStore::load(SysTime now) {
    db::Connection conn(opts_);
    conn.exec("DELETE FROM chat_mutes WHERE until_ms <= ?", {unix_ms(now)});
    std::vector<std::pair<std::string, SysTime>> out;
    for (const auto& row : conn.exec("SELECT name, until_ms FROM chat_mutes").rows)
        out.emplace_back(*row[0], SysTime(std::chrono::milliseconds(std::stoull(*row[1]))));
    return out;
}

void MuteStore::save(const MuteChange& m) {
    if (m.until == SysTime{}) {
        run("unmute", [key = name_key(m.name)](db::Connection& c) {
            c.exec("DELETE FROM chat_mutes WHERE name_key = ?", {key});
        });
        return;
    }
    run("mute", [m, key = name_key(m.name), created = utc_mysql(std::chrono::system_clock::now())](db::Connection& c) {
        c.exec("INSERT INTO chat_mutes (name_key, name, until_ms, muted_by, created_at) VALUES (?, ?, ?, ?, ?) "
               "ON DUPLICATE KEY UPDATE name = VALUES(name), until_ms = VALUES(until_ms), muted_by = VALUES(muted_by), "
               "created_at = VALUES(created_at)",
               {key, m.name, unix_ms(m.until), m.byAccount, created});
    });
}

void MuteStore::purge(SysTime now) {
    run("purge", [ms = unix_ms(now)](db::Connection& c) { c.exec("DELETE FROM chat_mutes WHERE until_ms <= ?", {ms}); });
}

void MuteStore::run(const char* what, std::function<void(db::Connection&)> f) {
    thread_.push([this, what, f = std::move(f)] {
        try {
            if (!threadReady_) {
                db::thread_init();
                threadReady_ = true;
            }
            if (!conn_) {
                conn_ = std::make_unique<db::Connection>(opts_);
            }
            f(*conn_);
        } catch (const std::exception& e) {
            conn_.reset(); // reconnect on the next job
            log::error("mute store write failed", {{"op", what}, {"error", e.what()}});
        }
    });
}

} // namespace chthonic::chat
