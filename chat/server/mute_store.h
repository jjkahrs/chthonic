// The mutes in MySQL (chthonic_chat.chat_mutes, migrations/): loaded once at startup, then written behind the router on a
// worker thread, so a slow or unavailable database never stalls chat. A failed write is logged and the mute stays in
// memory (it is lost on a restart if the database never comes back).
#pragma once
#include "chat_router.h"

#include <core/job_thread.h>
#include <db/mysql.h>

#include <memory>
#include <utility>
#include <vector>

namespace chthonic::chat {

class MuteStore {
public:
    explicit MuteStore(db::ConnectOptions opts) : opts_(std::move(opts)) {}
    ~MuteStore() { thread_.stop(); }

    // Every mute still running at now; deletes the expired ones. Blocking (startup). Throws db::DbError.
    std::vector<std::pair<std::string, SysTime>> load(SysTime now);
    // Queued: insert / replace the mute, or delete it (until == SysTime{}).
    void save(const MuteChange& m);
    // Queued: deletes the mutes that ended by now.
    void purge(SysTime now);

private:
    // Runs f with a connection (made on first use, and again after a failure); logs and drops the job if it fails.
    void run(const char* what, std::function<void(db::Connection&)> f);

    db::ConnectOptions opts_;
    std::unique_ptr<db::Connection> conn_; // only on thread_
    bool threadReady_ = false;             // db::thread_init ran on thread_
    JobThread thread_;                     // last: stops before the connection goes
};

} // namespace chthonic::chat
