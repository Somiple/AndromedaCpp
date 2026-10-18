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

bool same_note(const Note& a, const Note& b) {
    return a.start == b.start && a.length == b.length && a.key == b.key &&
        a.channel == b.channel && a.velocity == b.velocity;
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

void LuaNoteEditing::edit_working_copy(const sol::protected_function& func, const Note& original,
    std::size_t id, PendingTrackEdits& edits) {
    const auto it = edits.modified.empty() ? edits.modified.end() : edits.modified.find(id);
    Note working = it != edits.modified.end() ? it->second : original;
    const Note before = working;

    check(func(&working));  // throws on a Lua error; nothing in the project has changed

    if (!same_note(working, before)) {
        edits.modified.insert_or_assign(id, working);
    }
}

Note LuaNoteEditing::view_note(std::uint16_t track, std::size_t id, const Note& original) const {
    const auto t = pending_edits.find(track);
    if (t != pending_edits.end()) {
        const auto n = t->second.modified.find(id);
        if (n != t->second.modified.end()) return n->second;
    }
    return original;
}

void LuaNoteEditing::rollback() { pending_edits.clear(); }

void LuaNoteEditing::for_each_note(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    if (curr_track >= tracks.size()) {
        throw std::runtime_error("curr_track is out of range");
    }

    auto& pending = pending_edits[static_cast<std::uint16_t>(curr_track)];
    const std::vector<Note>& notes = tracks[curr_track].get_notes();

    for (std::size_t i = 0; i < notes.size(); ++i) {
        edit_working_copy(func, notes[i], i, pending);
    }
}

void LuaNoteEditing::for_each_selected(sol::this_state state, sol::protected_function func) {
    const std::size_t curr_track = current_track(sol::state_view(state));

    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    SharedSelectedNotes& selection = *_controller->get_selection();
    if (curr_track >= tracks.size()) {
        throw std::runtime_error("curr_track is out of range");
    }

    auto& pending = pending_edits[static_cast<std::uint16_t>(curr_track)];
    const std::vector<Note>& notes = tracks[curr_track].get_notes();

    for (const std::size_t id :
    selected_ids_or_empty(selection, static_cast<std::uint16_t>(curr_track))) {
        if (id >= notes.size()) continue;
        edit_working_copy(func, notes[id], id, pending);
    }
}

void LuaNoteEditing::for_each_selected_all(sol::this_state, sol::protected_function func) {
    auto& tracks = *_controller->get_project_manager()->get_tracks();
    auto& selection = *_controller->get_selection();

    for (std::size_t track_id = 0; track_id < tracks.size(); ++track_id) {
        const auto& sel_ids =
            selected_ids_or_empty(selection, static_cast<std::uint16_t>(track_id));
        if (sel_ids.empty()) continue;

        auto& pending = pending_edits[static_cast<std::uint16_t>(track_id)];
        const auto& notes = tracks[track_id].get_notes();

        for (const std::size_t id : sel_ids) {
            if (id >= notes.size()) continue;
            edit_working_copy(func, notes[id], id, pending);
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
    const std::vector<Note>& notes = tracks[curr_track].get_notes();

    for (const std::size_t id :
    selected_ids_or_empty(selection, static_cast<std::uint16_t>(curr_track))) {
        if (id >= notes.size()) continue;
        Note note = view_note(static_cast<std::uint16_t>(curr_track), id, notes[id]);
        check(func(&note));
    }
}

void LuaNoteEditing::iter_selected_all(sol::this_state, sol::protected_function func) {
    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();
    SharedSelectedNotes& selection = *_controller->get_selection();

    for (std::size_t track_id = 0; track_id < tracks.size(); ++track_id) {
        const auto& sel_ids =
            selected_ids_or_empty(selection, static_cast<std::uint16_t>(track_id));
        const auto& notes = tracks[track_id].get_notes();

        for (const std::size_t id : sel_ids) {
            if (id >= notes.size()) continue;
            Note note = view_note(static_cast<std::uint16_t>(track_id), id, notes[id]);
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

void LuaNoteEditing::create_note(std::uint16_t track, MIDITick start, MIDITick length,
    std::uint8_t channel, std::uint8_t key, std::uint8_t velocity) {
    pending_edits[track].notes_to_add.emplace_back(start, length, key, channel, velocity);
}


void LuaNoteEditing::apply_changes() {
    std::vector<midi::MIDITrack>& tracks = *_controller->get_project_manager()->get_tracks();

    struct TrackPlan {
        std::uint16_t track;
        std::vector<std::size_t> ids;  // notes that really differ from the project
    };
    std::vector<TrackPlan> plans;
    bool any = false;

    // decide what actually changed before opening a transaction
    for (auto& [track_id, edits] : pending_edits) {
        if (track_id >= tracks.size()) continue;
        const auto& notes = tracks[track_id].get_notes();

        TrackPlan plan{ track_id, {} };
        for (const auto& [id, working] : edits.modified) {
            if (id < notes.size() && !same_note(notes[id], working)) plan.ids.push_back(id);
        }
        std::sort(plan.ids.begin(), plan.ids.end());

        if (!plan.ids.empty() || !edits.notes_to_add.empty()) {
            any = true;
            plans.push_back(std::move(plan));
        }
    }

    if (!any) {
        Debugger::log("Plugin made no changes");
        pending_edits.clear();
        return;
    }

    auto tx = _controller->begin_action("Run lua plugin");

    for (const TrackPlan& plan : plans) {
        PendingTrackEdits& edits = pending_edits[plan.track];
        const auto& notes = tracks[plan.track].get_notes();

        auto mutator = tx.notes(plan.track, plan.ids);  // once per track

        for (const std::size_t id : plan.ids) {
            const Note& orig = notes[id];
            const Note& w = edits.modified.at(id);

            if (w.start != orig.start) {
                mutator.set_position(id, w.start, w.key);
            }
            else if (w.key != orig.key) {
                mutator.set_key(id, static_cast<std::uint8_t>(std::min<int>(w.key, 127)));
            }
            if (w.length != orig.length) {
                mutator.set_length(id, std::max<MIDITick>(w.length, 1));
            }
            if (w.velocity != orig.velocity) {
                mutator.set_velocity(id, static_cast<std::uint8_t>(std::clamp<int>(w.velocity, 1, 127)));
            }
            if (w.channel != orig.channel) {
                mutator.set_channel(id, static_cast<std::uint8_t>(std::min<int>(w.channel, 15)));
            }
        }

        for (const Note& note : edits.notes_to_add) {
            mutator.add_note(note);
        }
    }

    if (!tx.commit()) {
        Debugger::log_error("Failed to commit Lua plugin changes transaction");
    }

    pending_edits.clear();
}

}
