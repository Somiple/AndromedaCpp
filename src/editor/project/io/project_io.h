#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"

namespace andromeda::editor {

class ProjectManager;

class ProjectWriter {
public:
    ProjectWriter(ProjectManager* project_manager, const std::filesystem::path& path);

    std::expected<void, std::string> write();
private:
    std::expected<void, std::string> write_header();
    std::expected<void, std::string> write_project_meta();
    std::expected<void, std::string> write_track_meta_chunk();
    std::expected<void, std::string> write_note_chunk();
    std::expected<void, std::string> write_channel_event_chunk();
    std::expected<void, std::string> write_global_event_chunk();
    std::expected<void, std::string> finalize();

    static std::array<std::uint8_t, 2> u16_to_bytes(std::uint16_t num);
    static std::array<std::uint8_t, 4> u32_to_bytes(std::uint32_t num);

    std::expected<void, std::string> flush_buffer();
    std::expected<void, std::string> write(std::span<const std::uint8_t> data);

    // Compresses `raw` and writes: u32 raw size, u32 compressed size, compressed bytes.
    std::expected<void, std::string> write_block(std::span<const std::uint8_t> raw);

    static std::pair<std::uint32_t, std::vector<std::uint8_t>> text_to_bytes(std::string_view text);

    std::ofstream stream_;
    ProjectManager* project_manager_;

    std::vector<std::uint8_t> buffer_;
};

class ProjectReader {
public:
    ProjectReader(ProjectManager* project_manager, const std::filesystem::path& path);
    std::expected<void, std::string> load();

private:
    struct ProjectHeader {
        std::uint16_t version;
        std::uint16_t track_count;
        std::uint16_t ppq;
    };
    std::expected<ProjectHeader, std::string> parse_header();

    struct ProjectMeta {
        std::string name;
        std::string author;
        std::string description;
    };
    std::expected<ProjectMeta, std::string> parse_meta();

    struct TrackMeta {
        std::string name;
        std::uint8_t port = 0;
        bool muted = false;
        std::optional<std::uint32_t> note_count;
        std::optional<std::uint32_t> channel_event_count;
        std::optional<std::array<std::uint8_t, 3>> color_code;
    };

    std::expected<std::vector<TrackMeta>, std::string> parse_track_metas(std::size_t count);

    struct ProjectEvents {
        std::vector<std::vector<midi::Note>> notes;                  // one entry per track
        std::vector<std::vector<midi::ChannelEvent>> channel_events; // one entry per track
        std::vector<midi::MetaEvent> global_events;
    };
    std::expected<ProjectEvents, std::string> parse_events(const std::vector<TrackMeta>& track_metas);

    // Reads one block written by ProjectWriter::write_block and returns the decompressed bytes.
    std::expected<std::vector<std::uint8_t>, std::string> read_block();

    std::ifstream stream_;
    ProjectManager* project_manager_;

    std::expected<std::uint32_t, std::string> read_u32();
    std::expected<std::uint16_t, std::string> read_u16();

    std::expected<std::string, std::string> read_text(std::size_t count);
};

}
