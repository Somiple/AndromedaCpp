#include "editor/editing/lua_note_editing.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "editor/editing.h"
#include "editor/editing/note_editing.h"
#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "midi/midi_track.h"
#include "util/debugger.h"

namespace andromeda::editor {

using midi::Note;
using note_seq::extract_with;
using note_seq::merge_notes_and_return_ids;
using util::Debugger;

namespace {

void check(const sol::protected_function_result& result) {
    if (!result.valid()) {
        const sol::error err = result;
        throw std::runtime_error(err.what());
    }
}

const std::vector<std::size_t>& selected_ids_or_empty(const SharedSelectedNotes& selected,
                                                      std::uint16_t track) {
    static const std::vector<std::size_t> empty;
    const std::vector<std::size_t>* ids = selected.get_selected_ids_in_track(track);
    return ids ? *ids : empty;
}

}

LuaNoteEditing::LuaNoteEditing(std::shared_ptr<NoteEditing> note_editing_)
    : note_editing(std::move(note_editing_)) {}

std::size_t LuaNoteEditing::current_track(sol::state_view lua) {
    const sol::optional<std::size_t> curr_track = lua["curr_track"];
    if (!curr_track) {
        throw std::runtime_error("curr_track is not set");
    }
    return *curr_track;
}

void LuaNoteEditing::register_types(sol::state& lua) {
    lua.new_usertype<Note>("Note", sol::no_constructor, "channel", &Note::channel, "start",
                           &Note::start, "length", &Note::length, "key", &Note::key, "velocity",
                           &Note::velocity);

    lua.new_usertype<LuaNoteEditing>(
        "LuaNoteEditing", sol::no_constructor,
        "for_each_note", &LuaNoteEditing::for_each_note,
        "for_each_selected", &LuaNoteEditing::for_each_selected,
        "iter_selected", &LuaNoteEditing::iter_selected,
        "get_selection_tick_range", &LuaNoteEditing::get_selection_tick_range,
        "get_selection_key_range", &LuaNoteEditing::get_selection_key_range,
        "create_note", &LuaNoteEditing::create_note);
}

void LuaNoteEditing::change_note_and_update_deltas(const sol::protected_function& func, Note& note,
                                                  std::size_t id) {
    const MIDITick old_start = note.get_start();
    const std::uint8_t old_key = note.get_key();
    const MIDITick old_length = note.get_length();
    const std::uint8_t old_channel = note.get_channel();
    const std::uint8_t old_velocity = note.get_velocity();

    Debugger::log(std::format(
        "Passing Note* {:p}, start={}, length={}",
        static_cast<void*>(&note),
        note.get_start(),
        note.get_length()
    ));

    check(func(&note));

    const SignedMIDITick delta_start =
        static_cast<SignedMIDITick>(note.get_start()) - static_cast<SignedMIDITick>(old_start);
    const auto delta_key =
        static_cast<std::int16_t>(static_cast<int>(note.get_key()) - static_cast<int>(old_key));
    const SignedMIDITick delta_length =
        static_cast<SignedMIDITick>(note.get_length()) - static_cast<SignedMIDITick>(old_length);
    const auto delta_channel = static_cast<std::int8_t>(static_cast<int>(note.get_channel()) -
                                                        static_cast<int>(old_channel));
    const auto delta_velocity = static_cast<std::int8_t>(static_cast<int>(note.get_velocity()) -
                                                         static_cast<int>(old_velocity));

    if (delta_start != 0 || delta_key != 0) {
        delta_note_pos[id] = {delta_start, delta_key};
    }

    if (delta_length != 0) {
        delta_note_lengths[id] = delta_length;
    }

    if (delta_channel != 0) {
        delta_note_channels[id] = delta_channel;
    }

    if (delta_velocity != 0) {
        delta_note_velocities[id] = delta_velocity;
    }
}

void LuaNoteEditing::for_each_note(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    const auto tracks = note_editing->get_tracks();
    std::unique_lock lock(tracks->mutex);
    if (curr_track >= tracks->value.size()) {
        throw std::runtime_error("curr_track is out of range");
    }
    std::vector<Note>& track = tracks->value[curr_track].get_notes_mut();

    for (std::size_t i = 0; i < track.size(); ++i) {
        change_note_and_update_deltas(func, track[i], i);
    }
}

void LuaNoteEditing::for_each_selected(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    const auto tracks = note_editing->get_tracks();
    const auto sel = note_editing->get_shared_selected_ids();

    std::unique_lock lock(tracks->mutex);
    if (curr_track >= tracks->value.size()) {
        throw std::runtime_error("curr_track is out of range");
    }
    std::vector<Note>& track = tracks->value[curr_track].get_notes_mut();

    const std::vector<std::size_t> sel_ids =
        selected_ids_or_empty(*sel, static_cast<std::uint16_t>(curr_track));

    for (const std::size_t sel_id : sel_ids) {
        if (sel_id >= track.size()) {
            continue;
        }
        change_note_and_update_deltas(func, track[sel_id], sel_id);
    }
}

void LuaNoteEditing::iter_selected(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    const auto tracks = note_editing->get_tracks();
    const auto sel = note_editing->get_shared_selected_ids();

    std::shared_lock lock(tracks->mutex);
    if (curr_track >= tracks->value.size()) {
        throw std::runtime_error("curr_track is out of range");
    }
    const std::vector<Note>& track = tracks->value[curr_track].get_notes();

    const std::vector<std::size_t> sel_ids =
        selected_ids_or_empty(*sel, static_cast<std::uint16_t>(curr_track));

    for (const std::size_t sel_id : sel_ids) {
        if (sel_id >= track.size()) {
            continue;
        }
        Note note = track[sel_id];
        check(func(&note));
    }
}

sol::object LuaNoteEditing::get_selection_tick_range(sol::this_state state, bool inclusive) {
    sol::state_view lua(state);
    const std::size_t curr_track = current_track(lua);

    const auto tracks = note_editing->get_tracks();
    const auto sel = note_editing->get_shared_selected_ids();

    const std::vector<std::size_t> sel_ids =
        selected_ids_or_empty(*sel, static_cast<std::uint16_t>(curr_track));
    if (sel_ids.empty()) {
        return sol::nil;
    }

    std::shared_lock lock(tracks->mutex);
    if (curr_track >= tracks->value.size()) {
        return sol::nil;
    }
    const std::vector<Note>& track = tracks->value[curr_track].get_notes();

    MIDITick min_tick = 0;
    MIDITick max_tick = 0;
    if (inclusive) {
        const auto range = get_min_max_ticks_in_selection(track, sel_ids);
        if (!range) {
            return sol::nil;
        }
        min_tick = range->first;
        max_tick = range->second;
    } else {
        if (sel_ids.front() >= track.size() || sel_ids.back() >= track.size()) {
            return sol::nil;
        }
        min_tick = track[sel_ids.front()].get_start();
        max_tick = track[sel_ids.back()].get_start();
    }

    sol::table table = lua.create_table();
    table["min"] = min_tick;
    table["max"] = max_tick;
    return table;
}

sol::object LuaNoteEditing::get_selection_key_range(sol::this_state state) {
    sol::state_view lua(state);
    const std::size_t curr_track = current_track(lua);

    const auto tracks = note_editing->get_tracks();
    const auto sel = note_editing->get_shared_selected_ids();

    const std::vector<std::size_t> sel_ids =
        selected_ids_or_empty(*sel, static_cast<std::uint16_t>(curr_track));
    if (sel_ids.empty()) {
        return sol::nil;
    }

    std::shared_lock lock(tracks->mutex);
    if (curr_track >= tracks->value.size()) {
        return sol::nil;
    }
    const std::vector<Note>& track = tracks->value[curr_track].get_notes();

    const auto range = get_min_max_keys_in_selection(track, sel_ids);
    if (!range) {
        return sol::nil;
    }

    sol::table table = lua.create_table();
    table["min"] = range->first;
    table["max"] = range->second;
    return table;
}

void LuaNoteEditing::create_note(MIDITick start, MIDITick length, std::uint8_t channel,
                                 std::uint8_t key, std::uint8_t velocity) {
    notes_to_add.push_back(Note{start, length, key, velocity, channel});
}

void LuaNoteEditing::apply_changes(std::uint16_t track, EditorActions& editor_actions) {
    std::vector<EditorAction> bulk_actions;

    std::vector<std::pair<std::size_t, std::int8_t>> dt_channels(delta_note_channels.begin(),
                                                                 delta_note_channels.end());
    std::vector<std::pair<std::size_t, std::int8_t>> dt_velocities(delta_note_velocities.begin(),
                                                                   delta_note_velocities.end());
    std::vector<std::pair<std::size_t, std::pair<SignedMIDITick, std::int16_t>>> dt_position(
        delta_note_pos.begin(), delta_note_pos.end());
    std::vector<std::pair<std::size_t, SignedMIDITick>> dt_length(delta_note_lengths.begin(),
                                                                  delta_note_lengths.end());
    std::vector<Note> notes_to_add_ = std::move(notes_to_add);

    const auto by_id = [](const auto& a, const auto& b) { return a.first < b.first; };
    std::sort(dt_channels.begin(), dt_channels.end(), by_id);
    std::sort(dt_velocities.begin(), dt_velocities.end(), by_id);
    std::sort(dt_length.begin(), dt_length.end(), by_id);
    std::sort(dt_position.begin(), dt_position.end(), by_id);

    if (!dt_channels.empty()) {
        std::vector<std::size_t> ids;
        std::vector<std::int8_t> ch_change;
        ids.reserve(dt_channels.size());
        ch_change.reserve(dt_channels.size());
        for (const auto& [id, delta] : dt_channels) {
            ids.push_back(id);
            ch_change.push_back(delta);
        }
        bulk_actions.push_back(ChannelChange{std::move(ids), std::move(ch_change), track});
    }

    if (!dt_velocities.empty()) {
        std::vector<std::size_t> ids;
        std::vector<std::int8_t> vel_change;
        ids.reserve(dt_velocities.size());
        vel_change.reserve(dt_velocities.size());
        for (const auto& [id, delta] : dt_velocities) {
            ids.push_back(id);
            vel_change.push_back(delta);
        }
        bulk_actions.push_back(VelocityChange{std::move(ids), std::move(vel_change), track});
    }

    if (!dt_length.empty()) {
        std::vector<std::size_t> ids;
        std::vector<SignedMIDITick> len_change;
        ids.reserve(dt_length.size());
        len_change.reserve(dt_length.size());
        for (const auto& [id, delta] : dt_length) {
            ids.push_back(id);
            len_change.push_back(delta);
        }
        bulk_actions.push_back(LengthChange{std::move(ids), std::move(len_change), track});
    }

    if (!dt_position.empty()) {
        std::vector<std::size_t> ids;
        std::vector<std::pair<SignedMIDITick, std::int16_t>> delta_pos;
        ids.reserve(dt_position.size());
        delta_pos.reserve(dt_position.size());
        for (const auto& [id, delta] : dt_position) {
            ids.push_back(id);
            delta_pos.push_back(delta);
        }

        std::vector<Note> old_notes = note_editing->take_notes_in_track(track);

        auto [notes_with_delta, remaining] =
            extract_with(std::move(old_notes), ids, std::move(delta_pos));
        std::stable_sort(notes_with_delta.begin(), notes_with_delta.end(),
                         [](const auto& a, const auto& b) {
                             return a.first.get_start() < b.first.get_start();
                         });

        std::vector<Note> notes_to_move;
        std::vector<std::pair<SignedMIDITick, std::int16_t>> delta;
        notes_to_move.reserve(notes_with_delta.size());
        delta.reserve(notes_with_delta.size());
        for (auto& [note, d] : notes_with_delta) {
            notes_to_move.push_back(note);
            delta.push_back(d);
        }

        auto [merged, note_ids] =
            merge_notes_and_return_ids(std::move(remaining), std::move(notes_to_move));
        note_editing->set_notes_in_track(track, std::move(merged));

        bulk_actions.push_back(NotesMove{std::move(note_ids), std::move(delta), track, true});
    }

    if (!notes_to_add_.empty()) {
        std::sort(notes_to_add_.begin(), notes_to_add_.end(),
                  [](const Note& a, const Note& b) { return a.get_start() < b.get_start(); });

        std::vector<Note> old_notes = note_editing->take_notes_in_track(track);
        auto [merged, ids] =
            merge_notes_and_return_ids(std::move(old_notes), std::move(notes_to_add_));

        note_editing->set_notes_in_track(track, std::move(merged));
        bulk_actions.push_back(PlaceNotes{std::move(ids), std::nullopt, track});
    }

    if (!bulk_actions.empty()) {
        editor_actions.register_action(Bulk{std::move(bulk_actions)});
    }
}

}
