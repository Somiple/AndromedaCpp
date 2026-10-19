#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "editor/history/action_executor.h"
#include "editor/history/action_history.h"
#include "editor/history/change.h"
#include "editor/history/event_traits.h"

namespace andromeda::editor::history {

class Transaction;
class ActionRecorder;

// Stages, in order:
//
//   TrackEditing -> { NoteEditing | ChannelEventEditing | MetaEventEditing }* -> SelectionChange
//
// Notes, channel events and meta events live in separate arrays, so they may be edited in any
// order, but they must not intertwine: asking for a mutator of a different kind (or for
// selection()) SEALS the mutators of the kind you were in. Sealed mutators are flushed (so
// final_id() works on them) and any further edit through them rejects the transaction.
enum class TransactionState { TrackEditing, NoteEditing, ChannelEventEditing, MetaEventEditing,
    SelectionChange, Committed, RolledBack };

// Returned by an add_* call. Elements cannot get their final index until the mutator is flushed
// (tx.selection(), tx.commit(), or starting another kind of editing), so this is a ticket; redeem
// it with final_id(Added) after the flush.
template <typename Item>
struct Added {
    std::size_t slot = 0;
};

using AddedNote = Added<midi::Note>;
using AddedChannelEvent = Added<midi::ChannelEvent>;
using AddedMetaEvent = Added<midi::MetaEvent>;

// ---------------------------------------------------------------------------------------------
// TrackMutator: structural track edits. Applied immediately (tracks are cheap to move), recorded
// as it goes. Track ids are indices, so an id returned by one call is only valid until the next
// track call. Call finalize_tracks() when done; only then may notes() / selection() be used.
// ---------------------------------------------------------------------------------------------
class TrackMutator {
public:
    // appends; returns the new track's index
    TrackId add_track(midi::MIDITrack track = midi::MIDITrack::new_empty());
    TrackId insert_track(TrackId index, midi::MIDITrack track);
    void remove_track(TrackId index);
    void swap_tracks(TrackId a, TrackId b);
    // the track at `from` ends up at index `to`
    void move_track(TrackId from, TrackId to);

    void finalize_tracks();

private:
    friend class Transaction;
    explicit TrackMutator(Transaction* tx) : tx_(tx) {}
    Transaction* tx_;
};

// ---------------------------------------------------------------------------------------------
// EventSession<Item>: the bookkeeping for ONE mutator (one track, one element kind) while the
// transaction is open. Owned by the Transaction; the mutator only holds an index.
// ---------------------------------------------------------------------------------------------
template <typename Item>
struct EventSession {
    using Position = typename EventTraits<Item>::Position;

    TrackId track = 0;
    std::vector<EventId> touched;      // ascending, unique: the only ids the mutator may use
    std::size_t props_change = 0;      // index of this session's PropertyChange in changes_

    // edits made so far; before = original values, after = current values
    std::unordered_map<EventId, PropertyEdit<Item>> prop_edits;
    bool props_finalized = false;

    // queued structural edits, applied together by the flush
    std::unordered_map<EventId, Position> moves;
    std::vector<EventId> deletes;
    std::vector<Item> adds;

    bool flushed = false;
    std::vector<EventId> remove_at;    // as applied by the flush
    std::vector<EventId> insert_at;
    std::unordered_map<EventId, EventId> moved_final;
    std::vector<EventId> added_final;
};

// ---------------------------------------------------------------------------------------------
// EventMutatorBase<Item>: everything NoteMutator, ChannelEventMutator and MetaEventMutator have
// in common. A derived mutator only adds the typed setters for its element kind.
//
// Ids are indices into the track as it was when the mutator was created and they stay valid for
// the mutator's whole life: nothing shifts while you work.
//   * property setters are applied right away (they cannot change an element's index) and
//     coalesced per element.
//   * delete / move / set_position / add are queued and applied together in one pass when the
//     mutator is flushed.
// Only ids passed when the mutator was requested may be touched. After the flush use final_id()
// to translate.
// ---------------------------------------------------------------------------------------------
template <typename Item>
class EventMutatorBase {
public:
    using Traits = EventTraits<Item>;
    using Position = typename Traits::Position;

