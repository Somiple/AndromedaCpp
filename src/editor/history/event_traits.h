#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

#include "editor/util.h"
#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"

namespace andromeda::editor::history {

using TrackId = std::uint16_t;
using NoteId = std::size_t;
using EventId = std::size_t;

// Pseudo track id that addresses the project's GLOBAL meta events (ProjectData::global_metas:
// tempo, time/key signature, markers, lyrics...) instead of a real track. Only meaningful for
// midi::MetaEvent changes/mutators. Real track indices are therefore limited to 0..0xFFFE.
inline constexpr TrackId kGlobalTrack = 0xFFFF;

enum class ChangeKind { Tracks, Notes, ChannelEvents, MetaEvents, GlobalMetaEvents, Selection,
                        ActiveTrack };

// ---------------------------------------------------------------------------------------------
// "Props" are the fields of an element that can change WITHOUT changing where it sits in its
// (time-sorted) array. Anything that changes the time is a move = remove + insert, and is
// expressed as a PresenceChange instead.
// ---------------------------------------------------------------------------------------------

struct NoteProps {
    MIDITick length = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
    std::uint8_t channel = 0;

    bool operator==(const NoteProps&) const = default;
};

struct ChannelEventProps {
    std::uint8_t channel = 0;
    midi::ChannelEventType event_type{};

    bool operator==(const ChannelEventProps&) const = default;
};

struct MetaEventProps {
    midi::MetaEventType type = midi::MetaEventType::Text;
    std::vector<std::uint8_t> data;

    bool operator==(const MetaEventProps&) const = default;
};

// "Position" is what a queued move can change: the time, plus the key for notes.
struct NotePosition {
    MIDITick tick = 0;
    std::uint8_t key = 0;
};

struct EventPosition {
    MIDITick tick = 0;
};

// ---------------------------------------------------------------------------------------------
// EventTraits<Item>: everything the generic machinery (ActionExecutor, Transaction sessions,
// EventMutatorBase) needs to know about one kind of time-sorted track element. Adding a new kind
// of element means adding a specialization here plus a thin mutator; no other code changes.
//
//   kind      which ChangeKind observers are told about
//   accessor  name used in error messages
//   items()   the track's array for this element type (must be sorted by time())
//   position()/place()  read/write what a move can change
//   props()/write()     read/write what a property edit can change
// ---------------------------------------------------------------------------------------------
template <typename Item>
struct EventTraits;

template <>
struct EventTraits<midi::Note> {
    using Props = NoteProps;
    using Position = NotePosition;

    static constexpr ChangeKind kind = ChangeKind::Notes;
    static constexpr std::string_view accessor = "notes()";

    static std::vector<midi::Note>& items(midi::MIDITrack& t) { return t.get_notes_mut(); }

    static MIDITick time(const midi::Note& n) { return n.start; }
    static Position position(const midi::Note& n) { return Position{n.start, n.key}; }
    static void place(midi::Note& n, const Position& p) {
        n.start = p.tick;
        n.key = p.key;
    }

    static Props props(const midi::Note& n) {
        return Props{n.length, n.key, n.velocity, n.channel};
    }
    static void write(midi::Note& n, const Props& p) {
        n.length = p.length;
        n.key = p.key;
        n.velocity = p.velocity;
        n.channel = p.channel;
    }
};

template <>
struct EventTraits<midi::ChannelEvent> {
    using Props = ChannelEventProps;
    using Position = EventPosition;

    static constexpr ChangeKind kind = ChangeKind::ChannelEvents;
    static constexpr std::string_view accessor = "channel_events()";

    static std::vector<midi::ChannelEvent>& items(midi::MIDITrack& t) {
        return t.get_channel_evs_mut();
    }

    static MIDITick time(const midi::ChannelEvent& e) { return e.tick; }
    static Position position(const midi::ChannelEvent& e) { return Position{e.tick}; }
    static void place(midi::ChannelEvent& e, const Position& p) { e.tick = p.tick; }

    static Props props(const midi::ChannelEvent& e) { return Props{e.channel, e.event_type}; }
    static void write(midi::ChannelEvent& e, const Props& p) {
        e.channel = p.channel;
        e.event_type = p.event_type;
    }
};

template <>
struct EventTraits<midi::MetaEvent> {
    using Props = MetaEventProps;
    using Position = EventPosition;

    static constexpr ChangeKind kind = ChangeKind::MetaEvents;
    static constexpr std::string_view accessor = "meta_events()";

    static std::vector<midi::MetaEvent>& items(midi::MIDITrack& t) {
        return t.get_meta_events_mut();
    }

    static MIDITick time(const midi::MetaEvent& e) { return e.tick; }
    static Position position(const midi::MetaEvent& e) { return Position{e.tick}; }
    static void place(midi::MetaEvent& e, const Position& p) { e.tick = p.tick; }

    static Props props(const midi::MetaEvent& e) { return Props{e.event_type, e.data}; }
    static void write(midi::MetaEvent& e, const Props& p) {
        e.event_type = p.type;
        e.data = p.data;
    }
};

// The ChangeKind observers are told about for an edit of `Item` on `track`. Only differs from
// EventTraits<Item>::kind for global meta events.
template <typename Item>
constexpr ChangeKind kind_for(TrackId track) {
    if constexpr (std::is_same_v<Item, midi::MetaEvent>) {
        if (track == kGlobalTrack) {
            return ChangeKind::GlobalMetaEvents;
        }
    }
    return EventTraits<Item>::kind;
}

}