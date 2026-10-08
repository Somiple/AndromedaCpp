#include "editor/editing/lua_note_editing.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "editor/editing.h"
#include "editor/editing/note_editing.h"
#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "editor/editor_controller.h"
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
        "for_each_selected_all", &LuaNoteEditing::for_each_selected_all,
        "iter_selected", &LuaNoteEditing::iter_selected,
        "iter_selected_all", &LuaNoteEditing::iter_selected_all,
        "get_selection_tick_range", &LuaNoteEditing::get_selection_tick_range,
        "get_selection_key_range", &LuaNoteEditing::get_selection_key_range,
        "create_note", &LuaNoteEditing::create_note);
}

void LuaNoteEditing::change_note_and_update_deltas(const sol::protected_function& func, Note& note,
                                                  std::size_t id, PendingNoteEdits& edits) {
    const MIDITick old_start = note.get_start();
    const std::uint8_t old_key = note.get_key();
    const MIDITick old_length = note.get_length();
    const std::uint8_t old_channel = note.get_channel();
    const std::uint8_t old_velocity = note.get_velocity();

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
    
    // fix: if a plugin touched a note twice, only the last delta was recorded!
    if (delta_start != 0 || delta_key != 0) {
        auto [it, inserted] = edits.positions.try_emplace(id, std::pair{ delta_start, delta_key });
        if (!inserted) {
            it->second.first += delta_start;
            it->second.second = static_cast<std::int16_t>(it->second.second + delta_key);
            if (it->second.first == 0 && it->second.second == 0) edits.positions.erase(it);
        }
    }

    const auto accumulate = [id](auto& map, auto delta) {
        if (delta == 0) return;
        auto [it, inserted] = map.try_emplace(id, delta);
        if (!inserted) {
            it->second = static_cast<decltype(delta)>(it->second + delta);
            if (it->second == 0) map.erase(it);
        }
    };

    accumulate(edits.lengths, delta_length);
    accumulate(edits.channels, delta_channel);
    accumulate(edits.velocities, delta_velocity);
}

void LuaNoteEditing::rollback() {
    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();

    for (const auto& [track, edits] : pending_edits) {
        std::vector<Note>& notes = tracks[track].get_notes_mut();
        const auto sub_tick = [](MIDITick v, SignedMIDITick d) {
            return static_cast<MIDITick>(static_cast<SignedMIDITick>(v) - d);
        };

        for (const auto& [id, d] : edits.positions) {
            if (id >= notes.size()) continue;
            notes[id].start = sub_tick(notes[id].start, d.first);
            notes[id].key = static_cast<std::uint8_t>(static_cast<int>(notes[id].key) - d.second);
        }

        for (const auto& [id, d] : edits.lengths) {
            if (id < notes.size()) notes[id].length = sub_tick(notes[id].length, d);
        }

        for (const auto& [id, d] : edits.channels) {
            if (id < notes.size())
                notes[id].channel = static_cast<std::uint8_t>(static_cast<int>(notes[id].channel) - d);
        }
        for (const auto& [id, d] : edits.velocities) {
            if (id < notes.size())
                notes[id].velocity = static_cast<std::uint8_t>(static_cast<int>(notes[id].velocity) - d);
        }
    }

    pending_edits.clear();
}

void LuaNoteEditing::for_each_note(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    if (curr_track >= tracks.size()) {
        throw std::runtime_error("curr_track is out of range");
    }

    auto& pending = pending_edits[curr_track];
    std::vector<Note>& track = tracks[curr_track].get_notes_mut();

    for (std::size_t i = 0; i < track.size(); ++i) {
        change_note_and_update_deltas(func, track[i], i, pending);
    }
}

void LuaNoteEditing::for_each_selected(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    SharedSelectedNotes& selection = *_controller->get_selection();

    if (curr_track >= tracks.size()) {
        throw std::runtime_error("curr_track is out of range");
    }

    auto& pending = pending_edits[curr_track];
    std::vector<Note>& track = tracks[curr_track].get_notes_mut();

    const std::vector<std::size_t>& sel_ids =
        selected_ids_or_empty(selection, static_cast<std::uint16_t>(curr_track));

    for (const std::size_t sel_id : sel_ids) {
        if (sel_id >= track.size()) {
            continue;
        }
        change_note_and_update_deltas(func, track[sel_id], sel_id, pending);
    }
}

void LuaNoteEditing::for_each_selected_all(sol::this_state state, sol::protected_function func) {
    auto& tracks = *_controller->get_project_manager()->get_tracks();
    auto& selection = *_controller->get_selection();

    for (std::size_t track_id = 0; track_id < tracks.size(); ++track_id) {
        const auto& sel_ids = selected_ids_or_empty(
            selection,
            static_cast<std::uint16_t>(track_id)
        );

        if (sel_ids.empty()) {
            continue;
        }

        auto& pending = pending_edits[track_id];
        auto& notes = tracks[track_id].get_notes_mut();

        for (const std::size_t id : sel_ids) {
            if (id >= notes.size()) {
                continue;
            }

            change_note_and_update_deltas(
                func,
                notes[id],
                id,
                pending
            );
        }
    }
}

