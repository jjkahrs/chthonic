// Thin RAII wrapper over the MySQL C API. Prepared statements only: values are always bound, never spliced into SQL.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct MYSQL;

namespace chthonic::db {

struct DbError : std::runtime_error {
    DbError(const std::string& what, unsigned code) : std::runtime_error(what), code(code) {}
    unsigned code; // MySQL error number (0 = client-side)
};

constexpr unsigned kErDupEntry = 1062;

using Param = std::variant<std::nullptr_t, int64_t, uint64_t, double, std::string>;
using Row = std::vector<std::optional<std::string>>; // every column as text; NULL = nullopt

struct ExecResult {
    std::vector<Row> rows;
    uint64_t affectedRows = 0;
    uint64_t insertId = 0;
};

struct ConnectOptions {
    std::string host;
    uint16_t port = 3306;
    std::string user, password, database;
    unsigned connectTimeoutSec = 3;
    unsigned readWriteTimeoutSec = 5;
};

class Connection {
public:
    explicit Connection(const ConnectOptions& o); // throws DbError
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Prepares, binds, executes and (for result sets) fetches all rows. Throws DbError.
    ExecResult exec(std::string_view sql, const std::vector<Param>& params = {});

private:
    MYSQL* mysql_ = nullptr;
};

// Call once on the main thread before any other thread uses MySQL; worker threads call thread_init/thread_end.
void library_init();
void thread_init();
void thread_end();

} // namespace chthonic::db
