#include "db_worker.h"
#include "../core/log.h"

#include <algorithm>
#include <cstdint>

namespace chthonic::db {

void queue_save(PendingSaves& pending, const CharSave& s, TimePoint now) {
    auto& p = pending[s.charId];
    p.save = s; // latest save wins; keeps the retry schedule of an older pending save
    if (p.attempts == 0) p.due = now;
}

std::vector<uint64_t> due_saves(const PendingSaves& pending, TimePoint now, size_t max) {
    std::vector<std::pair<TimePoint, uint64_t>> due;
    for (const auto& [id, p] : pending)
        if (p.due <= now) due.emplace_back(p.due, id);
    const size_t n = std::min(max, due.size());
    std::partial_sort(due.begin(), due.begin() + static_cast<std::ptrdiff_t>(n), due.end());
    std::vector<uint64_t> ids;
    for (size_t i = 0; i < n; ++i) ids.push_back(due[i].second);
    return ids;
}

DbWorker::DbWorker(WorkerOptions opts, Post post) : opts_(std::move(opts)), post_(std::move(post)) {
    const uint32_t n = std::max<uint32_t>(1, opts_.poolSize);
    for (uint32_t i = 0; i < n; ++i) shards_.push_back(std::make_unique<Shard>());
    for (auto& sh : shards_) sh->thread = std::thread([this, s = sh.get()] { run(*s); });
    log::info("database pool started", {{"poolSize", n}});
}

DbWorker::~DbWorker() {
    if (!shards_.empty() && shards_[0]->thread.joinable()) stop(SteadyClock::now());
}

void DbWorker::enqueue(std::optional<uint64_t> charId, Job job) {
    std::lock_guard lk(mu_);
    if (stopping_) return; // shutting down: no new requests
    if (charId) {
        Shard& sh = shard(*charId);
        sh.jobs.push_back(std::move(job));
        sh.cv.notify_one();
        return;
    }
    jobs_.push_back(std::move(job));
    // Wake one idle worker; busy ones check the shared queue after their current job.
    for (auto& sh : shards_)
        if (sh->idle) {
            sh->idle = false;
            sh->cv.notify_one();
            break;
        }
}

void DbWorker::save(const CharSave& s) {
    std::lock_guard lk(mu_);
    Shard& sh = shard(s.charId);
    queue_save(sh.pending, s, SteadyClock::now());
    sh.cv.notify_one();
}

size_t DbWorker::stop(TimePoint deadline) {
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
        deadline_ = deadline;
        for (auto& sh : shards_) sh->cv.notify_one();
    }
    for (auto& sh : shards_)
        if (sh->thread.joinable()) sh->thread.join();
    return stats().pending;
}

SaveStats DbWorker::stats() const {
    std::lock_guard lk(mu_);
    size_t pending = 0;
    for (const auto& sh : shards_) pending += sh->pending.size();
    return {savesOk_, saveFailures_, pending};
}

Connection* DbWorker::connection(Shard& sh) {
    if (sh.conn) return sh.conn.get();
    const auto now = SteadyClock::now();
    // DB down: fail requests fast instead of waiting out a connect timeout for each one.
    if (sh.lastConnectFailure && now - *sh.lastConnectFailure < Millis(1000)) return nullptr;
    try {
        sh.conn = std::make_unique<Connection>(opts_.connect);
        if (sh.lastConnectFailure) log::info("database reconnected");
        sh.lastConnectFailure.reset();
    } catch (const DbError& e) {
        if (!sh.lastConnectFailure) log::warn("database unavailable", {{"error", e.what()}, {"code", e.code}});
        sh.lastConnectFailure = now;
    }
    return sh.conn.get();
}

