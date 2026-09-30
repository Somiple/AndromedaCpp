#include "midi/io/mapped_file.h"

#include <format>
#include <fstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace andromeda::midi {

namespace {

#ifdef _WIN32
std::wstring widen(std::string_view path, unsigned int code_page) {
    if (path.empty()) {
        return {};
    }

    const int needed = MultiByteToWideChar(code_page, 0, path.data(), static_cast<int>(path.size()),
                                           nullptr, 0);
    if (needed <= 0) {
        return {};
    }

    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(code_page, 0, path.data(), static_cast<int>(path.size()), out.data(),
                        needed);
    return out;
}

HANDLE open_handle(std::string_view path) {
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;

    for (const unsigned int cp : {static_cast<unsigned int>(CP_UTF8),
                                  static_cast<unsigned int>(CP_ACP)}) {
        const std::wstring wide = widen(path, cp);
        if (wide.empty()) {
            continue;
        }

        HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, flags, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            return h;
        }
    }

    return INVALID_HANDLE_VALUE;
}
#endif

}

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_),
      size_(other.size_),
      file_handle_(other.file_handle_),
      mapping_handle_(other.mapping_handle_),
      fd_(other.fd_),
      fallback_(std::move(other.fallback_)) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.file_handle_ = nullptr;
    other.mapping_handle_ = nullptr;
    other.fd_ = -1;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
        file_handle_ = other.file_handle_;
        mapping_handle_ = other.mapping_handle_;
        fd_ = other.fd_;
        fallback_ = std::move(other.fallback_);
        other.data_ = nullptr;
        other.size_ = 0;
        other.file_handle_ = nullptr;
        other.mapping_handle_ = nullptr;
        other.fd_ = -1;
    }
    return *this;
}

void MappedFile::close() {
#ifdef _WIN32
    if (data_ != nullptr && fallback_.empty()) {
        UnmapViewOfFile(static_cast<LPCVOID>(data_));
    }
    if (mapping_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
    }
    if (file_handle_ != nullptr && file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
    }
#else
    if (data_ != nullptr && fallback_.empty()) {
        munmap(const_cast<std::uint8_t*>(data_), size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
#endif

    data_ = nullptr;
    size_ = 0;
    file_handle_ = nullptr;
    mapping_handle_ = nullptr;
    fd_ = -1;
    fallback_.clear();
    fallback_.shrink_to_fit();
}

std::expected<MappedFile, std::string> MappedFile::open(std::string_view path) {
    MappedFile out;

#ifdef _WIN32
    HANDLE file = open_handle(path);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(std::format("could not open {}", path));
    }
    out.file_handle_ = file;

    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file, &file_size) == 0) {
        return std::unexpected(std::format("could not size {}", path));
    }
    if (file_size.QuadPart <= 0) {
        return out;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        return std::unexpected(std::format("could not map {}", path));
    }
    out.mapping_handle_ = mapping;

    auto* view = static_cast<const std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (view == nullptr) {
        return std::unexpected(std::format("could not view {}", path));
    }

    out.data_ = view;
    out.size_ = static_cast<std::size_t>(file_size.QuadPart);
    return out;
#else
    const std::string p(path);
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(std::format("could not open {}", path));
    }
    out.fd_ = fd;

    struct stat st {};
    if (fstat(fd, &st) != 0) {
        return std::unexpected(std::format("could not size {}", path));
    }
    if (st.st_size == 0) {
        return out;
    }

    void* view = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        return std::unexpected(std::format("could not map {}", path));
    }

    out.data_ = static_cast<const std::uint8_t*>(view);
    out.size_ = static_cast<std::size_t>(st.st_size);
    return out;
#endif
}

void MappedFile::prefetch() const {
    if (data_ == nullptr || !fallback_.empty()) {
        return;
    }

#ifdef _WIN32
    struct WIN32_MEMORY_RANGE_ENTRY_LOCAL {
        PVOID VirtualAddress;
        SIZE_T NumberOfBytes;
    };
    using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, WIN32_MEMORY_RANGE_ENTRY_LOCAL*, ULONG);

    static PrefetchFn prefetch_fn = [] {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        return k32 != nullptr
                   ? reinterpret_cast<PrefetchFn>(
                         reinterpret_cast<void*>(GetProcAddress(k32, "PrefetchVirtualMemory")))
                   : nullptr;
    }();

    if (prefetch_fn == nullptr) {
        return;
    }

    WIN32_MEMORY_RANGE_ENTRY_LOCAL range{const_cast<std::uint8_t*>(data_), size_};
    prefetch_fn(GetCurrentProcess(), 1, &range, 0);
#else
    madvise(const_cast<std::uint8_t*>(data_), size_, MADV_WILLNEED);
#endif
}

}
