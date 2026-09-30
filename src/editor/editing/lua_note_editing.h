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

class NoteEditing;

class LuaNoteEditing {
public:
    explicit LuaNoteEditing(std::shared_ptr<NoteEditing> note_editing);

    static void register_types(sol::state& lua);

    void apply_changes(std::uint16_t track, EditorActions& editor_actions);

    void for_each_note(sol::this_state state, sol::protected_function func);
    void for_each_selected(sol::this_state state, sol::protected_function func);
    void iter_selected(sol::this_state state, sol::protected_function func);
    sol::object get_selection_tick_range(sol::this_state state, bool inclusive);
    sol::object get_selection_key_range(sol::this_state state);
    void create_note(MIDITick start, MIDITick length, std::uint8_t channel, std::uint8_t key,
                     std::uint8_t velocity);

    std::shared_ptr<NoteEditing> note_editing;
    std::unordered_map<std::size_t, std::pair<SignedMIDITick, std::int16_t>> delta_note_pos;
    std::unordered_map<std::size_t, SignedMIDITick> delta_note_lengths;
    std::unordered_map<std::size_t, std::int8_t> delta_note_channels;
    std::unordered_map<std::size_t, std::int8_t> delta_note_velocities;

    std::vector<midi::Note> notes_to_add;

private:
    void change_note_and_update_deltas(const sol::protected_function& func, midi::Note& note,
                                       std::size_t id);

    [[nodiscard]] static std::size_t current_track(sol::state_view lua);
};

}
