#pragma once

#include <cstdint>
#include <string>

#include "util/timer.h"

namespace andromeda::util {

struct MemoryUnits {
    enum class Unit { Bytes, KiloBytes, MegaBytes, GigaBytes, TeraBytes };

    Unit unit = Unit::Bytes;
    double value = 0.0;

    [[nodiscard]] std::string to_string() const;
};

class SystemStats {
public:
    SystemStats();

    void update();

    float cpu_usage = 0.0f;
    MemoryUnits memory_usage{};
    float memory_pers = 0.0f;
    std::uint64_t total_memory = 0;

private:
    Timer refresh_timer_{};

    std::uint64_t last_proc_time_100ns_ = 0;
    std::uint64_t last_wall_time_100ns_ = 0;
    unsigned int cpu_count_ = 1;
};

}
