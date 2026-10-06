#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "editor/midi_types.h"
#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"

namespace andromeda::editor {

using midi::ChannelEvent;
using midi::ChannelEventType;
using midi::MetaEvent;
using midi::MetaEventType;
using midi::Note;

std::size_t bin_search_notes(const std::vector<Note>& notes, MIDITick tick);

std::size_t bin_search_notes_exact(const std::vector<Note>& notes, MIDITick tick);

std::size_t veb_search_notes(const std::vector<Note>& notes, MIDITick tick);

std::vector<std::size_t> get_notes_in_range(const std::vector<Note>& notes, MIDITick min_tick,
                                            MIDITick max_tick, std::uint8_t min_key,
                                            std::uint8_t max_key, bool include_ends);

std::optional<std::size_t> find_note_at(const std::vector<Note>& notes, MIDITick tick_pos,
                                        std::uint8_t key_pos);

std::optional<std::pair<std::uint8_t, std::uint8_t>> get_min_max_keys_in_selection(
    const std::vector<Note>& notes, const std::vector<std::size_t>& ids);

std::optional<std::pair<MIDITick, MIDITick>> get_min_max_ticks_in_selection(
    const std::vector<Note>& notes, const std::vector<std::size_t>& ids);

std::optional<MIDITick> get_absolute_max_tick_from_ids(const std::vector<Note>& notes,
                                                       const std::vector<std::size_t>& ids);

struct PianoRollNavigation;
struct TrackViewNavigation;

struct ViewRect {
    float left = 0.0f;
    float top = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

struct MousePianoRollPos {
    MIDITick tick = 0;
    std::uint8_t key = 0;
    std::uint8_t key_rounded = 0;
};

MousePianoRollPos get_mouse_midi_pos(const ViewRect& rect, float mouse_x, float mouse_y, const PianoRollNavigation& nav);

std::pair<MIDITick, std::uint16_t> get_mouse_track_view_pos(const ViewRect& rect, float mouse_x,
                                                            float mouse_y, bool mouse_over,
                                                            const TrackViewNavigation& nav);

struct MoveNotesResult {
    std::vector<std::size_t> old_ids;
    std::vector<std::size_t> new_ids;
    std::vector<std::pair<SignedMIDITick, std::int16_t>> changed_positions;
};

MoveNotesResult move_notes_to(
    std::vector<Note>& notes,
    std::vector<std::pair<std::size_t, std::pair<SignedMIDITick, std::int16_t>>> ids_with_delta_pos);

template <typename StartFn>
MoveNotesResult manipulate_note_ticks(std::vector<Note>& notes,
                                      const std::vector<std::size_t>& ids,
                                      StartFn start_fn) {
    std::vector<std::pair<std::size_t, std::pair<SignedMIDITick, std::int16_t>>> ids_with_delta_pos;
    ids_with_delta_pos.reserve(ids.size());

    for (const std::size_t id : ids) {
        Note& note = notes[id];
        const MIDITick old_start = note.get_start();
        note.start = start_fn(note.get_start());
        const MIDITick new_start = note.get_start();
        ids_with_delta_pos.emplace_back(
            id, std::pair<SignedMIDITick, std::int16_t>{
                    static_cast<SignedMIDITick>(new_start) - static_cast<SignedMIDITick>(old_start),
                    0});
    }

    return move_notes_to(notes, std::move(ids_with_delta_pos));
}

template <typename LengthFn>
std::vector<SignedMIDITick> manipulate_note_lengths(std::vector<Note>& notes,
                                                    const std::vector<std::size_t>& ids,
                                                    LengthFn length_fn) {
    std::vector<SignedMIDITick> changed_lengths;

    for (const std::size_t id : ids) {
        Note& note = notes[id];
        const MIDITick old_length = note.length;
        const MIDITick new_length = length_fn(old_length);

        changed_lengths.push_back(static_cast<SignedMIDITick>(new_length) -
                                  static_cast<SignedMIDITick>(old_length));
        note.length = new_length;
    }

    return changed_lengths;
}

template <typename T>
void move_element(std::vector<T>& v, std::size_t from, std::size_t to) {
    if (from < to) {
        std::rotate(v.begin() + static_cast<std::ptrdiff_t>(from),
                    v.begin() + static_cast<std::ptrdiff_t>(from) + 1,
                    v.begin() + static_cast<std::ptrdiff_t>(to) + 1);
    } else if (from > to) {
        std::rotate(v.begin() + static_cast<std::ptrdiff_t>(to),
                    v.begin() + static_cast<std::ptrdiff_t>(from),
                    v.begin() + static_cast<std::ptrdiff_t>(from) + 1);
    }
}

std::pair<std::uint16_t, std::uint8_t> decode_note_group(std::uint32_t note_group);

std::uint32_t mul_rgb(std::uint32_t rgb, float val);

const MetaEvent* get_meta_next_tick(const std::vector<MetaEvent>& metas, MetaEventType meta_type,
                                    MIDITick tick);

std::filesystem::path path_rel_to_abs(const std::string& path);

std::array<std::uint8_t, 3> tempo_as_bytes(float tempo);

float bytes_as_tempo(std::span<const std::uint8_t> bytes);

std::optional<std::size_t> get_next_specific_ch_ev_idx(std::span<const ChannelEvent> ch_evs,
                                                       const ChannelEventType& ev_type,
                                                       std::optional<std::size_t> start_idx);

}
