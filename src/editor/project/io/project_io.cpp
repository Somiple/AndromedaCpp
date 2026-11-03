#include "project_io.h"

#include <array>

#include "event_codec.h"
#include "editor/project/project_manager.h"
#include <functional>

namespace {
// v1: header, meta, track meta.
// v2: v1 + compressed note / channel event / global event chunks.
inline constexpr std::uint16_t AND_FORMAT_VERSION = 2;

static constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return (std::uint32_t(a) << 24) |
           (std::uint32_t(b) << 16) |
           (std::uint32_t(c) << 8) |
            std::uint32_t(d);
}

template <std::unsigned_integral T>
inline std::vector<std::uint8_t> to_bytes(T value) {
    std::vector<std::uint8_t> result;
    result.reserve(sizeof(T));

    for (int i = 0; i < sizeof(T); i++) {
        result.push_back(
            static_cast<std::uint8_t>((value >> ((sizeof(T) - i - 1) << 3)) & 0xFF));
    }

    return result;
}

template <std::unsigned_integral T>
constexpr T from_bytes(std::span<const std::uint8_t> bytes) {
    T result = 0;

    for (const std::uint8_t byte : bytes) {
        result = static_cast<T>((result << 8) | static_cast<T>(byte));
    }

    return result;
}

std::vector<std::uint8_t> text_to_bytes(std::string_view text) {
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
        }
        else {
            push_unit(static_cast<std::uint16_t>(cp));
        }
    }

    return std::move(bytes);
}

std::expected<std::string, std::string> bytes_to_text(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    text.reserve(bytes.size());

    const auto read_unit = [&bytes](std::size_t offset) -> std::uint16_t {
        return static_cast<std::uint16_t>(bytes[offset]) |
            (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
        };

    const auto append_utf8 = [&text](std::uint32_t cp) {
        if (cp <= 0x7F) {
            text.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF) {
            text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF) {
            text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else {
            text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        };

    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        std::uint32_t cp = read_unit(i);

        // UTF-16 surrogate pair
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 3 >= bytes.size()) {
                return std::unexpected("Invalid UTF-16LE: incomplete surrogate pair");
            }

            const std::uint16_t low = read_unit(i + 2);

            if (low < 0xDC00 || low > 0xDFFF) {
                return std::unexpected("Invalid UTF-16LE: invalid low surrogate");
            }

            cp = 0x10000 +
                ((cp - 0xD800) << 10) +
                (low - 0xDC00);

            i += 2;
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return std::unexpected("Invalid UTF-16LE: unexpected low surrogate");
        }

        append_utf8(cp);
    }

    return text;
}

using PropertyId = std::array<char, 4>;

struct Property {
    PropertyId id;
    std::vector<std::uint8_t> bytes;
};

template <typename T, typename Encoder>
Property make_property(PropertyId id, const T& value, Encoder&& encoder) {
    return { id, std::forward<Encoder>(encoder)(value) };
}

constexpr auto p_string_encoder = [](const std::string& val) { return text_to_bytes(val); };
constexpr auto p_bool_encoder   = [](bool val) { return std::vector<std::uint8_t>{static_cast<std::uint8_t>(val)}; };
constexpr auto p_u16_encoder    = [](std::uint16_t val) { return to_bytes<std::uint16_t>(val); };
constexpr auto p_u32_encoder    = [](std::uint32_t val) { return to_bytes<std::uint32_t>(val); };
constexpr auto p_rgb_encoder    = [](const std::array<uint8_t, 3>& val) { return std::vector<std::uint8_t>(val.begin(), val.end()); }; // lmao
constexpr auto p_rgba_encoder   = [](const std::array<uint8_t, 4>& val) { return std::vector<std::uint8_t>(val.begin(), val.end()); };

