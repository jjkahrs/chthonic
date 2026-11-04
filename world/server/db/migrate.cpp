#include "migrate.h"
#include "../core/log.h"
#include "../core/utc.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace chthonic::db {
namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

std::vector<std::string> split_statements(const std::string& sql) {
    std::vector<std::string> out;
    std::string current, line;
    std::istringstream in(sql);
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t.rfind("--", 0) == 0) continue;
        current += line + "\n";
        if (t.back() == ';') {
            std::string stmt = trim(current);
            stmt.pop_back(); // the ';'
            out.push_back(stmt);
            current.clear();
        }
    }
    if (!trim(current).empty()) out.push_back(trim(current));
    return out;
}

std::vector<std::string> migrate(Connection& conn, const std::string& dir, const std::string& lockName) {
    auto lock = conn.exec("SELECT GET_LOCK(?, 60)", {lockName});
    if (lock.rows.empty() || lock.rows[0][0] != "1") throw std::runtime_error("timed out waiting for the migration lock");

    std::vector<std::string> applied;
    try {
        conn.exec("CREATE TABLE IF NOT EXISTS schema_migrations (name VARCHAR(255) NOT NULL PRIMARY KEY, "
                  "applied_at DATETIME(3) NOT NULL)");
        std::set<std::string> done;
        for (const auto& row : conn.exec("SELECT name FROM schema_migrations").rows) done.insert(*row[0]);

        std::vector<std::string> files;
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".sql") files.push_back(e.path().filename().string());
        std::sort(files.begin(), files.end());

        for (const auto& f : files) {
            if (done.count(f)) continue;
            // MySQL DDL auto-commits, so a file that fails halfway must be fixed by hand.
            for (const auto& stmt : split_statements(read_file(std::filesystem::path(dir) / f))) conn.exec(stmt);
            conn.exec("INSERT INTO schema_migrations (name, applied_at) VALUES (?, ?)",
                      {f, utc_mysql(std::chrono::system_clock::now())});
            log::info("migration applied", {{"file", f}});
            applied.push_back(f);
        }
    } catch (...) {
        conn.exec("SELECT RELEASE_LOCK(?)", {lockName});
        throw;
    }
    conn.exec("SELECT RELEASE_LOCK(?)", {lockName});
    return applied;
}

} // namespace chthonic::db