    // ---- valid once flushed ----

    // index of an original element afterwards; nullopt if it was deleted
    [[nodiscard]] std::optional<EventId> final_id(EventId original) const;
    [[nodiscard]] EventId final_id(Added<Item> added) const;
    // maps many ids at once in O(n + inserted); deleted elements are dropped, result is ascending
    [[nodiscard]] std::vector<EventId> remap_ids(std::vector<EventId> original_ids) const;
    // final indices (ascending) of every element this mutator added or moved
    [[nodiscard]] const std::vector<EventId>& inserted_ids() const;

protected:
    EventMutatorBase(Transaction* tx, std::size_t session) : tx_(tx), session_(session) {}

    // runs `edit(Item&)` on the element and records before/after props. Edits that end up
    // changing nothing are dropped at flush, so `edit` may silently ignore elements of the wrong
    // sub-type.
    template <typename F>
    void edit_props(EventId id, std::string_view op, F&& edit);

    // the queued move target of `id` (created from the element's current position); nullptr if
    // the mutator is unusable or `id` was not declared
    [[nodiscard]] Position* pending_position(EventId id, std::string_view op);
    void set_pending_position(EventId id, const Position& position, std::string_view op);

    void erase(EventId id, std::string_view op);
    void erase_many(std::span<const EventId> ids, std::string_view op);
    template <typename Pred>
    void erase_if(std::span<const EventId> ids, Pred&& pred, std::string_view op);
    Added<Item> add(const Item& item, std::string_view op);

    Transaction* tx_;
    std::size_t session_;

private:
    friend class SelectionMutator;

    [[nodiscard]] const EventSession<Item>* flushed_session(std::string_view op) const;
};

// ---------------------------------------------------------------------------------------------
// NoteMutator: edits to the notes of ONE track.
// ---------------------------------------------------------------------------------------------
class NoteMutator : public EventMutatorBase<midi::Note> {
public:
    void set_length(NoteId id, MIDITick length);
    void set_length(std::span<const NoteId> ids, MIDITick length);
    void set_length(std::span<const NoteId> ids, std::span<MIDITick> lengths);
    void set_key(NoteId id, std::uint8_t key);
    void set_key(std::span<const NoteId> ids, std::uint8_t key);
    void set_key(std::span<const NoteId> ids, std::span<std::uint8_t> keys);
    void set_velocity(NoteId id, std::uint8_t velocity);
    void set_velocity(std::span<const NoteId> ids, std::uint8_t velocity);
    void set_velocity(std::span<const NoteId> ids, std::span<std::uint8_t> velocities);
    void set_channel(NoteId id, std::uint8_t channel);
    void set_channel(std::span<const NoteId> ids, std::uint8_t channel);
    void set_channel(std::span<const NoteId> ids, std::span<std::uint8_t> channels);

    // relative to the note's current (possibly already queued) target; clamped to 0 / 0..127
    void move_note(NoteId id, SignedMIDITick delta_ticks, std::int16_t delta_keys);
    void move_notes(std::span<const NoteId> ids, SignedMIDITick delta_ticks,
                    std::int16_t delta_keys);
    void set_position(NoteId id, MIDITick start, std::uint8_t key);

    void delete_note(NoteId id) { erase(id, "delete_note"); }
    void delete_notes(std::span<const NoteId> ids) { erase_many(ids, "delete_notes"); }
    // deletes the declared notes among `ids` for which the predicate holds
    void delete_ifs(std::span<const NoteId> ids, const std::function<bool(const midi::Note&)>& condition) {
        erase_if(ids, condition, "delete_ifs");
    }