void write_property_chunk(std::vector<std::uint8_t>& out, const std::vector<Property>& properties) {
    std::vector<std::uint8_t> descriptors;
    std::vector<std::uint8_t> payload;

    std::uint8_t count = 0;

    for (const auto& property : properties) {
        if (property.bytes.empty()) continue;
        ++count;

        descriptors.insert(descriptors.end(), property.id.begin(), property.id.end());
        const auto size_bytes = to_bytes<std::uint32_t>(property.bytes.size());
        descriptors.insert(descriptors.end(), size_bytes.begin(), size_bytes.end());
        payload.insert(payload.end(), property.bytes.begin(), property.bytes.end());
    }

    out.push_back(count);
    out.insert(out.end(), descriptors.begin(), descriptors.end());
    out.insert(out.end(), payload.begin(), payload.end());
}

std::expected<std::vector<Property>, std::string> read_property_chunk(std::ifstream& stream) {
    std::uint8_t count = 0;
    if (!stream.read(reinterpret_cast<char*>(&count), 1))
        return std::unexpected("Failde to read property count");

    std::vector<std::pair<std::array<char, 4>, std::uint32_t>> descriptors;
    descriptors.reserve(count);

    std::array<char, 4> id{};
    std::array<std::uint8_t, 4> size{};
    for (std::size_t i = 0; i < count; ++i) {
        if (!stream.read(reinterpret_cast<char*>(id.data()), 4))
            return std::unexpected("Failed to read property ID");

        if (!stream.read(reinterpret_cast<char*>(size.data()), 4))
            return std::unexpected("Failed to read property size");

        descriptors.emplace_back(id, from_bytes<std::uint32_t>(size));
    }

    // read payloads
    std::vector<Property> properties;
    properties.reserve(count);

    for (const auto& [id, size] : descriptors) {
        Property property{};
        property.id = id;
        property.bytes.resize(size);

        if (size > 0 && !stream.read(reinterpret_cast<char*>(property.bytes.data()), static_cast<std::streamsize>(size)))
            return std::unexpected("Failed to read property payload");

        properties.push_back(std::move(property));
    }

    return properties;
}

template <typename Decoder>
auto find_property(const std::vector<Property>& properties, const std::array<char, 4>& id, Decoder&& decoder) {
    const auto it = std::find_if(properties.begin(), properties.end(),
        [&](const Property& property) { return property.id == id; });

    using Result = decltype(decoder(std::declval<const std::vector<std::uint8_t>&>()));
    if (it == properties.end()) return Result{ std::unexpected(std::string("Property not found")) };
    return decoder(it->bytes);
}


constexpr auto p_string_decoder = [](const std::vector<std::uint8_t>& bytes) { return bytes_to_text(bytes); };
constexpr auto p_bool_decoder = [](const std::vector<std::uint8_t>& bytes) { return std::expected<bool, std::string>(bytes[0] == 1 ? true : false); };
constexpr auto p_u16_decoder = [](const std::vector<std::uint8_t>& bytes) { return std::expected<std::uint16_t, std::string>(from_bytes<std::uint16_t>(bytes)); };
constexpr auto p_u32_decoder = [](const std::vector<std::uint8_t>& bytes) { return std::expected<std::uint32_t, std::string>(from_bytes<std::uint32_t>(bytes)); };
constexpr auto p_rgb_decoder = [](const std::vector<std::uint8_t>& bytes) {
    std::array<std::uint8_t, 3> decoded{};
    decoded[0] = bytes[0];
    decoded[1] = bytes[1];
    decoded[2] = bytes[2];
    return std::expected<std::array<std::uint8_t, 3>, std::string>(std::move(decoded));
};
constexpr auto p_rgba_decoder = [](const std::vector<std::uint8_t>& bytes) {
    std::array<std::uint8_t, 4> decoded{};
    decoded[0] = bytes[0];
    decoded[1] = bytes[1];
    decoded[2] = bytes[2];
    decoded[3] = bytes[3];
    return std::expected<std::array<std::uint8_t, 4>, std::string>(std::move(decoded));
 };

}