bool DbWorker::attempt_save(Shard& sh, uint64_t charId) {
    CharSave s;
    {
        std::lock_guard lk(mu_);
        auto it = sh.pending.find(charId);
        if (it == sh.pending.end()) return true;
        s = it->second.save;
    }
    bool ok = false;
    std::string error = "database unavailable";
    if (Connection* c = connection(sh)) {
        try {
            CharRepo(*c, opts_.realmId).save(s);
            ok = true;
        } catch (const DbError& e) {
            error = e.what();
            sh.conn.reset();
        }
    }
    std::lock_guard lk(mu_);
    auto it = sh.pending.find(charId);
    if (ok) {
        ++savesOk_;
        // A newer save may have been queued while this one was written; keep it.
        const auto& cur = it->second.save;
        if (cur.pos == s.pos && cur.heading == s.heading && cur.components == s.components) sh.pending.erase(it);
        else {
            it->second.attempts = 0;
            it->second.due = SteadyClock::now();
        }
    } else {
        ++saveFailures_;
        PendingSave& p = it->second;
        p.attempts = std::min<uint32_t>(p.attempts + 1, 30);
        const auto backoff = std::min<int64_t>(opts_.retryMax.count(), opts_.retryMin.count() << std::min<uint32_t>(p.attempts - 1, 20));
        p.due = SteadyClock::now() + Millis(backoff);
        log::warn("save failed, will retry", {{"charId", charId}, {"attempt", p.attempts}, {"retryInMs", backoff},
                                              {"error", error}});
    }
    return ok;
}

void DbWorker::flush_saves(Shard& sh, TimePoint now, size_t max) {
    std::vector<uint64_t> due;
    {
        std::lock_guard lk(mu_);
        due = due_saves(sh.pending, now, max);
    }
    for (uint64_t id : due)
        if (!attempt_save(sh, id)) break; // DB is down; the rest wait for their own retry time
}

bool DbWorker::run_job(Shard& sh, const std::function<void(CharRepo&)>& work, std::optional<uint64_t> charId) {
    Connection* c = connection(sh);
    if (!c) return false;
    if (charId && !attempt_save(sh, *charId)) return false;
    c = connection(sh);
    if (!c) return false;
    try {
        CharRepo repo(*c, opts_.realmId);
        work(repo);
        return true;
    } catch (const DbError& e) {
        log::warn("database request failed", {{"error", e.what()}, {"code", e.code}});
        sh.conn.reset();
    } catch (const std::exception& e) {
        log::error("database request failed", {{"error", e.what()}});
    }
    return false;
}

void DbWorker::run(Shard& sh) {
    thread_init();
    for (;;) {
        {
            std::unique_lock lk(mu_);
            for (;;) {
                if (stopping_ || !sh.jobs.empty() || !jobs_.empty()) break;
                TimePoint next = TimePoint::max();
                for (const auto& [id, p] : sh.pending) next = std::min(next, p.due);
                if (next <= SteadyClock::now()) break;
                sh.idle = true;
                if (next == TimePoint::max()) sh.cv.wait(lk);
                else sh.cv.wait_until(lk, next);
                sh.idle = false;
            }
            if (stopping_) break;
        }
        // Only this worker's own due saves, at most kSavesPerJob, then one job: its own characters' loads first, then
        // the shared queue.
        flush_saves(sh, SteadyClock::now(), kSavesPerJob);
        Job job;
        {
            std::lock_guard lk(mu_);
            auto& q = sh.jobs.empty() ? jobs_ : sh.jobs;
            if (!q.empty()) {
                job = std::move(q.front());
                q.pop_front();
            }
        }
        if (job) job(sh);
    }

    // Shutdown: queued requests are dropped (their sessions are closing); every pending save of this worker is
    // retried until the deadline, ignoring backoff.
    for (;;) {
        TimePoint deadline;
        {
            std::lock_guard lk(mu_);
            if (sh.pending.empty()) break;
            deadline = deadline_;
        }
        if (SteadyClock::now() >= deadline) break;
        sh.lastConnectFailure.reset();
        flush_saves(sh, TimePoint::max(), SIZE_MAX);
        std::unique_lock lk(mu_);
        if (!sh.pending.empty()) sh.cv.wait_until(lk, std::min(deadline, SteadyClock::now() + Millis(200)));
    }
    sh.conn.reset();
    thread_end();
}

} // namespace chthonic::db