void LuaNoteEditing::iter_selected(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    SharedSelectedNotes& selection = *_controller->get_selection();

    if (curr_track >= tracks.size()) {
        throw std::runtime_error("curr_track is out of range");
    }
    const std::vector<Note>& track = tracks[curr_track].get_notes();

    const std::vector<std::size_t>& sel_ids =
        selected_ids_or_empty(selection, static_cast<std::uint16_t>(curr_track));

    for (const std::size_t sel_id : sel_ids) {
        if (sel_id >= track.size()) {
            continue;
        }
        Note note = track[sel_id];
        check(func(&note));
    }
}

void LuaNoteEditing::iter_selected_all(sol::this_state state, sol::protected_function func) {
    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    SharedSelectedNotes& selection = *_controller->get_selection();

    for (auto track_id = 0; track_id < tracks.size(); track_id++) {
        const auto& sel_ids = selected_ids_or_empty(
            selection,
            static_cast<std::uint16_t>(track_id)
        );

        auto& notes = tracks[track_id].get_notes_mut();

        for (const std::size_t sel_id : sel_ids) {
            if (sel_id >= notes.size()) continue;
            Note note = notes[sel_id];
            check(func(&note));
        }
    }
}

sol::object LuaNoteEditing::get_selection_tick_range(sol::this_state state, bool inclusive) {
    sol::state_view lua(state);
    const std::size_t curr_track = current_track(lua);

    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    SharedSelectedNotes& selection = *_controller->get_selection();

    const std::vector<std::size_t>& sel_ids =
        selected_ids_or_empty(selection, static_cast<std::uint16_t>(curr_track));
    if (sel_ids.empty()) {
        return sol::nil;
    }

    if (curr_track >= tracks.size()) {
        return sol::nil;
    }
    const std::vector<Note>& track = tracks[curr_track].get_notes();

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

    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    SharedSelectedNotes& selection = *_controller->get_selection();

    const std::vector<std::size_t>& sel_ids =
        selected_ids_or_empty(selection, static_cast<std::uint16_t>(curr_track));
    if (sel_ids.empty()) {
        return sol::nil;
    }

    if (curr_track >= tracks.size()) {
        return sol::nil;
    }
    const std::vector<Note>& track = tracks[curr_track].get_notes();

    const auto range = get_min_max_keys_in_selection(track, sel_ids);
    if (!range) {
        return sol::nil;
    }

    sol::table table = lua.create_table();
    table["min"] = range->first;
    table["max"] = range->second;
    return table;
}

void LuaNoteEditing::create_note(std::uint16_t track, MIDITick start, MIDITick length, std::uint8_t channel,
                                 std::uint8_t key, std::uint8_t velocity) {
    auto& pending = pending_edits[track];
    pending.notes_to_add.emplace_back(start, length, key, channel, velocity);
}

void LuaNoteEditing::apply_changes(EditorActions& editor_actions) {
    std::vector<EditorAction> bulk_actions;

    for (auto& [track_id, edits] : pending_edits) {
        std::vector<EditorAction> track_actions;

        std::vector<std::pair<std::size_t, std::int8_t>> dt_channels(edits.channels.begin(),
            edits.channels.end());
        std::vector<std::pair<std::size_t, std::int8_t>> dt_velocities(edits.velocities.begin(),
            edits.velocities.end());
        std::vector<std::pair<std::size_t, std::pair<SignedMIDITick, std::int16_t>>> dt_position(
            edits.positions.begin(), edits.positions.end());
        std::vector<std::pair<std::size_t, SignedMIDITick>> dt_length(edits.lengths.begin(),
            edits.lengths.end());
        std::vector<Note> notes_to_add_ = std::move(edits.notes_to_add);

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
            track_actions.push_back(ChannelChange{ std::move(ids), std::move(ch_change), track_id });
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
            track_actions.push_back(VelocityChange{ std::move(ids), std::move(vel_change), track_id });
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
            track_actions.push_back(LengthChange{ std::move(ids), std::move(len_change), track_id });
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

            NoteEditing* note_editing = _controller->get_note_editing();
            std::vector<Note> old_notes = note_editing->take_notes_in_track(track_id);

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
            note_editing->set_notes_in_track(track_id, std::move(merged));

            track_actions.push_back(NotesMove{ std::move(note_ids), std::move(delta), track_id, true });
        }

        if (!notes_to_add_.empty()) {
            NoteEditing* note_editing = _controller->get_note_editing();

            std::sort(notes_to_add_.begin(), notes_to_add_.end(),
                [](const Note& a, const Note& b) { return a.get_start() < b.get_start(); });

            std::vector<Note> old_notes = note_editing->take_notes_in_track(track_id);
            auto [merged, ids] =
                merge_notes_and_return_ids(std::move(old_notes), std::move(notes_to_add_));

            note_editing->set_notes_in_track(track_id, std::move(merged));
            track_actions.push_back(PlaceNotes{ std::move(ids), std::nullopt, track_id });
        }

        if (!track_actions.empty()) bulk_actions.push_back(Bulk{ std::move(track_actions) });
    }

    if (!bulk_actions.empty()) {
        editor_actions.register_action(Bulk{ std::move(bulk_actions) });
    } else {
        Debugger::log("Plugin made no changes");
    }
}

}
