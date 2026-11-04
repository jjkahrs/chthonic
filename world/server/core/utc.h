// UTC timestamp formatting, written by the application (never NOW() in SQL).
#pragma once
#include <chrono>
#include <string>

namespace chthonic {

// "2026-10-15T12:34:56.789Z"
std::string utc_iso8601(std::chrono::system_clock::time_point t);
// "2026-10-15 12:34:56.789" for MySQL DATETIME(3)
std::string utc_mysql(std::chrono::system_clock::time_point t);

} // namespace chthonic
