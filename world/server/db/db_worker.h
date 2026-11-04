// The only threads that talk to MySQL: a pool of DB_POOL_SIZE workers, each with its own connection. The game thread
// enqueues work; results come back through `post` (asio::post onto the io_context), so the tick never blocks on the
// database.
//
// Saves are not ordinary jobs: they go into a pending map keyed by character (latest position wins) and are
// retried with exponential backoff until they land. A pending save is never dropped, and a character is never
// loaded while its save is still pending. Both hold because a character belongs to one worker (charId % pool size):
// its saves and its EnterWorld load run there, in order. Lobby jobs (list/create/delete) go to any idle worker.
#pragma once
#include "../core/clock.h"
#include "char_repo.h"
#include "mysql.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace chthonic::db {

struct WorkerOptions {
    ConnectOptions connect;
    uint32_t realmId = 0;
    Millis retryMin{500}, retryMax{30000};
    uint32_t poolSize = 4; // worker threads = connections
};

struct SaveStats {
    uint64_t ok = 0;
    uint64_t failedAttempts = 0;
    size_t pending = 0;
};

// The save queue's pure parts (no MySQL), checked by tests/self_check.cpp.
struct PendingSave {
    CharSave save;
    uint32_t attempts = 0;
    TimePoint due{};
};
using PendingSaves = std::map<uint64_t, PendingSave>;

// The worker that owns charId's saves and EnterWorld load.
inline size_t shard_of(uint64_t charId, size_t shards) { return charId % shards; }
// Queues (or replaces) s: the latest save wins and keeps the retry schedule of an older pending save.
void queue_save(PendingSaves& pending, const CharSave& s, TimePoint now);
// Up to `max` saves due at `now`, earliest due first.
std::vector<uint64_t> due_saves(const PendingSaves& pending, TimePoint now, size_t max);

class DbWorker {
public:
    using Post = std::function<void(std::function<void()>)>;

    // Fair interleave: a worker lands at most this many of its own due saves, then takes one job, and repeats. A
    // lobby job waits behind at most this many saves; a steady stream of jobs still lets saves through.
    static constexpr size_t kSavesPerJob = 8;

    DbWorker(WorkerOptions opts, Post post);
    ~DbWorker();

    // Runs `work` on any idle worker; `done` runs via `post` with nullopt when the DB is unavailable.
    template <typename T>
    void query(std::function<T(CharRepo&)> work, std::function<void(std::optional<T>)> done) {
        submit<T>(std::nullopt, std::move(work), std::move(done));
    }
    // Same, but on charId's worker, after it lands any pending save for charId (EnterWorld must read the latest
    // position).
    template <typename T>
    void query_for_char(uint64_t charId, std::function<T(CharRepo&)> work, std::function<void(std::optional<T>)> done) {
        submit<T>(charId, std::move(work), std::move(done));
    }

    // Queues (or replaces) the save for s.charId on its worker.
    void save(const CharSave& s);

    // Stops taking jobs, flushes pending saves until `deadline`, joins. Returns the saves that could not be flushed.
    size_t stop(TimePoint deadline);

    SaveStats stats() const;

private:
    // One worker thread with its own connection, and the saves of the characters it owns.
    struct Shard {
        std::unique_ptr<Connection> conn;
        std::optional<TimePoint> lastConnectFailure;
        std::deque<std::function<void(Shard&)>> jobs; // query_for_char for its characters
        PendingSaves pending;
        std::condition_variable cv;
        bool idle = false; // waiting on cv with nothing to do: a shared job may wake it
        std::thread thread;
    };
    using Job = std::function<void(Shard&)>;

    template <typename T>
    void submit(std::optional<uint64_t> charId, std::function<T(CharRepo&)> work,
                std::function<void(std::optional<T>)> done) {
        enqueue(charId, [this, charId, work = std::move(work), done = std::move(done)](Shard& sh) {
            auto result = std::make_shared<std::optional<T>>();
            run_job(sh, [&](CharRepo& repo) { *result = work(repo); }, charId);
            post_([done, result] { done(std::move(*result)); });
        });
    }

    void run(Shard& sh);
    void enqueue(std::optional<uint64_t> charId, Job job);
    Shard& shard(uint64_t charId) { return *shards_[shard_of(charId, shards_.size())]; }
    Connection* connection(Shard& sh);
    bool attempt_save(Shard& sh, uint64_t charId);
    void flush_saves(Shard& sh, TimePoint now, size_t max);
    // Worker thread: runs work against the DB; false (DB unavailable) on any error.
    bool run_job(Shard& sh, const std::function<void(CharRepo&)>& work, std::optional<uint64_t> charId);

    WorkerOptions opts_;
    Post post_;

    // ponytail: one mutex for the shared queue and every shard's queue and pending map; held only for map/queue
    // updates, never across a query. Per-shard locks if profiling ever shows contention here.
    mutable std::mutex mu_;
    std::deque<Job> jobs_; // query: any worker
    std::vector<std::unique_ptr<Shard>> shards_;
    bool stopping_ = false;
    TimePoint deadline_{};
    uint64_t savesOk_ = 0, saveFailures_ = 0;
};

} // namespace chthonic::db