namespace andromeda::editor {

ProjectWriter::ProjectWriter(ProjectManager* project_manager,
                             const std::filesystem::path& path)
    : stream_(path, std::ios::binary), project_manager_(project_manager) {}

std::expected<void, std::string> ProjectWriter::write() {
    auto try_write = [](auto&& write_fn, const char* payload_name)
        -> std::expected<void, std::string> {
        if (auto r = write_fn(); !r)
            return std::unexpected(std::format("Failed to write '{}' section. {}", payload_name, r.error()));
        return {};
    };

#define TRY(func, payload) if (auto r = try_write([this] { return func(); }, payload); !r) return r

    TRY(write_header, "Header");
    TRY(write_project_meta, "Project meta");
    TRY(write_track_meta_chunk, "Track meta(s)");
    TRY(write_global_event_chunk, "Global events");
    TRY(write_note_chunk, "Notes");
    TRY(write_channel_event_chunk, "Channel events");

#undef TRY

    if (auto r = finalize(); !r) return std::unexpected("Failed to finalize project file");

    return {};
}

std::expected<void, std::string> ProjectWriter::write_header() {
    static constexpr std::uint8_t MAGIC[] = {'A', 'n', 'H', 'd'};
    if (auto r = write(MAGIC); !r) {
        return r;
    }

    // version, track count, ppq.
    {
        const ProjectData& project_data = project_manager_->get_project_data();
        
        const auto ver_bytes = to_bytes<std::uint16_t>(AND_FORMAT_VERSION);
        buffer_.insert(buffer_.end(), ver_bytes.begin(), ver_bytes.end());

        const auto trk_bytes = to_bytes<std::uint16_t>(project_data.tracks.size());
        buffer_.insert(buffer_.end(), trk_bytes.begin(), trk_bytes.end());

        const auto ppq_bytes = to_bytes<std::uint16_t>(project_data.ppq);
        buffer_.insert(buffer_.end(), ppq_bytes.begin(), ppq_bytes.end());
    }

    // actually write the header
    const auto buf_len = to_bytes<std::uint32_t>(static_cast<std::uint32_t>(buffer_.size()));
    if (auto r = write(buf_len); !r) {
        return r;
    }
    if (auto r = flush_buffer(); !r) {
        return r;
    }

    return {};
}

std::expected<void, std::string> ProjectWriter::write_project_meta() {
    static constexpr std::uint8_t META[] = {'A', 'n', 'M', 'T'};
    if (auto r = write(META); !r) return r;

    const ProjectInfo& project_info = project_manager_->get_project_info();
    
    write_property_chunk(buffer_, {
        make_property({ 'n','a','m','e' }, project_info.name,        p_string_encoder),
        make_property({ 'a','u','t','h' }, project_info.author,      p_string_encoder),
        make_property({ 'd','e','s','c' }, project_info.description, p_string_encoder)
    });

    return flush_buffer();
}

std::expected<void, std::string> ProjectWriter::write_track_meta_chunk() {
    static constexpr std::uint8_t TrMt[4] = {'T', 'r', 'M', 't'};
    if (auto r = write(TrMt); !r) return r;

    const ProjectData& project_data = project_manager_->get_project_data();
    const auto& tracks = project_data.tracks;

    for (const auto& track : tracks) {
        std::vector<Property> properties{
            make_property({ 'n','a','m','e' }, track.name.value_or(""),     p_string_encoder),
            make_property({ 'm','u','t','e' }, track.muted,                 p_bool_encoder),
            make_property({ 'n','n','o','t' }, track.notes.size(),          p_u32_encoder),
            make_property({ 'n','c','e','v' }, track.channel_events.size(), p_u32_encoder)
        };

        if (track.project_meta.color_coded) {
            properties.push_back(
                make_property(std::array{ 'c','o','l',' ' }, track.project_meta.color_code, p_rgb_encoder));
        }

        write_property_chunk(buffer_, std::move(properties));
    }

    return flush_buffer();
}

std::expected<void, std::string> ProjectWriter::write_note_chunk() {
    static constexpr std::uint8_t MAGIC[4] = {'A', 'n', 'N', 't'};
    if (auto r = write(MAGIC); !r) return r;

    for (const auto& track : project_manager_->get_project_data().tracks) {
        if (auto r = write_block(codec::encode_notes(track.notes)); !r) return r;
    }
    return {};
}

std::expected<void, std::string> ProjectWriter::write_channel_event_chunk() {
    static constexpr std::uint8_t MAGIC[4] = {'A', 'n', 'C', 'E'};
    if (auto r = write(MAGIC); !r) return r;

    for (const auto& track : project_manager_->get_project_data().tracks) {
        if (auto r = write_block(codec::encode_channel_events(track.channel_events)); !r) return r;
    }
    return {};
}

std::expected<void, std::string> ProjectWriter::write_global_event_chunk() {
    static constexpr std::uint8_t MAGIC[4] = {'A', 'n', 'G', 'E'};
    if (auto r = write(MAGIC); !r) return r;

    const ProjectData& project_data = project_manager_->get_project_data();
    return write_block(codec::encode_meta_events(project_data.global_metas));
}

std::expected<void, std::string> ProjectWriter::write_block(std::span<const std::uint8_t> raw) {
    if (raw.size() > codec::MAX_BLOCK_SIZE) {
        return std::unexpected("Block too large to write");
    }

    const auto compressed = codec::compress(raw);
    if (!compressed) return std::unexpected(compressed.error());

    const auto raw_len = to_bytes<std::uint32_t>(static_cast<std::uint32_t>(raw.size()));
    const auto comp_len = to_bytes<std::uint32_t>(static_cast<std::uint32_t>(compressed->size()));

    buffer_.insert(buffer_.end(), raw_len.begin(), raw_len.end());
    buffer_.insert(buffer_.end(), comp_len.begin(), comp_len.end());
    buffer_.insert(buffer_.end(), compressed->begin(), compressed->end());

    return flush_buffer();
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

ProjectReader::ProjectReader(ProjectManager* project_manager, const std::filesystem::path& path)
    : stream_(path, std::ios::binary), project_manager_(project_manager) { }

std::expected<void, std::string> ProjectReader::load() {
    const auto header_result = parse_header();
    if (!header_result) return std::unexpected(header_result.error());

    const auto meta_result = parse_meta();
    if (!meta_result) return std::unexpected(meta_result.error());

    const auto track_metas_result = parse_track_metas(header_result->track_count);
    if (!track_metas_result) return std::unexpected(track_metas_result.error());

    // v2+ files carry the events. Parse them (and fail) *before* touching the open project.
    ProjectEvents events;
    if (header_result->version >= 2) {
        auto events_result = parse_events(*track_metas_result);
        if (!events_result) return std::unexpected(events_result.error());
        events = std::move(*events_result);
    }

    // we can now overwrite the current project at this point

    const auto version = header_result->version;

    if (version == 1 || version == 2) {
        project_manager_->new_empty_project();
        project_manager_->change_ppq(header_result->ppq);

        auto& tracks = *project_manager_->get_tracks();

        // "new empty project" may have already made a base track, so "i" starts at tracks.size() instead.
        for (std::size_t i = tracks.size(); i < static_cast<std::size_t>(header_result->track_count); i++) {
            tracks.push_back(midi::MIDITrack::new_empty());
        }

        auto& info = project_manager_->get_project_info();
        info.name = meta_result->name;
        info.author = meta_result->author;
        info.description = meta_result->description;

        for (std::size_t i = 0; i < tracks.size(); i++) {
            auto& track = tracks[i];
            auto& meta = (*track_metas_result)[i];
            track.name = meta.name.empty() ? std::nullopt : std::optional(meta.name);
            track.muted = meta.muted;

            if (i < events.notes.size()) track.notes = std::move(events.notes[i]);
            if (i < events.channel_events.size()) track.channel_events = std::move(events.channel_events[i]);

            // assign color code
            if (meta.color_code.has_value()) {
                track.set_color_code(*meta.color_code);
            }
        }

        if (version >= 2) {
            auto& events_dest = project_manager_->get_project_data().global_metas;
            events_dest = std::move(events.global_events);
            project_manager_->get_timeline()->rebuild(events_dest, header_result->ppq);
        }
    }

    return {};
}

std::expected<ProjectReader::ProjectHeader, std::string> ProjectReader::parse_header() {
    if (read_u32() != fourcc('A','n','H','d')) return std::unexpected("Not an Andromeda project file.");
    
    const auto header_size = read_u32();
    if (!header_size) return std::unexpected("Failed to read header size");
    if (*header_size != 6) return std::unexpected("Header size is not 6");

    const auto version = read_u16();
    if (!version) return std::unexpected(version.error());

    std::expected<std::uint16_t, std::string> track_count;
    std::expected<std::uint16_t, std::string> ppq;

    if (*version != 1 && *version != 2) {
        return std::unexpected("Unsupported project version " + std::to_string(*version));
    }

    {  // identical header layout in v1 and v2
        track_count = read_u16();
        if (!track_count) return std::unexpected(track_count.error());

        ppq = read_u16();
        if (!ppq) return std::unexpected(ppq.error());
    }

    return ProjectReader::ProjectHeader{ *version, *track_count, *ppq };
}

std::expected<ProjectReader::ProjectMeta, std::string> ProjectReader::parse_meta() {
    if (read_u32() != fourcc('A','n','M','T')) return std::unexpected("Expected meta chunk.");

    ProjectMeta meta{};

    const auto& properties = read_property_chunk(stream_);
    if (!properties) return std::unexpected(properties.error());

    auto name = find_property(*properties, { 'n','a','m','e' }, p_string_decoder);
    auto author = find_property(*properties, { 'a','u','t','h' }, p_string_decoder);
    auto desc = find_property(*properties, { 'd','e','s','c' }, p_string_decoder);

    if (name) meta.name = *name;
    if (author) meta.author = *author;
    if (desc) meta.description = *desc;

    return meta;
}

std::expected<std::vector<ProjectReader::TrackMeta>, std::string> ProjectReader::parse_track_metas(std::size_t count) {
    if (read_u32() != fourcc('T','r','M','t')) return std::unexpected("Expected track meta chunk.");

    std::vector<TrackMeta> metas{};
    metas.reserve(count);

    for (std::size_t i = 0; i < count; i++) {
        TrackMeta t_meta{};

        // read property
        const auto& properties = read_property_chunk(stream_);
        if (!properties) return std::unexpected(properties.error());

        auto name = find_property(*properties, { 'n','a','m','e' }, p_string_decoder);
        auto muted = find_property(*properties, { 'm','u','t','e' }, p_bool_decoder);
        auto note_count = find_property(*properties, { 'n','n','o','t' }, p_u32_decoder);
        auto ch_ev_count = find_property(*properties, { 'n','c','e','v' }, p_u32_decoder);
        auto color_code = find_property(*properties, { 'c','o','l',' ' }, p_rgb_decoder);

        if (name) t_meta.name = *name;
        if (muted) t_meta.muted = *muted;
        if (note_count) t_meta.note_count = *note_count;
        if (ch_ev_count) t_meta.channel_event_count = *ch_ev_count;
        if (color_code) { t_meta.color_code = std::move(*color_code); }

        metas.push_back(std::move(t_meta));
    }

    return metas;
}

std::expected<ProjectReader::ProjectEvents, std::string> ProjectReader::parse_events(
    const std::vector<TrackMeta>& track_metas) {
    ProjectEvents events;
    events.notes.reserve(track_metas.size());
    events.channel_events.reserve(track_metas.size());

    if (read_u32() != fourcc('A', 'n', 'G', 'E')) return std::unexpected("Expected global event chunk.");
    {
        const auto block = read_block();
        if (!block) return std::unexpected(block.error());

        auto global_events = codec::decode_meta_events(*block);
        if (!global_events) return std::unexpected(global_events.error());
        events.global_events = std::move(*global_events);
    }

    if (read_u32() != fourcc('A','n','N','t')) return std::unexpected("Expected note chunk.");
    for (const auto& meta : track_metas) {
        const auto block = read_block();
        if (!block) return std::unexpected(block.error());

        auto notes = codec::decode_notes(*block, meta.note_count);
        if (!notes) return std::unexpected(notes.error());
        events.notes.push_back(std::move(*notes));
    }

    if (read_u32() != fourcc('A','n','C','E')) return std::unexpected("Expected channel event chunk.");
    for (const auto& meta : track_metas) {
        const auto block = read_block();
        if (!block) return std::unexpected(block.error());

        auto channel_events = codec::decode_channel_events(*block, meta.channel_event_count);
        if (!channel_events) return std::unexpected(channel_events.error());
        events.channel_events.push_back(std::move(*channel_events));
    }

    return events;
}

std::expected<std::vector<std::uint8_t>, std::string> ProjectReader::read_block() {
    const auto raw_size = read_u32();
    if (!raw_size) return std::unexpected(raw_size.error());

    const auto comp_size = read_u32();
    if (!comp_size) return std::unexpected(comp_size.error());

    if (*raw_size > codec::MAX_BLOCK_SIZE || *comp_size > codec::MAX_BLOCK_SIZE) {
        return std::unexpected("Corrupt block: size is unreasonably large");
    }
    if ((*raw_size == 0) != (*comp_size == 0)) {
        return std::unexpected("Corrupt block: inconsistent sizes");
    }

    std::vector<std::uint8_t> compressed(*comp_size);
    if (*comp_size > 0 &&
        !stream_.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size())))
        return std::unexpected("Failed to read block payload");

