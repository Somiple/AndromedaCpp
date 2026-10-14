#pragma once

// the file the exporter writes, and every call to the operating system that it takes.
// private to the exporter

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace andromeda::midi::exporter {

// views of a file begin on a multiple of this
inline constexpr std::uint64_t VIEW_ALIGN_BYTES = 64ull << 10;
inline constexpr std::size_t PAGE_BYTES = 4096;

// bytes on their way into the file
struct Piece {
    const std::uint8_t* source = nullptr;
    // where they go in the file
    std::uint64_t offset = 0;
    std::size_t length = 0;
    // the job they belong to
    std::uint32_t job = 0;
};

// the two ways bytes reach a view. a failing disk or share shows up as a fault on the view's
// memory: both catch it and return false. any thread may call them, on bytes no other
// thread writes to at the time

// view shows the file from view_offset on and holds every piece
[[nodiscard]] bool copy_guarded(const Piece* pieces, std::size_t count, std::uint8_t* view,
                                std::uint64_t view_offset);

// makes the pages of a view present and writable, with a write that changes nothing
[[nodiscard]] bool touch_guarded(std::uint8_t* begin, std::size_t length);

// true while the calling thread is inside one of the two above. a fault on a view passes
// every handler of the process before it reaches theirs; one that logs crashes can ask here
// whether the fault is one the exporter deals with
[[nodiscard]] bool writing_to_view() noexcept;

// the scan and the merge use avx2 in every build
[[nodiscard]] bool processor_has_avx2() noexcept;

// what the system has to say about an error code, in utf-8; empty for zero
[[nodiscard]] std::string error_text(std::uint32_t code);

// the output file, written through mapped views: threads can fill different parts at once,
// which plain writes do not allow. the final size is only known at the end, and the file is
// never made longer than it is known to get: first as long as every event at its shortest,
// then as far as bytes have a place. so nothing has to be cut off at the end, which a
// program that has the file open could keep from working. a file that cannot be mapped at
// all takes plain writes instead.
// create, restart, reserve, section_for and finish are for one caller at a time, the rest
// for any thread
class OutputFile {
public:
    OutputFile() = default;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    // a file that was created and not finished is emptied and deleted, as far as another
    // program that holds it open lets that happen
    ~OutputFile();

    // an existing file is replaced
    [[nodiscard]] bool create(const std::filesystem::path& path);

    // empties the file for another go at it: the same file, not a new one of its name, so
    // that links, permissions and other programs' handles stay good. no view may be left
    void restart();

    // maps the first bytes of the file, which makes it that long. a file that cannot be
    // mapped, or is on a share, stays as it is and takes plain writes for everything.
    // false when the volume has no room for that many bytes
    [[nodiscard]] bool reserve(std::uint64_t bytes);

    // what reserve mapped; zero for a file that takes plain writes
    [[nodiscard]] std::uint64_t reserved() const { return reserved_; }

    // a mapping that covers the file up to need. when the newest one is too short a new one
    // of the given size is made, which makes the file that long. null when the file cannot
    // grow that far. mappings stay valid until the file is finished or destroyed
    [[nodiscard]] void* section_for(std::uint64_t need, std::uint64_t size);

    // a view of [begin, end) of the file through a mapping that covers it; begin is a
    // multiple of the view alignment
    [[nodiscard]] std::uint8_t* map(void* section, std::uint64_t begin, std::uint64_t end);

    static void unmap(std::uint8_t* view);

    // copies the pieces, which follow each other in the file, through a view of their own
    [[nodiscard]] bool write_through(void* section, const Piece* pieces, std::size_t count);

    // writes the pieces with plain writes; the file grows as needed
    [[nodiscard]] bool write(const Piece* pieces, std::size_t count);

    // closes the file at the given size, which every byte has been written up to: the file
    // stays. no view may be left
    [[nodiscard]] bool finish(std::uint64_t size);

    // the system's code for the first call that failed, zero when none did or it had none
    [[nodiscard]] std::uint32_t error() const { return error_.load(std::memory_order_relaxed); }

private:
    void close_sections();
    void note_error();

    std::filesystem::path path_;
    // null while there is no file to clean up
    void* file_ = nullptr;
    // earlier mappings stay open: views of them may still be in use
    std::vector<void*> sections_;
    // the size of the newest mapping
    std::uint64_t section_bytes_ = 0;
    std::uint64_t reserved_ = 0;
    // the file may be this long from an earlier go that could not be emptied
    std::uint64_t left_over_ = 0;
    std::atomic<std::uint32_t> error_{0};
};

}
