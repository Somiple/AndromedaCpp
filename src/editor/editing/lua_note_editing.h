#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "editor/actions.h"
#include "editor/util.h"
#include "midi/events/note.h"

namespace andromeda::editor {

class EditorController;

class NoteEditing;

class LuaNoteEditing {
public:
    explicit LuaNoteEditing(EditorController* controller)
        : _controller(controller) { }

    static void register_types(sol::state& lua);

    void apply_changes(EditorActions& editor_actions);

    void for_each_note(sol::this_state state, sol::protected_function func);
    void for_each_selected(sol::this_state state, sol::protected_function func);
    void for_each_selected_all(sol::this_state state, sol::protected_function func);
    void iter_selected(sol::this_state state, sol::protected_function func);
    void iter_selected_all(sol::this_state state, sol::protected_function func);
    sol::object get_selection_tick_range(sol::this_state state, bool inclusive);
    sol::object get_selection_key_range(sol::this_state state);
    void create_note(std::uint16_t track, MIDITick start, MIDITick length, std::uint8_t channel, std::uint8_t key,
                     std::uint8_t velocity);

    EditorController* _controller;
private:
    struct PendingNoteEdits {
        std::unordered_map<std::size_t, std::int8_t> velocities;
        std::unordered_map<std::size_t, std::int8_t> channels;
        std::unordered_map<std::size_t, SignedMIDITick> lengths;
        std::unordered_map<std::size_t,
            std::pair<SignedMIDITick, std::int16_t>> positions;

        std::vector<midi::Note> notes_to_add;
    };

    void change_note_and_update_deltas(const sol::protected_function& func, midi::Note& note,
                                       std::size_t id, PendingNoteEdits& edits);

    [[nodiscard]] static std::size_t current_track(sol::state_view lua);

    std::unordered_map<std::uint16_t, PendingNoteEdits> pending_edits;
};

}
