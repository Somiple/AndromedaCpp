#include "midi/io/buffered_reader.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace andromeda::midi {

BufferedByteReader::BufferedByteReader(SharedFilePtr stream, std::size_t start,
                                       std::size_t len, std::size_t buf_size)
    : file_stream(std::move(stream)),
      start_(start),
      len_(len),
      buf_size_(buf_size),
      pos_(start),
      buf_start_(0),
      buf_pos_(0) {

    std::size_t buffer_length = buf_size;
    if (buffer_length > len) {
        buffer_length = len;
    }

    buf_.assign(buffer_length, 0);

    update_buffer();
}

void BufferedByteReader::update_buffer() {
    std::size_t read = buf_size_;

    if ((pos_ + read) > (start_ + len_)) {
        read = start_ + len_ - pos_;
    }

    if (read == 0 && buf_size_ != 0) {
        throw std::runtime_error("outside buffer");
    }

    // fixed rust bug: filled the whole buffer, reading past the end of the chunk
    {
        std::lock_guard lock(file_stream->mutex);
        auto& strm = file_stream->stream;
        strm.clear();
        strm.seekg(static_cast<std::streamoff>(pos_), std::ios::beg);
        strm.read(reinterpret_cast<char*>(buf_.data()), static_cast<std::streamsize>(read));
    }

    buf_start_ = pos_;
    buf_pos_ = 0;
}

void BufferedByteReader::seek(std::ptrdiff_t offset, int origin) {
    std::ptrdiff_t real_offs = offset;
    if (origin == 0) {
        real_offs += static_cast<std::ptrdiff_t>(start_);
    } else {
        real_offs += static_cast<std::ptrdiff_t>(pos_);
    }

    if (real_offs < static_cast<std::ptrdiff_t>(start_)) {
        throw std::runtime_error("seek before start");
    }
    if (real_offs > static_cast<std::ptrdiff_t>(start_ + len_)) {
        throw std::runtime_error("seek past end");
    }

    pos_ = static_cast<std::size_t>(real_offs);

    if (static_cast<std::ptrdiff_t>(buf_start_) <= real_offs &&
        real_offs < static_cast<std::ptrdiff_t>(buf_start_ + buf_size_)) {
        buf_pos_ = pos_ - buf_start_;
        return;
    }

    update_buffer();
}

void BufferedByteReader::read(std::span<std::uint8_t> dst, std::size_t size) {
    if (pos_ + size > start_ + len_) {
        throw std::runtime_error(std::format("read past end | requested read: {}, buff size: {}",
                                             pos_ + size, start_ + len_));
    }
    if (size > buf_size_) {
        throw std::runtime_error("unimplemented; read size larger than buffer size");
    }

    if (buf_start_ + buf_pos_ + size > buf_start_ + buf_size_) {
        update_buffer();
    }

    // fixed rust bug: sliced past the buffer when the track is shorter than it
    if (buf_pos_ + size > buf_.size()) {
        throw std::runtime_error("read past end of buffer");
    }

    std::copy_n(buf_.begin() + static_cast<std::ptrdiff_t>(buf_pos_), size, dst.begin());
    pos_ += size;
    buf_pos_ += size;
}

std::uint8_t BufferedByteReader::read_byte() {
    std::uint8_t ret[1] = {0};
    read(ret, 1);
    return ret[0];
}

std::pair<std::uint8_t, std::uint8_t> BufferedByteReader::read_u8x2() {
    std::uint8_t ret[2] = {0, 0};
    read(ret, 2);
    return {ret[0], ret[1]};
}

std::tuple<std::uint8_t, std::uint8_t, std::uint8_t> BufferedByteReader::read_u8x3() {
    std::uint8_t ret[3] = {0, 0, 0};
    read(ret, 3);
    return {ret[0], ret[1], ret[2]};
}

void BufferedByteReader::skip_bytes(std::size_t size) {
    seek(static_cast<std::ptrdiff_t>(size), 1);
}

}