    return codec::decompress(compressed, *raw_size);
}

std::expected<std::uint16_t, std::string> ProjectReader::read_u16() {
    std::array<std::uint8_t, 2> res{};

    if (!stream_.read(reinterpret_cast<char*>(res.data()), static_cast<std::streamsize>(res.size())))
        return std::unexpected("Failed to read short");

    return (static_cast<std::uint16_t>(res[0]) << 8) |
           (static_cast<std::uint16_t>(res[1]));
}

std::expected<std::uint32_t, std::string> ProjectReader::read_u32() {
    std::array<std::uint8_t, 4> res{};
    if (!stream_.read(reinterpret_cast<char*>(res.data()), static_cast<std::streamsize>(res.size())))
        return std::unexpected("Failed to read int");

    return (static_cast<std::uint32_t>(res[0]) << 24) |
           (static_cast<std::uint32_t>(res[1]) << 16) |
           (static_cast<std::uint32_t>(res[2]) << 8) |
           (static_cast<std::uint32_t>(res[3]));
}

std::expected<std::string, std::string> ProjectReader::read_text(std::size_t count) {
    if (count % 2 != 0) {
        return std::unexpected("Invalid UTF-16LE data: odd byte count");
    }

    std::string text;
    text.reserve(count);

    std::vector<std::uint8_t> bytes;
    bytes.resize(count);
    if (!stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return std::unexpected("Failed to read text");

    const auto read_unit = [&bytes](std::size_t offset) -> std::uint16_t {
        return static_cast<std::uint16_t>(bytes[offset]) |
            (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
        };

    const auto append_utf8 = [&text](std::uint32_t cp) {
        if (cp <= 0x7F) {
            text.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF) {
            text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF) {
            text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else {
            text.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        };

    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        std::uint32_t cp = read_unit(i);

        // UTF-16 surrogate pair
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 3 >= bytes.size()) {
                return std::unexpected("Invalid UTF-16LE: incomplete surrogate pair");
            }

            const std::uint16_t low = read_unit(i + 2);

            if (low < 0xDC00 || low > 0xDFFF) {
                return std::unexpected("Invalid UTF-16LE: invalid low surrogate");
            }

            cp = 0x10000 +
                ((cp - 0xD800) << 10) +
                (low - 0xDC00);

            i += 2;
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return std::unexpected("Invalid UTF-16LE: unexpected low surrogate");
        }

        append_utf8(cp);
    }

    return text;
}

}