    AddedNote add_note(const midi::Note& note) { return add(note, "add_note"); }

private:
    friend class Transaction;
    NoteMutator(Transaction* tx, std::size_t session) : EventMutatorBase(tx, session) {}
};

// ---------------------------------------------------------------------------------------------
// ChannelEventMutator: edits to the channel events of ONE track.
//
// The typed setters only touch events of the matching sub-type (set_program on a pitch bend does
// nothing). Out-of-range values are clamped.
// ---------------------------------------------------------------------------------------------
class ChannelEventMutator : public EventMutatorBase<midi::ChannelEvent> {
public:
    void set_poly_pressure(EventId id, std::uint8_t key, std::uint8_t pressure);
    void set_poly_pressure_key(EventId id, std::uint8_t key);
    void set_poly_pressure_value(EventId id, std::uint8_t pressure);

    void set_poly_pressure(std::span<const EventId> ids, std::uint8_t key, std::uint8_t pressure);
    void set_poly_pressure_key(std::span<const EventId> ids, std::uint8_t key);
    void set_poly_pressure_value(std::span<const EventId> ids, std::uint8_t pressure);

    void set_control_change(EventId id, std::uint8_t controller, std::uint8_t value);
    void set_controller(EventId id, std::uint8_t controller);
    void set_controller_value(EventId id, std::uint8_t value);

    void set_control_change(std::span<const EventId> ids, std::uint8_t controller, std::uint8_t value);
    void set_controller(std::span<const EventId> ids, std::uint8_t controller);
    void set_controller_value(std::span<const EventId> ids, std::uint8_t value);

    void set_program(EventId id, std::uint8_t program);
    void set_program(std::span<const EventId> ids, std::uint8_t program);

    void set_channel_pressure(EventId id, std::uint8_t pressure);
    void set_channel_pressure(std::span<const EventId> ids, std::uint8_t pressure);

    // raw 14-bit value, 0..16383 (8192 = centre); clamped
    void set_pitch_bend(EventId id, std::uint16_t value);
    void set_pitch_bend(std::span<const EventId> ids, std::uint16_t value);
    // -1..1 (0 = centre); clamped
    void set_pitch_bend_normalized(EventId id, float value);
    void set_pitch_bend_normalized(std::span<const EventId> ids, float value);

    // for ALL channel events
    void set_channel(EventId id, std::uint8_t channel);
    void set_channel(std::span<const EventId> ids, std::uint8_t channel);

    void move_event(EventId id, SignedMIDITick delta_ticks);
    void move_events(std::span<const EventId> ids, SignedMIDITick delta_ticks);
    void set_time(EventId id, MIDITick tick);

    void delete_event(EventId id) { erase(id, "delete_event"); }
    void delete_events(std::span<const EventId> ids) { erase_many(ids, "delete_events"); }
    // deletes the declared events among `ids` for which the predicate holds
    void delete_ifs(std::span<const EventId> ids,
                    const std::function<bool(const midi::ChannelEvent&)>& condition) {
        erase_if(ids, condition, "delete_ifs");
    }

    AddedChannelEvent add_event(const midi::ChannelEvent& event) { return add(event, "add_event"); }

private:
    friend class Transaction;
    ChannelEventMutator(Transaction* tx, std::size_t session) : EventMutatorBase(tx, session) {}

