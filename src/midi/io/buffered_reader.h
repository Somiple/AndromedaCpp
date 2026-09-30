#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace andromeda::midi {

struct SharedFile {
    std::mutex mutex;
    std::ifstream stream;
};

using SharedFilePtr = std::shared_ptr<SharedFile>;

class BufferedByteReader {
public:
    BufferedByteReader(SharedFilePtr stream, std::size_t start, std::size_t len,
                       std::size_t buf_size);

    void seek(std::ptrdiff_t offset, int origin);

    void read(std::span<std::uint8_t> dst, std::size_t size);

    std::uint8_t read_byte();

    std::pair<std::uint8_t, std::uint8_t> read_u8x2();
    std::tuple<std::uint8_t, std::uint8_t, std::uint8_t> read_u8x3();

    void skip_bytes(std::size_t size);

    [[nodiscard]] std::size_t pos() const { return pos_; }

    SharedFilePtr file_stream;

private:
    void update_buffer();

    std::size_t start_;
    std::size_t len_;
    std::size_t buf_size_;
    std::size_t pos_;
    std::size_t buf_start_;
    std::size_t buf_pos_;
    std::vector<std::uint8_t> buf_;
};

}
