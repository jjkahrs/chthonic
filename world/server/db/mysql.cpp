#include "mysql.h"

#include <mysql/mysql.h>

#include <cstring>

namespace chthonic::db {
namespace {

struct StmtCloser {
    void operator()(MYSQL_STMT* s) const { mysql_stmt_close(s); }
};
struct ResultFree {
    void operator()(MYSQL_RES* r) const { mysql_free_result(r); }
};

[[noreturn]] void throw_stmt(MYSQL_STMT* s) { throw DbError(mysql_stmt_error(s), mysql_stmt_errno(s)); }

// Storage for one bound parameter; must outlive mysql_stmt_execute.
struct ParamSlot {
    int64_t i = 0;
    double d = 0;
    std::string s;
    unsigned long len = 0;
    bool isNull = false;
};

} // namespace

void library_init() { mysql_library_init(0, nullptr, nullptr); }
void thread_init() { mysql_thread_init(); }
void thread_end() { mysql_thread_end(); }

Connection::Connection(const ConnectOptions& o) {
    mysql_ = mysql_init(nullptr);
    if (!mysql_) throw DbError("mysql_init failed", 0);
    mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &o.connectTimeoutSec);
    mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT, &o.readWriteTimeoutSec);
    mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT, &o.readWriteTimeoutSec);
    mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(mysql_, o.host.c_str(), o.user.c_str(), o.password.c_str(), o.database.c_str(), o.port,
                            nullptr, 0)) {
        DbError e(mysql_error(mysql_), mysql_errno(mysql_));
        mysql_close(mysql_);
        mysql_ = nullptr;
        throw e;
    }
    // Session time zone UTC so DATETIME values never shift; the app writes all timestamps itself.
    exec("SET time_zone = '+00:00'");
}

Connection::~Connection() {
    if (mysql_) mysql_close(mysql_);
}

ExecResult Connection::exec(std::string_view sql, const std::vector<Param>& params) {
    std::unique_ptr<MYSQL_STMT, StmtCloser> stmt(mysql_stmt_init(mysql_));
    if (!stmt) throw DbError(mysql_error(mysql_), mysql_errno(mysql_));
    MYSQL_STMT* s = stmt.get();
    if (mysql_stmt_prepare(s, sql.data(), static_cast<unsigned long>(sql.size())) != 0) throw_stmt(s);
    if (mysql_stmt_param_count(s) != params.size()) throw DbError("parameter count mismatch", 0);

    std::vector<ParamSlot> slots(params.size());
    std::vector<MYSQL_BIND> binds(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        MYSQL_BIND& b = binds[i];
        std::memset(&b, 0, sizeof b);
        ParamSlot& slot = slots[i];
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    slot.isNull = true;
                    b.buffer_type = MYSQL_TYPE_NULL;
                } else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>) {
                    slot.i = static_cast<int64_t>(v);
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    b.buffer = &slot.i;
                    b.is_unsigned = std::is_same_v<T, uint64_t>;
                } else if constexpr (std::is_same_v<T, double>) {
                    slot.d = v;
                    b.buffer_type = MYSQL_TYPE_DOUBLE;
                    b.buffer = &slot.d;
                } else {
                    slot.s = v;
                    slot.len = static_cast<unsigned long>(slot.s.size());
                    b.buffer_type = MYSQL_TYPE_STRING;
                    b.buffer = slot.s.data();
                    b.buffer_length = slot.len;
                    b.length = &slot.len;
                }
                b.is_null = &slot.isNull;
            },
            params[i]);
    }
    if (!binds.empty() && mysql_stmt_bind_param(s, binds.data())) throw_stmt(s);
    if (mysql_stmt_execute(s) != 0) throw_stmt(s);

    ExecResult result;
    result.affectedRows = mysql_stmt_affected_rows(s);
    result.insertId = mysql_stmt_insert_id(s);

    std::unique_ptr<MYSQL_RES, ResultFree> meta(mysql_stmt_result_metadata(s));
    if (!meta) return result; // not a SELECT
    const unsigned cols = mysql_num_fields(meta.get());
    if (mysql_stmt_store_result(s) != 0) throw_stmt(s);

    std::vector<MYSQL_BIND> out(cols);
    std::vector<std::string> bufs(cols, std::string(64, '\0'));
    std::vector<unsigned long> lens(cols);
    std::vector<char> nulls(cols); // bool storage without vector<bool>
    std::vector<char> errs(cols);
    for (unsigned c = 0; c < cols; ++c) {
        std::memset(&out[c], 0, sizeof out[c]);
        out[c].buffer_type = MYSQL_TYPE_STRING;
        out[c].buffer = bufs[c].data();
        out[c].buffer_length = static_cast<unsigned long>(bufs[c].size());
        out[c].length = &lens[c];
        out[c].is_null = reinterpret_cast<bool*>(&nulls[c]);
        out[c].error = reinterpret_cast<bool*>(&errs[c]);
    }
    if (mysql_stmt_bind_result(s, out.data())) throw_stmt(s);

    for (;;) {
        const int rc = mysql_stmt_fetch(s);
        if (rc == MYSQL_NO_DATA) break;
        if (rc == 1) throw_stmt(s);
        Row row(cols);
        for (unsigned c = 0; c < cols; ++c) {
            if (nulls[c]) continue;
            if (lens[c] > bufs[c].size()) { // MYSQL_DATA_TRUNCATED: refetch this column with a big enough buffer
                std::string big(lens[c], '\0');
                MYSQL_BIND one;
                std::memset(&one, 0, sizeof one);
                one.buffer_type = MYSQL_TYPE_STRING;
                one.buffer = big.data();
                one.buffer_length = lens[c];
                if (mysql_stmt_fetch_column(s, &one, c, 0)) throw_stmt(s);
                row[c] = std::move(big);
            } else {
                row[c] = std::string(bufs[c].data(), lens[c]);
            }
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}

} // namespace chthonic::db
