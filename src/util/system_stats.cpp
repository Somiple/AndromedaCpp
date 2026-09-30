#include "util/system_stats.h"

#include <array>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace andromeda::util {

namespace {

#ifdef _WIN32
std::uint64_t filetime_to_u64(const FILETIME& ft) {
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return v.QuadPart;
}

std::uint64_t process_cpu_time_100ns() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return filetime_to_u64(kernel) + filetime_to_u64(user);
}

std::uint64_t wall_time_100ns() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return filetime_to_u64(ft);
}
#endif

}

std::string MemoryUnits::to_string() const {
    const char* unit_name = "bytes";

    switch (unit) {
    case Unit::Bytes:     unit_name = "bytes"; break;
    case Unit::KiloBytes: unit_name = "KB"; break;
    case Unit::MegaBytes: unit_name = "MB"; break;
    case Unit::GigaBytes: unit_name = "GB"; break;
    case Unit::TeraBytes: unit_name = "TB"; break;
    }

    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%.2f %s", value, unit_name);
    return std::string(buf.data());
}

SystemStats::SystemStats() {
    cpu_count_ = std::thread::hardware_concurrency();
    if (cpu_count_ == 0) {
        cpu_count_ = 1;
    }

#ifdef _WIN32
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        total_memory = mem.ullTotalPhys;
    }

    last_proc_time_100ns_ = process_cpu_time_100ns();
    last_wall_time_100ns_ = wall_time_100ns();
#endif
}

void SystemStats::update() {
    if (refresh_timer_.elapsed() < 1.0f) {
        return;
    }
    refresh_timer_.start();

#ifdef _WIN32
    const std::uint64_t proc_now = process_cpu_time_100ns();
    const std::uint64_t wall_now = wall_time_100ns();

    const std::uint64_t proc_delta = proc_now - last_proc_time_100ns_;
    const std::uint64_t wall_delta = wall_now - last_wall_time_100ns_;

    last_proc_time_100ns_ = proc_now;
    last_wall_time_100ns_ = wall_now;

    if (wall_delta > 0) {
        const double busy = static_cast<double>(proc_delta) /
                            (static_cast<double>(wall_delta) * cpu_count_);
        cpu_usage = static_cast<float>(busy * 100.0);
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        const std::uint64_t memory = pmc.WorkingSetSize;

        if (memory >= 1000000000000ULL) {
            memory_usage = {MemoryUnits::Unit::TeraBytes,
                            static_cast<double>(memory) / 1000000000000.0};
        } else if (memory >= 1000000000ULL) {
            memory_usage = {MemoryUnits::Unit::GigaBytes,
                            static_cast<double>(memory) / 1000000000.0};
        } else if (memory >= 1000000ULL) {
            memory_usage = {MemoryUnits::Unit::MegaBytes,
                            static_cast<double>(memory) / 1000000.0};
        } else if (memory >= 1000ULL) {
            memory_usage = {MemoryUnits::Unit::KiloBytes,
                            static_cast<double>(memory) / 1000.0};
        } else {
            memory_usage = {MemoryUnits::Unit::Bytes, static_cast<double>(memory)};
        }

        if (total_memory > 0) {
            memory_pers = static_cast<float>(static_cast<double>(memory) / static_cast<double>(total_memory)) * 100.0f;
        }
    }
#endif
}

}
