#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/history/action_executor.h"
#include "editor/history/action_history.h"
#include "editor/history/change.h"

namespace andromeda::editor::history {

class Transaction;
class ActionRecorder;

// Tracks first, then notes, then selection. Requesting something earlier in that order after
// something later has been requested rejects the whole transaction.
enum class TransactionState { TrackEditing, NoteEditing, SelectionChange, Committed, RolledBack };

// Returned by NoteMutator::add_note. Notes cannot get their final index until the mutator is
// flushed (tx.selection() or tx.commit()), so this is a ticket; redeem it with
// NoteMutator::final_id(AddedNote) after the flush.
struct AddedNote {
    std::size_t slot = 0;
};

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
// NoteMutator: edits to the notes of ONE track.
//
// Ids are indices into the track as it was when the mutator was created and they stay valid for
// the mutator's whole life: nothing shifts while you work.
//   * set_length / set_key / set_velocity / set_channel are applied right away (they cannot
//     change a note's index) and coalesced per note.
//   * delete_note / move_note / set_position / add_note are queued and applied together in one
//     pass when the mutator is flushed.
// Only ids passed to notes() may be touched. After the flush use final_id() to translate.
// ---------------------------------------------------------------------------------------------
class NoteMutator {
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

    void delete_note(NoteId id);
    void delete_notes(std::span<const NoteId> ids);

    AddedNote add_note(const midi::Note& note);

    // ---- valid once flushed (after tx.selection() or tx.commit()) ----

    // index of an original note afterwards; nullopt if it was deleted
    [[nodiscard]] std::optional<NoteId> final_id(NoteId original) const;
    [[nodiscard]] NoteId final_id(AddedNote added) const;
    // maps many ids at once in O(n + inserted); deleted notes are dropped, result is ascending
    [[nodiscard]] std::vector<NoteId> remap_ids(std::vector<NoteId> original_ids) const;
    // final indices (ascending) of every note this mutator added or moved
    [[nodiscard]] const std::vector<NoteId>& inserted_ids() const;

private:
    friend class Transaction;
    friend class SelectionMutator;
    NoteMutator(Transaction* tx, std::size_t session) : tx_(tx), session_(session) {}

    template <typename F>
    void edit_props(NoteId id, std::string_view op, F&& edit);

    Transaction* tx_;
    std::size_t session_;
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
    [[nodiscard]] SelectionMutator selection();

    void set_active_track(TrackId track);

    // Applies queued note edits, drops no-ops and pushes the action to the history.
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
    friend class NoteMutator;
    friend class SelectionMutator;

    Transaction(ActionHistory* history, ActionExecutor* executor, std::string label);

    struct PendingMove {
        MIDITick start = 0;
        std::uint8_t key = 0;
    };

    struct NoteSession {
        TrackId track = 0;
        std::vector<NoteId> touched;       // ascending, unique
        std::size_t props_change = 0;      // index into changes_

        // edits made so far; before = original values, after = current values
        std::unordered_map<NoteId, NotePropertyEdit> prop_edits;
        std::unordered_map<NoteId, PendingMove> moves;
        std::vector<NoteId> deletes;
        std::vector<midi::Note> adds;

        bool flushed = false;
        std::vector<NoteId> remove_at;     // as applied by the flush
        std::vector<NoteId> insert_at;
        std::unordered_map<NoteId, NoteId> moved_final;
        std::vector<NoteId> added_final;
    };

    static constexpr std::size_t kNoSession = static_cast<std::size_t>(-1);

    bool fail(std::string_view reason);
    void release_slot();

    [[nodiscard]] std::size_t track_count() const;
    [[nodiscard]] std::vector<midi::Note>& notes_of(TrackId track);
    [[nodiscard]] bool track_ops_allowed(std::string_view op);

    // applies `change` through the executor and records it (in its post-apply, inverse form)
    bool apply_and_record(Change change);

    [[nodiscard]] NoteSession* session_for_edit(std::size_t index, std::string_view op);
    [[nodiscard]] bool check_touched(const NoteSession& session, NoteId id, std::string_view op);
    void finalize_props(NoteSession& session);
    bool flush(NoteSession& session);
    bool flush_all();

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
    std::deque<NoteSession> sessions_;               // deque: references stay valid
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