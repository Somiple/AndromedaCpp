#include "editor/util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include "editor/navigation.h"
#include "editor/settings/editor_settings.h"
#include "util/numeric.h"

namespace andromeda::editor {

std::size_t bin_search_notes(const std::vector<Note>& notes, MIDITick tick) {
    if (notes.empty()) {
        return 0;
    }

    std::size_t low = 0;
    std::size_t high = notes.size();

    if (tick <= notes[low].start) {
        return 0;
    }
    if (tick >= notes[high - 1].start) {
        return high;
    }

    while (low < high) {
        const std::size_t mid = (low + high) / 2;
        if (notes[mid].start <= tick) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    return low;
}

std::size_t bin_search_notes_exact(const std::vector<Note>& notes, MIDITick tick) {
    if (notes.empty()) {
        return 0;
    }

    std::size_t low = 0;
    std::size_t high = notes.size() - 1;

    if (tick <= notes[low].start) {
        return 0;
    }
    if (tick >= notes[high].start) {
        return high;
    }

    while (low < high) {
        const std::size_t mid = (low + high) / 2;
        if (notes[mid].start < tick) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    return low;
}

namespace {

std::size_t veb_recursive(std::span<const Note> notes, MIDITick tick, std::size_t offset,
                          int& num_steps) {
    const std::size_t n = notes.size();
    if (n == 0) {
        return 0;
    }
    if (n == 1) {
        std::printf("Searched in %d steps.\n", num_steps);
        return notes[0].get_start() <= tick ? offset : 0;
    }

    const std::size_t block_size = static_cast<std::size_t>(std::sqrt(static_cast<double>(n)));
    std::size_t i = 0;

    while (i + block_size < n && notes[i + block_size].get_start() <= tick) {
        i += block_size;
    }

    const std::size_t end = std::min(i + block_size + 1, n);
    num_steps += 1;
    return veb_recursive(notes.subspan(i, end - i), tick, offset + i, num_steps);
}

}

std::size_t veb_search_notes(const std::vector<Note>& notes, MIDITick tick) {
    int num_steps = 0;
    return veb_recursive(notes, tick, 0, num_steps);
}

std::vector<std::size_t> get_notes_in_range(const std::vector<Note>& notes, MIDITick min_tick,
                                            MIDITick max_tick, std::uint8_t min_key,
                                            std::uint8_t max_key, bool include_ends) {
    std::vector<std::size_t> note_ids;
    if (notes.empty()) {
        return note_ids;
    }

    {
        const Note& low_note = notes.front();
        const Note& high_note = notes.back();

        if (min_tick < low_note.start && max_tick < low_note.start) {
            return note_ids;
        }
        if (min_tick > high_note.start + high_note.length &&
            max_tick > high_note.start + high_note.length) {
            return note_ids;
        }
    }

    for (std::size_t i = 0; i < notes.size(); ++i) {
        const Note& note = notes[i];

        if ((note.start >= min_tick || (note.start + note.length > min_tick && include_ends)) &&
            note.start < max_tick && note.key >= min_key && note.key < max_key) {
            note_ids.push_back(i);
        }
        if (note.start > max_tick) {
            break;
        }
    }

    return note_ids;
}

std::optional<std::size_t> find_note_at(const std::vector<Note>& notes, MIDITick tick_pos,
                                        std::uint8_t key_pos) {
    if (notes.empty()) {
        return std::nullopt;
    }

    std::size_t low = 0;
    std::size_t high = notes.size();

    {
        const Note& lower = notes[low];
        const Note& upper = notes[high - 1];
        if (tick_pos < lower.start) {
            return std::nullopt;
        }
        if (tick_pos > upper.start + upper.length && tick_pos > lower.start + lower.length) {
            return std::nullopt;
        }
    }

    while (low < high) {
        const std::size_t mid = (low + high) / 2;
        if (notes[mid].start <= tick_pos) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    for (std::size_t i = low; i-- > 0;) {
        const Note& note = notes[i];
        if (note.key == key_pos && note.start <= tick_pos && note.start + note.length >= tick_pos) {
            return i;
        }
    }

    for (std::size_t i = low; i < notes.size(); ++i) {
        const Note& note = notes[i];
        if (note.key == key_pos && note.start <= tick_pos && note.start + note.length >= tick_pos) {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> get_min_max_keys_in_selection(
    const std::vector<Note>& notes, const std::vector<std::size_t>& ids) {
    if (ids.empty()) {
        return std::nullopt;
    }

    std::uint8_t min_key = 127;
    std::uint8_t max_key = 0;
    for (const std::size_t id : ids) {
        const Note& note = notes[id];
        if (min_key >= note.key) {
            min_key = note.key;
        }
        if (max_key <= note.key) {
            max_key = note.key;
        }
    }
    return std::pair{min_key, max_key};
}

std::optional<std::pair<MIDITick, MIDITick>> get_min_max_ticks_in_selection(
    const std::vector<Note>& notes, const std::vector<std::size_t>& ids) {
    if (ids.empty()) {
        return std::nullopt;
    }

    const MIDITick min_tick = notes[ids[0]].start;
    const MIDITick max_tick = *get_absolute_max_tick_from_ids(notes, ids);

    return std::pair{min_tick, max_tick};
}

std::optional<MIDITick> get_absolute_max_tick_from_ids(const std::vector<Note>& notes,
                                                       const std::vector<std::size_t>& ids) {
    if (ids.empty()) {
        return std::nullopt;
    }

    MIDITick max_tick = 0;
    for (const std::size_t id : ids) {
        const Note& note = notes[id];
        if (note.get_start() + note.get_length() >= max_tick) {
            max_tick = note.get_start() + note.get_length();
        }
    }

    return max_tick;
}

MoveNotesResult move_notes_to(
    std::vector<Note>& notes,
    std::vector<std::pair<std::size_t, std::pair<SignedMIDITick, std::int16_t>>>
        ids_with_delta_pos) {
    const std::size_t total = notes.size();
    const std::size_t moved_count = ids_with_delta_pos.size();

    const std::vector<Note> old_notes = std::move(notes);
    notes.clear();

    struct MovedNote {
        std::size_t old_id;
        Note note;
        SignedMIDITick start_delta;
        std::int16_t key_delta;
    };

    std::vector<Note> kept;
    kept.reserve(total - moved_count);
    std::vector<MovedNote> moved;
    moved.reserve(moved_count);

    std::size_t next = 0;

    for (std::size_t idx = 0; idx < old_notes.size(); ++idx) {
        if (next < ids_with_delta_pos.size()) {
            const auto& [move_id, deltas] = ids_with_delta_pos[next];
            if (idx == move_id) {
                moved.push_back(MovedNote{move_id, old_notes[idx], deltas.first, deltas.second});
                ++next;
                continue;
            }
        }

        kept.push_back(old_notes[idx]);
    }

    std::sort(moved.begin(), moved.end(),
              [](const MovedNote& a, const MovedNote& b) { return a.note.get_start() < b.note.get_start(); });

    std::vector<Note> merged;
    merged.reserve(total);

    MoveNotesResult result;
    result.old_ids.reserve(moved_count);
    result.new_ids.reserve(moved_count);
    result.changed_positions.reserve(moved_count);

    std::size_t ki = 0;
    std::size_t mi = 0;
    std::size_t write_idx = 0;

    for (;;) {
        if (ki < kept.size() && mi < moved.size()) {
            if (kept[ki].get_start() <= moved[mi].note.get_start()) {
                merged.push_back(std::move(kept[ki++]));
                write_idx += 1;
            } else {
                MovedNote& m = moved[mi++];
                result.old_ids.push_back(m.old_id);
                result.new_ids.push_back(write_idx);
                result.changed_positions.emplace_back(m.start_delta, m.key_delta);
                merged.push_back(std::move(m.note));
                write_idx += 1;
            }
        } else if (ki < kept.size()) {
            merged.insert(merged.end(), std::make_move_iterator(kept.begin() + static_cast<std::ptrdiff_t>(ki)),
                          std::make_move_iterator(kept.end()));
            break;
        } else if (mi < moved.size()) {
            while (mi < moved.size()) {
                MovedNote& m = moved[mi++];
                result.old_ids.push_back(m.old_id);
                result.new_ids.push_back(write_idx);
                result.changed_positions.emplace_back(m.start_delta, m.key_delta);
                merged.push_back(std::move(m.note));
                write_idx += 1;
            }
            break;
        } else {
            break;
        }
    }

    notes = std::move(merged);

    return result;
}

std::pair<std::uint16_t, std::uint8_t> decode_note_group(std::uint32_t note_group) {
    return {static_cast<std::uint16_t>(note_group >> 8),
            static_cast<std::uint8_t>(note_group & 0xF)};
}

std::uint32_t mul_rgb(std::uint32_t rgb, float val) {
    const float r = static_cast<float>((rgb & 0xFF0000) >> 16) * val;
    const float g = static_cast<float>((rgb & 0xFF00) >> 8) * val;
    const float b = static_cast<float>(rgb & 0xFF) * val;
    return ((static_cast<std::uint32_t>(r) & 0xFF) << 16) |
           ((static_cast<std::uint32_t>(g) & 0xFF) << 8) |
           (static_cast<std::uint32_t>(b) & 0xFF);
}

const MetaEvent* get_meta_next_tick(const std::vector<MetaEvent>& metas, MetaEventType meta_type,
                                    MIDITick tick) {
    for (const MetaEvent& meta : metas) {
        if (meta.event_type == meta_type) {
            if (meta.tick < tick) {
                continue;
            }
            return &meta;
        }
    }
    return nullptr;
}

std::filesystem::path path_rel_to_abs(const std::string& path) {
    return std::filesystem::absolute(path);
}

std::array<std::uint8_t, 3> tempo_as_bytes(float tempo) {
    const std::uint32_t tempo_conv = static_cast<std::uint32_t>(60000000.0f / tempo);
    return {static_cast<std::uint8_t>((tempo_conv >> 16) & 0xFF),
            static_cast<std::uint8_t>((tempo_conv >> 8) & 0xFF),
            static_cast<std::uint8_t>(tempo_conv & 0xFF)};
}

float bytes_as_tempo(std::span<const std::uint8_t> bytes) {
    const std::uint32_t bytes_conv = (static_cast<std::uint32_t>(bytes[0]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                     static_cast<std::uint32_t>(bytes[2]);
    return 60000000.0f / static_cast<float>(bytes_conv);
}

std::optional<std::size_t> get_next_specific_ch_ev_idx(std::span<const ChannelEvent> ch_evs,
                                                       const ChannelEventType& ev_type,
                                                       std::optional<std::size_t> start_idx) {
    const std::size_t start = start_idx.value_or(0);
    if (start >= ch_evs.size()) {
        return std::nullopt;
    }

    for (std::size_t search_idx = start; search_idx < ch_evs.size(); ++search_idx) {
        if (ch_evs[search_idx].event_type.index() == ev_type.index()) {
            return search_idx;
        }
    }

    return std::nullopt;
}

using util::saturating_cast;

MousePianoRollPos get_mouse_midi_pos(const ViewRect& rect, float mouse_x, float mouse_y,
                                     bool mouse_over, const PianoRollNavigation& nav) {
    if (!mouse_over) {
        return {};
    }

    float mx = (mouse_x - rect.left) / rect.width;
    float my = 1.0f - (mouse_y - rect.top) / rect.height;

    const float keyboard_width = PR_KEYBOARD_WIDTH / rect.width;
    mx = (mx - keyboard_width) / (1.0f - keyboard_width);

    const float tick = mx * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed;
    const float key = my * nav.zoom_keys_smoothed + nav.key_pos_smoothed;

    return {saturating_cast<MIDITick>(tick), saturating_cast<std::uint8_t>(key),
            saturating_cast<std::uint8_t>(std::round(key))};
}

std::pair<MIDITick, std::uint16_t> get_mouse_track_view_pos(const ViewRect& rect, float mouse_x,
                                                            float mouse_y, bool mouse_over,
                                                            const TrackViewNavigation& nav) {
    if (!mouse_over) {
        return {0, 0};
    }

    const float mx = (mouse_x - rect.left) / rect.width;
    const float my = (mouse_y - rect.top) / rect.height;

    const float tick = mx * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed;
    const float track = my * nav.zoom_tracks_smoothed + nav.track_pos_smoothed;

    return {saturating_cast<MIDITick>(tick), saturating_cast<std::uint16_t>(track)};
}

}
