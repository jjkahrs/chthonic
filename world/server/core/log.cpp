// chthonic::log on Quill: the caller builds the JSON line and enqueues it; Quill's backend thread writes it to stdout.
#include "log.h"
#include "utc.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/Sink.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <mutex>

namespace chthonic::log {
namespace {
std::atomic<Level> g_level{Level::info};
constexpr const char* kNames[] = {"debug", "info", "warn", "error"};

// Per caller thread queue: grows to 64 MiB, then the caller blocks until the backend catches up; never drops.
struct QueueOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedBlocking;
    static constexpr size_t initial_queue_capacity = 128u * 1024u;
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr size_t unbounded_queue_max_capacity = 64u * 1024u * 1024u;
    static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};
using Frontend = quill::FrontendImpl<QueueOptions>;
using Logger = quill::LoggerImpl<QueueOptions>;

// One metadata per level; the message is the finished JSON line.
constexpr quill::MacroMetadata kLine[] = {
    {"log.cpp:debug", "", "{}", nullptr, quill::LogLevel::Debug, quill::MacroMetadata::Event::Log},
    {"log.cpp:info", "", "{}", nullptr, quill::LogLevel::Info, quill::MacroMetadata::Event::Log},
    {"log.cpp:warn", "", "{}", nullptr, quill::LogLevel::Warning, quill::MacroMetadata::Event::Log},
    {"log.cpp:error", "", "{}", nullptr, quill::LogLevel::Error, quill::MacroMetadata::Event::Log}};

std::string dump(const nlohmann::json& j) {
    // replace: invalid UTF-8 from a client never throws inside logging
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// {"ts":..,"level":..,"msg":.., then the other fields}; fields' ts/level/msg replace the given ones in place.
std::string json_line(nlohmann::json ts, nlohmann::json level, nlohmann::json msg, nlohmann::json fields) {
    const std::pair<const char*, nlohmann::json*> reserved[] = {{"ts", &ts}, {"level", &level}, {"msg", &msg}};
    if (fields.is_object())
        for (auto [key, slot] : reserved)
            if (auto it = fields.find(key); it != fields.end()) {
                *slot = std::move(*it);
                fields.erase(it);
            }
    std::string line = R"({"ts":)" + dump(ts) + R"(,"level":)" + dump(level) + R"(,"msg":)" + dump(msg);
    if (fields.is_object() && !fields.empty()) line += ',' + dump(fields).substr(1);
    else line += '}';
    return line;
}

Level level_of(quill::LogLevel l) {
    if (l <= quill::LogLevel::Debug) return Level::debug;
    if (l <= quill::LogLevel::Notice) return Level::info;
    return l == quill::LogLevel::Warning ? Level::warn : Level::error;
}

// Runs on the backend thread: stdout and the test sink. Debug/info lines stay stdio-buffered until the backend is
// idle; warn/error lines are flushed at once, so they survive a SIGKILL that comes after they were written.
class JsonStdoutSink final : public quill::Sink {
public:
    void set_test_sink(std::function<void(const std::string&)> sink) {
        std::lock_guard lock(m_);
        test_ = std::move(sink);
    }

protected:
    void write_log(const quill::MacroMetadata* meta, uint64_t ts, std::string_view, std::string_view,
                   const std::string&, std::string_view, quill::LogLevel level, std::string_view, std::string_view,
                   const std::vector<std::pair<std::string, std::string>>*, std::string_view message,
                   std::string_view) override {
        std::string wrapped;
        if (meta < std::begin(kLine) || meta >= std::end(kLine)) { // Quill's own lines (crash handler): same JSON shape
            const auto t = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(ts)));
            wrapped = json_line(utc_iso8601(t), kNames[static_cast<int>(level_of(level))], message, nullptr);
            message = wrapped;
        }
        std::fwrite(message.data(), 1, message.size(), stdout);
        std::fputc('\n', stdout);
        if (level >= quill::LogLevel::Warning) std::fflush(stdout);
        std::lock_guard lock(m_);
        if (test_) test_(std::string(message));
    }
    void flush_sink() override { std::fflush(stdout); }

private:
    std::mutex m_;
    std::function<void(const std::string&)> test_;
};

struct Backend {
    std::shared_ptr<JsonStdoutSink> sink;
    Logger* logger = nullptr;
};

// Started on the first line (or flush/set_test_sink), so startup and config errors log too. Quill's atexit handler
// writes everything still queued when the process exits normally.
Backend& backend() {
    static Backend b = [] {
        quill::BackendOptions options;
        options.check_printable_char = {}; // lines are already JSON-escaped UTF-8
        options.thread_name = "chthonic_log";
        options.error_notifier = [](const std::string& e) { // ponytail: queue growth notices are dropped
            if (e.find("Quill INFO") == std::string::npos) std::fprintf(stderr, "%s\n", e.c_str());
        };
        quill::SignalHandlerOptions signals;
        signals.logger_name = "chthonic";
        // Crash signals only. Quill's SIGINT/SIGTERM handler std::exit(0)s, skipping main's graceful shutdown (and
        // the save flush); the backend can start after main's asio::signal_set, so it would replace that too.
#ifdef _WIN32
        // Windows: crashes go through the process-wide SEH filter start() installs. The CRT's SIGSEGV/SIGFPE/SIGILL
        // handlers are per thread, so only the process-wide SIGABRT (abort()) is hooked here.
        signals.catchable_signals = {SIGABRT};
#else
        signals.catchable_signals = {SIGABRT, SIGFPE, SIGILL, SIGSEGV};
#endif
        quill::Backend::start<QueueOptions>(options, signals);
#ifdef _WIN32
        // start() also hooks Ctrl+C, which would std::exit past the graceful shutdown: unhook it.
        SetConsoleCtrlHandler(quill::detail::on_console_signal<QueueOptions>, FALSE);
        quill::init_signal_handler<QueueOptions>(signals.catchable_signals);
#endif
        Backend made;
        made.sink = std::static_pointer_cast<JsonStdoutSink>(Frontend::create_or_get_sink<JsonStdoutSink>("chthonic_stdout"));
        made.logger = Frontend::create_or_get_logger(
            "chthonic", made.sink, quill::PatternFormatterOptions{"%(message)"});
        made.logger->set_log_level(quill::LogLevel::Debug); // g_level filters before anything is queued
        return made;
    }();
    return b;
}
} // namespace

void set_level(Level l) { g_level = l; }

void set_test_sink(std::function<void(const std::string& line)> sink) { backend().sink->set_test_sink(std::move(sink)); }

void flush() { backend().logger->flush_log(); }

bool parse_level(std::string_view s, Level& out) {
    for (int i = 0; i < 4; ++i)
        if (s == kNames[i]) { out = static_cast<Level>(i); return true; }
    return false;
}

void write(Level l, std::string_view msg, nlohmann::json fields) {
    if (l < g_level.load()) return;
    // C++ callers may still override ts/level/msg through fields.
    const std::string line =
        json_line(utc_iso8601(std::chrono::system_clock::now()), kNames[static_cast<int>(l)], msg, std::move(fields));
    backend().logger->log_statement<false>(&kLine[static_cast<int>(l)], std::string_view(line));
}

} // namespace chthonic::log
