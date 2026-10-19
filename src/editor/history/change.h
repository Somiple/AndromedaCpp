#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "editor/history/event_traits.h"
#include "editor/util.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"

namespace andromeda::editor::history {

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

// ---------------------------------------------------------------------------------------------
// Time-sorted per-track elements (notes, channel events, meta events) share two generic changes,
// parameterised on the element type (see EventTraits in event_traits.h).
// ---------------------------------------------------------------------------------------------

// Property edits that do not change an element's index in the (time-sorted) array.
// Start/tick changes are expressed as a PresenceChange.
template <typename Item>
struct PropertyEdit {
    EventId index = 0;
    typename EventTraits<Item>::Props before;  // what the element holds now
    typename EventTraits<Item>::Props after;   // what applying writes; swapped with `before`
};

template <typename Item>
struct PropertyChange {
    using item_type = Item;

    TrackId track = 0;
    std::vector<PropertyEdit<Item>> edits;  // ascending by index
};

// Elements appearing and disappearing in one track: add, delete and move (= delete + add) are
// the same thing.
//
//   remove_at : ascending indices into the track BEFORE applying
//   insert_at : ascending indices into the track AFTER applying, one per payload element
//   payload   : the elements to insert (moved in, so a delete stores each element exactly once)
//
// Apply: pull the elements at remove_at out of the track, insert payload at insert_at, then
// payload = the pulled-out elements and remove_at <-> insert_at. Because indices are exact, undo
// restores the array bit-for-bit, tie order included.
template <typename Item>
struct PresenceChange {
    using item_type = Item;

    TrackId track = 0;
    std::vector<EventId> remove_at;
    std::vector<EventId> insert_at;
    std::vector<Item> payload;
};

using NotePropertyEdit = PropertyEdit<midi::Note>;
using NotePropertyChange = PropertyChange<midi::Note>;
using NotePresenceChange = PresenceChange<midi::Note>;

using ChannelEventPropertyEdit = PropertyEdit<midi::ChannelEvent>;
using ChannelEventPropertyChange = PropertyChange<midi::ChannelEvent>;
using ChannelEventPresenceChange = PresenceChange<midi::ChannelEvent>;

using MetaEventPropertyEdit = PropertyEdit<midi::MetaEvent>;
using MetaEventPropertyChange = PropertyChange<midi::MetaEvent>;
using MetaEventPresenceChange = PresenceChange<midi::MetaEvent>;

template <typename T>
inline constexpr bool is_property_change_v = false;
template <typename Item>
inline constexpr bool is_property_change_v<PropertyChange<Item>> = true;

template <typename T>
inline constexpr bool is_presence_change_v = false;
template <typename Item>
inline constexpr bool is_presence_change_v<PresenceChange<Item>> = true;

// Replaces the selection of one track. Apply: selection = ids, ids = old selection.
// TODO: selection only knows about notes for now.
struct SelectionSwapChange {
    TrackId track = 0;
    std::vector<NoteId> ids;  // ascending
};

struct ActiveTrackChange {
    TrackId track = 0;
};

using Change = std::variant<TrackPresenceChange, TrackSwapChange, TrackMoveChange,
                            NotePropertyChange, NotePresenceChange,
                            ChannelEventPropertyChange, ChannelEventPresenceChange,
                            MetaEventPropertyChange, MetaEventPresenceChange,
                            SelectionSwapChange, ActiveTrackChange>;

struct Action {
    std::string label;
    std::vector<Change> changes;  // in the order they were applied while recording
    std::size_t approx_bytes = 0; // filled by ActionHistory when the action is pushed
};

// rough heap cost of a change, used for the history's memory budget
[[nodiscard]] std::size_t approx_bytes(Change& change);

// sent to ActionExecutor's observer after something was modified, so caches (gpu note buffers,
// bar cache, active track...) can refresh
struct ChangeEvent {
    ChangeKind kind = ChangeKind::Notes;
    TrackId track = 0;  // unused for ChangeKind::Tracks
};

}