    // runs `edit(T&)` only if the event currently is a T
    template <typename T, typename F>
    void edit_as(EventId id, std::string_view op, F&& edit);
};

// ---------------------------------------------------------------------------------------------
// MetaEventMutator: edits to the meta events (tempo, time signature, markers...) of ONE track, or
// of the project's global meta events (Transaction::global_meta_events()).
//
// The typed setters only touch events of the matching type; set_data works on any of them.
// ---------------------------------------------------------------------------------------------
class MetaEventMutator : public EventMutatorBase<midi::MetaEvent> {
public:
    // raw payload, any meta event type
    void set_data(EventId id, std::span<const std::uint8_t> data);
    // text-like events only (Text, Copyright, TrackName, InstrumentName, Lyric, Marker, CuePoint,
    // ProgramName, DeviceName)
    void set_text(EventId id, std::string_view text);
    // Tempo events only; beats per minute, ignored unless > 0
    void set_tempo(EventId id, float bpm);
    // TimeSignature events only; the denominator is a power of two (2 = quarter note)
    void set_time_signature(EventId id, std::uint8_t numerator, std::uint8_t denominator_power,
                            std::uint8_t clocks_per_click = 24, std::uint8_t notated_32nds = 8);
    // KeySignature events only; sharps_flats is -7..7 (negative = flats)
    void set_key_signature(EventId id, std::int8_t sharps_flats, bool minor);

    void move_event(EventId id, SignedMIDITick delta_ticks);
    void move_events(std::span<const EventId> ids, SignedMIDITick delta_ticks);
    void set_time(EventId id, MIDITick tick);

    void delete_event(EventId id) { erase(id, "delete_event"); }
    void delete_events(std::span<const EventId> ids) { erase_many(ids, "delete_events"); }
    void delete_ifs(std::span<const EventId> ids,
                    const std::function<bool(const midi::MetaEvent&)>& condition) {
        erase_if(ids, condition, "delete_ifs");
    }

    AddedMetaEvent add_event(const midi::MetaEvent& event) { return add(event, "add_event"); }

private:
    friend class Transaction;
    MetaEventMutator(Transaction* tx, std::size_t session) : EventMutatorBase(tx, session) {}
};

// ---------------------------------------------------------------------------------------------
// SelectionMutator: selection edits, in the FINAL note index space (after note edits).
// ---------------------------------------------------------------------------------------------
class SelectionMutator {
public:
    void set(TrackId track, std::span<const NoteId> ids);
    void add(TrackId track, std::span<const NoteId> ids);
    void remove(TrackId track, std::span<const NoteId> ids);
    void clear_track(TrackId track);
    void clear_all();

    // carries the track's existing selection through a note mutator's edits: deleted notes drop
    // out, shifted/moved notes follow their new index
    void remap(const NoteMutator& notes);

private:
    friend class Transaction;
    explicit SelectionMutator(Transaction* tx) : tx_(tx) {}
    Transaction* tx_;
};

// ---------------------------------------------------------------------------------------------
// Transaction: records one user-level operation. Non-copyable, non-movable (mutators point at
// it). Leaving scope without commit() rolls everything back and logs a warning.
// ---------------------------------------------------------------------------------------------
class Transaction {
public:
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    ~Transaction();

    [[nodiscard]] TrackMutator tracks();
    [[nodiscard]] NoteMutator notes(TrackId track, std::span<const NoteId> ids);
    [[nodiscard]] ChannelEventMutator channel_events(TrackId track, std::span<const EventId> ids);
    [[nodiscard]] MetaEventMutator meta_events(TrackId track, std::span<const EventId> ids);
    // The project's GLOBAL meta events (tempo, time/key signature, markers, lyrics...). Ids are
    // indices into ProjectData::global_metas. Same stage as meta_events(): may be mixed with it
    // and with the other element kinds in any order, but must not intertwine with them.
    [[nodiscard]] MetaEventMutator global_meta_events(std::span<const EventId> ids);
    [[nodiscard]] SelectionMutator selection();

    void set_active_track(TrackId track);

    // Applies queued edits, drops no-ops and pushes the action to the history.
    // False means the transaction was rejected and everything has been rolled back.
    bool commit();

    // undo everything done so far; the transaction cannot be used afterwards
    void rollback(std::string_view reason = "explicit rollback");

    [[nodiscard]] TransactionState state() const { return state_; }
    [[nodiscard]] bool is_active() const {
        return state_ != TransactionState::Committed && state_ != TransactionState::RolledBack;
    }
    [[nodiscard]] const std::string& label() const { return label_; }

private:
    friend class ActionRecorder;
    friend class TrackMutator;
    friend class SelectionMutator;
    template <typename Item>
    friend class EventMutatorBase;

