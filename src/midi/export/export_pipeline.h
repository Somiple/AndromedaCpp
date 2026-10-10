#pragma once

// the threads of the exporter: they turn a plan into the bytes of the file. private to the
// exporter

#include <cstdint>

#include "midi/export/export_file.h"
#include "midi/export/export_plan.h"

namespace andromeda::midi::exporter {

// file_failed: the file did not take the bytes; it knows the system's reason.
// needs_prepare: the input was found out of order, or with a note that ends past the last
// tick, while the plan was made for input that is neither
enum class Outcome { done, failed, file_failed, needs_prepare };

struct PipelineResult {
    Outcome outcome = Outcome::failed;
    // a literal; empty when done
    const char* message = "";
    // of the whole file, when done
    std::uint64_t size = 0;
};

// encodes every job of the plan and writes the bytes to the file, whose mapped part has to
// be reserved by then, with the calling thread as one of the workers. every thread it starts
// has ended and every view is gone when it returns; it does not throw
[[nodiscard]] PipelineResult run_pipeline(const Plan& plan, OutputFile& file,
                                          unsigned workers) noexcept;

}
