#include "util/debugger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

namespace andromeda::util {

namespace {

std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

std::optional<std::ofstream>& log_file() {
    static std::optional<std::ofstream> f;
    return f;
}

}

void Debugger::init_log_file(const std::string& path) {
    if constexpr (!DEBUGGING_ENABLED) {
        std::puts("Debugging has been disabled, did not make a log file.");
        return;
    }

    const std::filesystem::path p(path);

    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream file(p, std::ios::out | std::ios::app);

    std::lock_guard lock(log_mutex());
    log_file() = std::move(file);
}

void Debugger::write_file(std::string_view message) {
    std::lock_guard lock(log_mutex());

    if (auto& file = log_file(); file.has_value()) {
        *file << message << '\n';
        file->flush();
    }
}

std::string Debugger::timestamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif

    char buf[64];
    std::strftime(buf, sizeof(buf), "%m/%d/%Y @ %H:%M:%S", &local);
    return std::string(buf);
}

void Debugger::log(std::string_view message) {
    if constexpr (!DEBUGGING_ENABLED) {
        return;
    }

    const std::string msg = "[" + timestamp() + "] " + std::string(message);

    std::printf("%s\n", msg.c_str());
    write_file(msg);
}

void Debugger::log_notime(std::string_view message) {
    if constexpr (!DEBUGGING_ENABLED) {
        return;
    }

    std::printf("%.*s\n", static_cast<int>(message.size()), message.data());
    write_file(message);
}

void Debugger::log_warning(std::string_view message) {
    if constexpr (!DEBUGGING_ENABLED) {
        return;
    }

    const std::string time_fmt = timestamp();
    const std::string msg = "[" + time_fmt + "] " + std::string(message);

    std::printf("[%s] \x1b[33m(WARNING) %.*s\x1b[0m\n", time_fmt.c_str(),
                static_cast<int>(message.size()), message.data());
    write_file(msg);
}

void Debugger::log_error(std::string_view message) {
    if constexpr (!DEBUGGING_ENABLED) {
        return;
    }

    const std::string time_fmt = timestamp();
    const std::string msg = "[" + time_fmt + "] " + std::string(message);

    std::printf("[%s] \x1b[31m(ERROR) %.*s\x1b[0m\n", time_fmt.c_str(),
                static_cast<int>(message.size()), message.data());
    write_file(msg);
}

}
