#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "editor/util.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"

namespace andromeda::editor {

struct EditorAction;

struct PlaceNotes {
    std::vector<std::size_t> note_ids;
    std::optional<std::vector<Note>> notes;
    std::uint16_t note_group;
};

struct DeleteNotes {
    std::vector<std::size_t> note_ids;
    std::optional<std::vector<Note>> notes;
    std::uint16_t note_group;
};

struct PlaceNotesMultiTrack {
    std::vector<std::vector<std::size_t>> note_ids;
    std::optional<std::vector<std::vector<Note>>> notes;
    std::vector<std::uint16_t> note_groups;
};

struct DeleteNotesMultiTrack {
    std::vector<std::vector<std::size_t>> note_ids;
    std::optional<std::vector<std::vector<Note>>> notes;
    std::vector<std::uint16_t> note_groups;
};

struct LengthChange {
    std::vector<std::size_t> note_ids;
    std::vector<SignedMIDITick> length_delta;
    std::uint16_t note_group;
};

struct VelocityChange {
    std::vector<std::size_t> note_ids;
    std::vector<std::int8_t> velocity_delta;
    std::uint16_t note_group;
};

struct ChannelChange {
    std::vector<std::size_t> note_ids;
    std::vector<std::int8_t> channel_delta;
    std::uint16_t note_group;
};

struct KeyChange {
    std::vector<std::size_t> note_ids;
    std::vector<SignedMIDIKey> key_delta;
    std::uint16_t note_group;
};

struct NotesMove {
    std::vector<std::size_t> note_ids;
    std::vector<std::pair<SignedMIDITick, std::int16_t>> midi_pos_delta;
    std::uint16_t note_group;
    bool update_selected_ids;
};

struct NotesMoveImmediate {
    std::vector<std::size_t> note_ids;
    std::vector<std::pair<SignedMIDITick, std::int16_t>> midi_pos_delta;
    std::uint16_t note_group;
};

struct NotesMoveMultiTrack {
    std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> note_track_ids;
    std::pair<SignedMIDITick, std::int16_t> moved_by;
};

struct Select {
    std::vector<std::size_t> note_ids;
    std::uint16_t note_group;
};

struct Deselect {
    std::vector<std::size_t> note_ids;
    std::uint16_t note_group;
};

struct Duplicate {
    std::vector<std::size_t> note_ids;
    MIDITick paste_tick;
    std::uint16_t source_track;
    std::uint16_t dest_track;
};

struct AddMeta {
    std::vector<std::size_t> meta_ids;
    std::optional<std::vector<MetaEvent>> metas;
};

struct DeleteMeta {
    std::vector<std::size_t> meta_ids;
    std::optional<std::vector<MetaEvent>> metas;
};

struct AddTrack {
    std::uint16_t track;
    std::optional<std::deque<midi::MIDITrack>> deleted_tracks;
    bool last_track;
};

struct RemoveTrack {
    std::uint16_t track;
    std::optional<std::deque<midi::MIDITrack>> deleted_tracks;
    bool last_track;
};

struct SwapTracks {
    std::uint16_t track_1;
    std::uint16_t track_2;
};

struct DecomposeTrack {
    std::uint16_t track;
    std::uint16_t channel_count;
};

struct ComposeTrack {
    std::uint16_t track;
    std::uint16_t channel_count;
};

struct Bulk {
    std::vector<EditorAction> actions;
};

using EditorActionVariant = std::variant<
    PlaceNotes, DeleteNotes, PlaceNotesMultiTrack, DeleteNotesMultiTrack, LengthChange,
    VelocityChange, ChannelChange, KeyChange, NotesMove, NotesMoveImmediate, NotesMoveMultiTrack,
    Select, Deselect, Duplicate, AddMeta, DeleteMeta, AddTrack, RemoveTrack, SwapTracks,
    DecomposeTrack, ComposeTrack, Bulk>;

struct EditorAction {
    EditorActionVariant node;

    EditorAction() = default;

    template <typename T>
        requires(!std::is_same_v<std::decay_t<T>, EditorAction>)
    EditorAction(T&& v) : node(std::forward<T>(v)) {}
};

class EditorActions {
public:
    EditorActions() = default;
    explicit EditorActions(std::uint16_t max_actions) : max_actions_(max_actions) {}

    void register_action(EditorAction action);

    EditorAction* undo_action();

    EditorAction* redo_action();

    [[nodiscard]] bool get_can_undo() const;
    [[nodiscard]] bool get_can_redo() const;

    void clear_actions();

private:
    EditorAction invert_action(EditorAction action);

    std::deque<EditorAction> actions_;
    std::uint16_t max_actions_ = 10;
    std::uint16_t undo_depth_ = 0;
};

}
