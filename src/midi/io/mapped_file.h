#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace andromeda::midi {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // path may be utf-8 or the active code page; both are tried, keep both
    static std::expected<MappedFile, std::string> open(std::string_view path);

    [[nodiscard]] const std::uint8_t* data() const { return data_; }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool valid() const { return data_ != nullptr; }

    void prefetch() const;

private:
    void close();

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;

    void* file_handle_ = nullptr;
    void* mapping_handle_ = nullptr;
    int fd_ = -1;

    std::vector<std::uint8_t> fallback_;
};

}
