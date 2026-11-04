// Applies migrations/NNN_name.sql in filename order, once each, tracked in schema_migrations.
// A MySQL named lock stops two instances from migrating at the same time (as auth/src/db/migrate.ts).
#pragma once
#include "mysql.h"

#include <string>
#include <vector>

namespace chthonic::db {

// Returns the files applied by this call. Throws DbError (or std::runtime_error for I/O) on failure. lockName: the
// MySQL named lock (server-wide), one per service.
std::vector<std::string> migrate(Connection& conn, const std::string& dir, const std::string& lockName = "chthonic_world_migrate");

// Splits a migration file into statements: ';' at the end of a line ends one; full-line "--" comments are dropped.
// ponytail: line-based split, so a ';' that ends a line inside a string literal would break it; use a real
// SQL tokenizer if migrations ever need that.
std::vector<std::string> split_statements(const std::string& sql);

} // namespace chthonic::db
