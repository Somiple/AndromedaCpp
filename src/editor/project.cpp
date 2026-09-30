#include "editor/project.h"

#include <array>

#include "editor/project/project_manager.h"

namespace andromeda::editor {

ProjectWriter::ProjectWriter(const ProjectManager& project_manager,
                             const std::filesystem::path& path)
    : stream_(path, std::ios::binary), project_manager_(project_manager) {}

std::expected<void, std::string> ProjectWriter::write_header() {
    static constexpr std::uint8_t MAGIC[] = {'A', 'n', 'H', 'd'};
    if (auto r = write(MAGIC); !r) {
        return r;
    }

    {
        const ProjectData& project_data = project_manager_.get_project_data();
        const ProjectInfo& project_info = project_manager_.get_project_info();

        const std::uint16_t ppq = project_data.ppq;
        const auto ppq_bytes = u16_to_bytes(ppq);
        buffer_.insert(buffer_.end(), ppq_bytes.begin(), ppq_bytes.end());

        const auto [name_len, name_bytes] = text_to_bytes(project_info.name);
        const auto name_len_bytes = u32_to_bytes(name_len);
        buffer_.insert(buffer_.end(), name_len_bytes.begin(), name_len_bytes.end());
        buffer_.insert(buffer_.end(), name_bytes.begin(), name_bytes.end());

        const auto [author_len, author_bytes] = text_to_bytes(project_info.author);
        const auto author_len_bytes = u32_to_bytes(author_len);
        buffer_.insert(buffer_.end(), author_len_bytes.begin(), author_len_bytes.end());
        buffer_.insert(buffer_.end(), author_bytes.begin(), author_bytes.end());

        const auto [desc_len, desc_bytes] = text_to_bytes(project_info.description);
        const auto desc_len_bytes = u32_to_bytes(desc_len);
        buffer_.insert(buffer_.end(), desc_len_bytes.begin(), desc_len_bytes.end());
        buffer_.insert(buffer_.end(), desc_bytes.begin(), desc_bytes.end());
    }

    const auto buf_len = u32_to_bytes(static_cast<std::uint32_t>(buffer_.size()));
    if (auto r = write(buf_len); !r) {
        return r;
    }
    if (auto r = flush_buffer(); !r) {
        return r;
    }

    return {};
}

std::expected<void, std::string> ProjectWriter::finalize() {
    stream_.flush();
    if (!stream_) {
        return std::unexpected("flush failed");
    }
    return {};
}

std::array<std::uint8_t, 2> ProjectWriter::u16_to_bytes(std::uint16_t num) {
    return {static_cast<std::uint8_t>((num & 0xFF00) >> 8),
            static_cast<std::uint8_t>(num & 0xFF)};
}

std::array<std::uint8_t, 4> ProjectWriter::u32_to_bytes(std::uint32_t num) {
    return {static_cast<std::uint8_t>((num & 0xFF000000) >> 24),
            static_cast<std::uint8_t>((num & 0xFF0000) >> 16),
            static_cast<std::uint8_t>((num & 0xFF00) >> 8),
            static_cast<std::uint8_t>(num & 0xFF)};
}

std::expected<void, std::string> ProjectWriter::flush_buffer() {
    // fixed rust bug: the buffer was never cleared, so a second flush rewrote it
    auto result = write(buffer_);
    buffer_.clear();
    return result;
}

std::expected<void, std::string> ProjectWriter::write(std::span<const std::uint8_t> data) {
    stream_.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    if (!stream_) {
        return std::unexpected("write failed");
    }
    return {};
}

std::pair<std::uint32_t, std::vector<std::uint8_t>> ProjectWriter::text_to_bytes(
    std::string_view text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() * 2);

    const auto push_unit = [&bytes](std::uint16_t unit) {
        bytes.push_back(static_cast<std::uint8_t>(unit & 0xFF));
        bytes.push_back(static_cast<std::uint8_t>(unit >> 8));
    };

    for (std::size_t i = 0; i < text.size();) {
        const std::uint8_t b0 = static_cast<std::uint8_t>(text[i]);
        std::uint32_t cp = 0;
        std::size_t extra = 0;

        if (b0 < 0x80) {
            cp = b0;
            extra = 0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1Fu;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0Fu;
            extra = 2;
        } else {
            cp = b0 & 0x07u;
            extra = 3;
        }

        if (i + extra >= text.size()) {
            break;
        }

        for (std::size_t k = 1; k <= extra; ++k) {
            cp = (cp << 6) | (static_cast<std::uint8_t>(text[i + k]) & 0x3Fu);
        }
        i += extra + 1;

        if (cp >= 0x10000) {
            cp -= 0x10000;
            push_unit(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            push_unit(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            push_unit(static_cast<std::uint16_t>(cp));
        }
    }

    return {static_cast<std::uint32_t>(bytes.size()), std::move(bytes)};
}

}
