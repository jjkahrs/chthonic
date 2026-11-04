#include "utc.h"

#include <cstdio>

namespace chthonic {
namespace {
std::string format(std::chrono::system_clock::time_point t, const char* fmt) {
    using namespace std::chrono;
    const auto ms = floor<milliseconds>(t);
    const auto day = floor<days>(ms);
    const year_month_day ymd{day};
    const hh_mm_ss hms{ms - day};
    char buf[40];
    std::snprintf(buf, sizeof buf, fmt, static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                  static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count()),
                  static_cast<int>(hms.subseconds().count()));
    return buf;
}
} // namespace

std::string utc_iso8601(std::chrono::system_clock::time_point t) {
    return format(t, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ");
}
std::string utc_mysql(std::chrono::system_clock::time_point t) {
    return format(t, "%04d-%02u-%02u %02d:%02d:%02d.%03d");
}

} // namespace chthonic
