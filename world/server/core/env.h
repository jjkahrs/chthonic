// Environment-variable config helpers shared by the services (world, chat): look a variable up, parse and range-check
// it, collect one error line per bad or missing variable.
#pragma once
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace chthonic {

using EnvLookup = std::function<std::optional<std::string>(const char* name)>;

std::optional<std::string> process_env(const char* name);

class EnvLoader {
public:
    explicit EnvLoader(const EnvLookup& env) : env_(env) {}
    std::vector<std::string> errors;

    // Unset or empty keeps out (or reports `required`).
    void str(const char* name, std::string& out, bool required = false) {
        auto v = env_(name);
        if (v && !v->empty()) out = *v;
        else if (required) errors.push_back(std::string(name) + ": required");
    }

    // A number in [min, max]; unset or empty keeps out. False when nothing was read.
    template <typename T>
    bool num(const char* name, T& out, double min, double max, bool required = false) {
        auto v = env_(name);
        if (!v || v->empty()) {
            if (required) errors.push_back(std::string(name) + ": required");
            return false;
        }
        T parsed{};
        const char* end = v->data() + v->size();
        auto [p, ec] = std::from_chars(v->data(), end, parsed);
        if (ec != std::errc() || p != end || parsed < min || parsed > max) {
            errors.push_back(std::string(name) + ": must be a number in [" + fmt(min) + ", " + fmt(max) + "], got \"" +
                             *v + "\"");
            return false;
        }
        out = parsed;
        return true;
    }

    // Comma-separated; spaces around names and empty entries are ignored.
    void list(const char* name, std::vector<std::string>& out);

private:
    static std::string fmt(double d);
    const EnvLookup& env_;
};

} // namespace chthonic
