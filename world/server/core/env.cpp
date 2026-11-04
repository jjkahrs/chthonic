#include "env.h"

#include <algorithm>
#include <cstdlib>

namespace chthonic {

std::optional<std::string> process_env(const char* name) {
#ifdef _MSC_VER
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) return std::nullopt;
    std::string s(buf);
    std::free(buf);
    return s;
#else
    const char* v = std::getenv(name);
    return v ? std::optional<std::string>(v) : std::nullopt;
#endif
}

void EnvLoader::list(const char* name, std::vector<std::string>& out) {
    auto v = env_(name);
    if (!v) return;
    for (size_t start = 0; start <= v->size();) {
        const size_t end = std::min(v->find(',', start), v->size());
        std::string item = v->substr(start, end - start);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) out.push_back(std::move(item));
        start = end + 1;
    }
}

std::string EnvLoader::fmt(double d) {
    std::string s = std::to_string(d);
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s;
}

} // namespace chthonic
