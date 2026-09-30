#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "editor/util.h"
#include "midi/events/note.h"

namespace andromeda::editor {

enum class SelectionOp { NewSelection, AppendSelection, RemoveFromSelection };

class SharedClipboard {
public:
    SignedMIDITick offset_from_playhead = 0;
    bool is_empty = true;

    void move_notes_to_clipboard(std::vector<Note> notes, std::uint16_t track,
                                 bool clear_clipboard);

    void move_multi_notes_to_clipboard(std::vector<std::vector<Note>> notes,
                                       const std::vector<std::uint16_t>& tracks);

    [[nodiscard]] std::vector<std::pair<std::uint16_t, std::vector<Note>>>
    get_notes_from_clipboard() const;

    void clear_clipboard();

    [[nodiscard]] bool is_clipboard_empty() const { return is_empty; }

    [[nodiscard]] MIDITick get_clipboard_start_tick() const;

private:
    std::unordered_map<std::uint16_t, std::vector<Note>> notes_clipboard_map_;
};

class SharedSelectedNotes {
public:
    [[nodiscard]] std::vector<std::uint16_t> get_active_selected_tracks() const;

    [[nodiscard]] const std::vector<std::size_t>* get_selected_ids_in_track(
        std::uint16_t track) const;

    std::vector<std::size_t>& get_selected_ids_mut(std::uint16_t track);

    [[nodiscard]] std::vector<std::pair<std::uint16_t, const std::vector<std::size_t>*>>
    get_selected() const;

    void set_selected_in_track(std::vector<std::size_t> ids, std::uint16_t track);

    void add_selected_to_track(const std::vector<std::size_t>& ids, std::uint16_t track);

    std::vector<std::size_t> take_selected_from_track(std::uint16_t track);

    std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> take_selected_from_all();

    void clear_selected();

    [[nodiscard]] bool is_any_note_selected() const;

    [[nodiscard]] bool selected_ids_in_track_contains(std::uint16_t track, std::size_t id) const;

    [[nodiscard]] std::size_t get_num_selected_in_track(std::uint16_t track) const;

    [[nodiscard]] std::uint64_t version() const { return version_; }

private:
    void bump() { ++version_; }

    std::unordered_map<std::uint16_t, std::vector<std::size_t>> selected_notes_hash_;
    std::uint64_t version_ = 1;
};

}
