#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "editor/util.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"

namespace andromeda::editor::history {

using TrackId = std::uint16_t;
using NoteId = std::size_t;

// ---------------------------------------------------------------------------------------------
// Every change is an INVOLUTION: ActionExecutor::apply() performs it and then rewrites the struct
// in place into its own inverse. So there is no invert_action(): undo is "apply the changes of an
// action in reverse order", redo is "apply them again in forward order", and each application
// moves data (notes, tracks, id lists) between the project and the change instead of copying it.
//
// A change as stored in the history is always the one that would UNDO what was last done.
// ---------------------------------------------------------------------------------------------

// Insert or remove ONE track.
//   payload engaged   -> apply inserts it at `index`, change becomes a removal
//   payload empty     -> apply removes the track at `index` into payload, change becomes an insert
struct TrackPresenceChange {
    TrackId index = 0;
    std::optional<midi::MIDITrack> payload;
};

struct TrackSwapChange {
    TrackId a = 0;
    TrackId b = 0;
};

// Move the track at `from` so it ends up at `to`. Applying flips the two.
struct TrackMoveChange {
    TrackId from = 0;
    TrackId to = 0;
};

// The note fields that can change without changing the note's position in the (start-sorted)
// note array. Start changes are expressed as a NotePresenceChange.
struct NoteProps {
    MIDITick length = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
    std::uint8_t channel = 0;

    bool operator==(const NoteProps&) const = default;
};

struct NotePropertyEdit {
    NoteId index = 0;
    NoteProps before;  // what the note holds now
    NoteProps after;   // what applying writes; swapped with `before` afterwards
};

struct NotePropertyChange {
    TrackId track = 0;
    std::vector<NotePropertyEdit> edits;  // ascending by index
};

// Notes appearing and disappearing in one track: add, delete and move (= delete + add) are the
// same thing. Replaces PlaceNotes / DeleteNotes / PlaceNotesMove...
//
//   remove_at : ascending indices into the track BEFORE applying
//   insert_at : ascending indices into the track AFTER applying, one per payload note
//   payload   : the notes to insert (moved in, so a delete stores each note exactly once)
//
// Apply: pull the notes at remove_at out of the track, insert payload at insert_at, then
// payload = the pulled-out notes and remove_at <-> insert_at. Because indices are exact, undo
// restores the array bit-for-bit, tie order included.
struct NotePresenceChange {
    TrackId track = 0;
    std::vector<NoteId> remove_at;
    std::vector<NoteId> insert_at;
    std::vector<midi::Note> payload;
};

// Replaces the selection of one track. Apply: selection = ids, ids = old selection.
struct SelectionSwapChange {
    TrackId track = 0;
    std::vector<NoteId> ids;  // ascending
};

using Change = std::variant<TrackPresenceChange, TrackSwapChange, TrackMoveChange,
                            NotePropertyChange, NotePresenceChange, SelectionSwapChange>;

struct Action {
    std::string label;
    std::vector<Change> changes;  // in the order they were applied while recording
    std::size_t approx_bytes = 0; // filled by ActionHistory when the action is pushed
};

// rough heap cost of a change, used for the history's memory budget
[[nodiscard]] std::size_t approx_bytes(Change& change);

enum class ChangeKind { Tracks, Notes, Selection };

// sent to ActionExecutor's observer after something was modified, so caches (gpu note buffers,
// bar cache, active track...) can refresh
struct ChangeEvent {
    ChangeKind kind = ChangeKind::Notes;
    TrackId track = 0;  // unused for ChangeKind::Tracks
};

}