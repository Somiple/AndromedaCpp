#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <optional>
#include <array>

#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"

namespace andromeda::midi {

inline std::atomic<std::uint64_t> g_track_revisions{1};

inline std::uint64_t next_track_revision() {
    return g_track_revisions.fetch_add(1, std::memory_order_relaxed);
}

// dunno what to actually name this. lol
struct ProjectTrackMeta {
    bool color_coded = false;
    std::array<std::uint8_t, 3> color_code{ 0,0,0 };
};

struct MIDITrack {
    bool muted = false;
    std::optional<std::string> name = std::nullopt;
    std::vector<ChannelEvent> channel_events;
    std::vector<MetaEvent> meta_events;
    std::vector<Note> notes;
    ProjectTrackMeta project_meta{};

    // mutate notes only via get_notes_mut; renderer reuses gpu data until this changes
    std::uint64_t revision = next_track_revision();

    MIDITrack() = default;

    MIDITrack(std::vector<Note> notes_,
              std::vector<ChannelEvent> channel_events_,
              std::vector<MetaEvent> meta_events_)
        : muted(false),
          name({}),
          channel_events(std::move(channel_events_)),
          meta_events(std::move(meta_events_)),
          notes(std::move(notes_)) {}

    static MIDITrack new_empty() { return MIDITrack{}; }

    [[nodiscard]] const std::vector<Note>& get_notes() const { return notes; }
    std::vector<Note>& get_notes_mut() { revision = next_track_revision(); return notes; }

    [[nodiscard]] const std::vector<ChannelEvent>& get_channel_evs() const { return channel_events; }
    std::vector<ChannelEvent>& get_channel_evs_mut() { return channel_events; }

    [[nodiscard]] const std::vector<MetaEvent>& get_meta_events() const { return meta_events; }
    std::vector<MetaEvent>& get_meta_events_mut() { return meta_events; }

    [[nodiscard]] bool is_empty() const { return notes.empty() && channel_events.empty(); }

    void set_color_code(std::array<std::uint8_t, 3> color) {
        project_meta.color_coded = true;
        project_meta.color_code = std::move(color);
    }

    void clear_color_code() {
        project_meta.color_coded = false;
        project_meta.color_code = { 0, 0, 0 };
    }

    [[nodiscard]] const std::array<std::uint8_t, 3>* get_color_code() const {
        if (!project_meta.color_coded) return nullptr;
        return &project_meta.color_code;
    }

    void clear_track() {
        revision = next_track_revision();
        notes.clear();
        channel_events.clear();
        meta_events.clear();
    }
};

}
