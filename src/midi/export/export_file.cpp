#include "midi/export/export_file.h"

#include <algorithm>
#include <cstring>
#include <iterator>

#ifndef _WIN32
#error the exporter writes its file through the windows api
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <intrin.h>
#include <windows.h>

namespace andromeda::midi::exporter {

namespace {

// one plain write takes no more than this: its length is a 32 bit number
constexpr std::size_t MAX_WRITE_BYTES = std::size_t{1} << 30;

thread_local bool in_view_write = false;

[[nodiscard]] bool set_size(HANDLE file, std::uint64_t size) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(file, FileEndOfFileInfo, &info, sizeof(info)) != 0;
}

}

// no objects with destructors in these two: structured exception handling does not allow them

bool copy_guarded(const Piece* pieces, std::size_t count, std::uint8_t* view,
                  std::uint64_t view_offset) {
    bool ok = true;
    in_view_write = true;
    __try {
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(view + (pieces[i].offset - view_offset), pieces[i].source,
                        pieces[i].length);
        }
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                              : EXCEPTION_CONTINUE_SEARCH) {
        ok = false;
    }
    in_view_write = false;
    return ok;
}

bool touch_guarded(std::uint8_t* begin, std::size_t length) {
    bool ok = true;
    in_view_write = true;
    __try {
        for (std::size_t at = 0; at < length; at += PAGE_BYTES) {
            // a write the compiler cannot drop
            _InterlockedOr8(reinterpret_cast<char*>(begin) + at, 0);
        }
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                              : EXCEPTION_CONTINUE_SEARCH) {
        ok = false;
    }
    in_view_write = false;
    return ok;
}

bool writing_to_view() noexcept { return in_view_write; }

bool processor_has_avx2() noexcept {
    return IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != 0;
}

std::string error_text(std::uint32_t code) {
    switch (code) {
    case 0:
        return {};
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
        return "the file is open in another program";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return "the disk is full";
    default:
        break;
    }
    wchar_t wide[512];
    DWORD length =
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                       wide, static_cast<DWORD>(std::size(wide)), nullptr);
    // the system ends its messages with a full stop and a line break
    while (length > 0 && (wide[length - 1] == L'\r' || wide[length - 1] == L'\n' ||
                          wide[length - 1] == L'.' || wide[length - 1] == L' ')) {
        --length;
    }
    char text[3 * std::size(wide)];
    const int bytes = length > 0
                          ? WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), text,
                                                static_cast<int>(sizeof(text)), nullptr, nullptr)
                          : 0;
    if (bytes <= 0) {
        return "error " + std::to_string(code);
    }
    return std::string(text, static_cast<std::size_t>(bytes));
}

OutputFile::~OutputFile() {
    close_sections();
    if (file_ != nullptr) {
        // emptied first: should the delete not work, what stays is at least not half a file
        (void)set_size(file_, 0);
        CloseHandle(file_);
        DeleteFileW(path_.c_str());
    }
}

bool OutputFile::create(const std::filesystem::path& path) {
    path_ = path;
    // shared for reading and writing like the stream based writer before this one: a program
    // that keeps the old file open must not make the export fail
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        note_error();
        return false;
    }
    file_ = file;
    return true;
}

void OutputFile::restart() {
    close_sections();
    left_over_ = 0;
    error_.store(0, std::memory_order_relaxed);
    if (!set_size(file_, 0)) {
        // another program has the file mapped. the bytes of the earlier go get written over,
        // and only what lies behind the final size has to be cut off at the end
        LARGE_INTEGER size{};
        left_over_ = GetFileSizeEx(file_, &size) != 0 ? static_cast<std::uint64_t>(size.QuadPart)
                                                      : ~std::uint64_t{0};
    }
}

bool OutputFile::reserve(std::uint64_t bytes) {
    // bytes that go through a view of a file on a share can be lost without any error when
    // another program opens the file meanwhile, so such a file takes plain writes
    FILE_REMOTE_PROTOCOL_INFO remote{};
    if (GetFileInformationByHandleEx(file_, FileRemoteProtocolInfo, &remote, sizeof(remote)) != 0) {
        return true;
    }
    if (section_for(bytes, bytes) != nullptr) {
        reserved_ = bytes;
        return true;
    }
    // a volume without room for the shortest the file can get has no room for the file:
    // plain writes would only fill it up first
    const std::uint32_t code = error();
    if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
        return false;
    }
    // not a failure yet: should the plain writes fail too, their reason is the one to tell
    error_.store(0, std::memory_order_relaxed);
    return true;
}

void* OutputFile::section_for(std::uint64_t need, std::uint64_t size) {
    if (need > section_bytes_) {
        sections_.reserve(sections_.size() + 1);
        // a mapping larger than the file makes the file that large
        const HANDLE section =
            CreateFileMappingW(file_, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                               static_cast<DWORD>(size), nullptr);
        if (section == nullptr) {
            note_error();
            return nullptr;
        }
        sections_.push_back(section);
        section_bytes_ = size;
    }
    return sections_.back();
}

std::uint8_t* OutputFile::map(void* section, std::uint64_t begin, std::uint64_t end) {
    auto* const view = static_cast<std::uint8_t*>(
        MapViewOfFile(section, FILE_MAP_WRITE, static_cast<DWORD>(begin >> 32),
                      static_cast<DWORD>(begin), static_cast<std::size_t>(end - begin)));
    if (view == nullptr) {
        note_error();
    }
    return view;
}

void OutputFile::unmap(std::uint8_t* view) { UnmapViewOfFile(view); }

bool OutputFile::write_through(void* section, const Piece* pieces, std::size_t count) {
    const std::uint64_t begin = pieces[0].offset - pieces[0].offset % VIEW_ALIGN_BYTES;
    std::uint8_t* const view =
        map(section, begin, pieces[count - 1].offset + pieces[count - 1].length);
    if (view == nullptr) {
        return false;
    }
    const bool ok = copy_guarded(pieces, count, view, begin);
    UnmapViewOfFile(view);
    return ok;
}

bool OutputFile::write(const Piece* pieces, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* source = pieces[i].source;
        std::uint64_t offset = pieces[i].offset;
        std::size_t left = pieces[i].length;
        while (left > 0) {
            // the place is given with the write: no file pointer that threads would share
            OVERLAPPED at{};
            at.Offset = static_cast<DWORD>(offset);
            at.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD written = 0;
            if (WriteFile(file_, source, static_cast<DWORD>(std::min(left, MAX_WRITE_BYTES)),
                          &written, &at) == 0) {
                note_error();
                return false;
            }
            if (written == 0) {
                return false;
            }
            source += written;
            offset += written;
            left -= written;
        }
    }
    return true;
}

bool OutputFile::finish(std::uint64_t size) {
    // the mappings never reach past bytes that have a place, so this is only about left overs
    const std::uint64_t length = std::max(left_over_, section_bytes_);
    close_sections();
    if (length > size && !set_size(file_, size)) {
        note_error();
        return false;
    }
    CloseHandle(file_);
    file_ = nullptr;
    return true;
}

void OutputFile::close_sections() {
    for (void* const section : sections_) {
        CloseHandle(section);
    }
    sections_.clear();
    section_bytes_ = 0;
    reserved_ = 0;
}

void OutputFile::note_error() {
    std::uint32_t none = 0;
    error_.compare_exchange_strong(none, GetLastError(), std::memory_order_relaxed);
}

}