    Transaction(ActionHistory* history, ActionExecutor* executor, std::string label);

    static constexpr std::size_t kNoSession = static_cast<std::size_t>(-1);

    static constexpr TransactionState editing_state(ChangeKind kind) {
        switch (kind) {
        case ChangeKind::Notes:         return TransactionState::NoteEditing;
        case ChangeKind::ChannelEvents: return TransactionState::ChannelEventEditing;
        default:                        return TransactionState::MetaEventEditing;
        }
    }

    bool fail(std::string_view reason);
    void release_slot();

    [[nodiscard]] std::size_t track_count() const;
    [[nodiscard]] bool track_ops_allowed(std::string_view op);

    // applies `change` through the executor and records it (in its post-apply, inverse form)
    bool apply_and_record(Change change);

    // ---- event sessions (one set of these per element kind, selected by Item) ----

    template <typename Item>
    [[nodiscard]] std::deque<EventSession<Item>>& sessions() {
        return std::get<std::deque<EventSession<Item>>>(sessions_);
    }
    // callers have validated `track` (kGlobalTrack addresses ProjectData::global_metas)
    template <typename Item>
    [[nodiscard]] std::vector<Item>& items_of(TrackId track) {
        return *items_for<Item>(*executor_->context().project, track);
    }
    template <typename Item>
    [[nodiscard]] bool event_track_exists(TrackId track) {
        return items_for<Item>(*executor_->context().project, track) != nullptr;
    }

    // validates the request, seals the previous kind if needed, and returns the new session's
    // index, or kNoSession (after failing the transaction) if the request is invalid
    template <typename Item>
    [[nodiscard]] std::size_t open_session(TrackId track, std::span<const EventId> ids);
    template <typename Item>
    [[nodiscard]] EventSession<Item>* session_for_edit(std::size_t index, std::string_view op);
    template <typename Item>
    [[nodiscard]] bool check_touched(const EventSession<Item>& session, EventId id,
                                     std::string_view op);
    template <typename Item>
    void finalize_props(EventSession<Item>& session);
    template <typename Item>
    void finalize_unflushed();
    template <typename Item>
    bool flush(EventSession<Item>& session);
    template <typename Item>
    bool flush_kind();
    bool flush_all();

    // ---- selection ----

    [[nodiscard]] bool selection_ready(std::string_view op);
    [[nodiscard]] bool valid_selection_ids(TrackId track, const std::vector<NoteId>& ids,
                                           std::string_view op);
    void snapshot_selection(TrackId track);
    [[nodiscard]] std::vector<NoteId> current_selection(TrackId track) const;
    void write_selection(TrackId track, std::vector<NoteId> ids);

    ActionHistory* history_;
    ActionExecutor* executor_;
    std::string label_;
    TransactionState state_ = TransactionState::TrackEditing;
    bool owns_slot_ = false;

    bool tracks_requested_ = false;
    bool tracks_finalized_ = false;

    std::vector<Change> changes_;
    // deques: references stay valid while sessions are added
    std::tuple<std::deque<EventSession<midi::Note>>,
               std::deque<EventSession<midi::ChannelEvent>>,
               std::deque<EventSession<midi::MetaEvent>>> sessions_;
    std::unordered_map<TrackId, std::size_t> selection_snapshots_;  // track -> index in changes_
};

// Entry point: owns nothing, just pairs the history with the executor.
class ActionRecorder {
public:
    ActionRecorder(ActionHistory* history, ActionExecutor* executor)
        : history_(history), executor_(executor) {}

    [[nodiscard]] Transaction begin(std::string label) {
        return Transaction(history_, executor_, std::move(label));
    }

private:
    ActionHistory* history_;
    ActionExecutor* executor_;
};

}