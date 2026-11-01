#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <vector>
#include <array>
#include <utility>
#include <cstddef>

namespace andromeda::editor {

enum class EventSourceKind : std::size_t {
    Velocity = 0,
    Tempo,
    PitchBend,
    ProgramChange,
    ControlChange,
    ChannelPressure,
    PolyPressure,
    Count // only for static asserts
};

enum class EventSourceOrigin {
    Notes,
    Keys,
    Channels,
    Global
};

struct EventSourceInfo {
    std::pair<double, double> range;
    EventSourceOrigin source_origin;
};

struct EventSource {
    std::string name;
    EventSourceKind kind;
    EventSourceInfo info;
};

class EventSources {
public:
    using SourceArray = std::array<EventSource, static_cast<std::size_t>(EventSourceKind::Count)>;

    [[nodiscard]] static const EventSource* get_source(EventSourceKind source_kind);
    [[nodiscard]] static const SourceArray& get_sources() { return sources; }
    [[nodiscard]] static std::string_view cc_name_of(std::uint8_t controller);
    [[nodiscard]] static std::string_view pc_name_of(std::uint8_t program);
private:
    // refactor: std::array of a non-trivial element needs the inner brace pair spelled out, or
    // MSVC cannot brace-elide into _Elems and rejects the whole list with C2440.
    inline static SourceArray sources = SourceArray{ {
        { "Velocity", EventSourceKind::Velocity,
            EventSourceInfo{ { 1, 127 }, EventSourceOrigin::Notes } },
        { "Tempo", EventSourceKind::Tempo,
            EventSourceInfo{ { 10.0, 522.0 }, EventSourceOrigin::Global } },
        { "Pitch bend", EventSourceKind::PitchBend,
            EventSourceInfo{ { -8192, 8191 }, EventSourceOrigin::Channels } },
        { "Program change", EventSourceKind::ProgramChange,
            EventSourceInfo{ { 0, 127 }, EventSourceOrigin::Channels } },
        { "Control change", EventSourceKind::ControlChange,
            EventSourceInfo{ { 0, 127 }, EventSourceOrigin::Channels } },
        { "Channel pressure", EventSourceKind::ChannelPressure,
            EventSourceInfo{ { 0, 127 }, EventSourceOrigin::Channels } },
        { "Polyphonic key pressure", EventSourceKind::PolyPressure,
            EventSourceInfo{ { 0, 127 }, EventSourceOrigin::Keys } }
    } };

    inline static std::vector<std::string> control_change_names{};
    inline static std::vector<std::string> patch_names{};
};

}