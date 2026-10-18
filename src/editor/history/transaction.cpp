#include "editor/history/transaction.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

#include "util/debugger.h"

namespace andromeda::editor::history {

using midi::MIDITrack;
using midi::Note;
using util::Debugger;

namespace {

NoteProps props_of(const Note& n) { return NoteProps{n.length, n.key, n.velocity, n.channel}; }

std::vector<NoteId> sorted_unique(std::span<const NoteId> ids) {
    std::vector<NoteId> out(ids.begin(), ids.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}

// =============================================================================================
// Transaction: lifetime
// =============================================================================================

Transaction::Transaction(ActionHistory* history, ActionExecutor* executor, std::string label)
    : history_(history), executor_(executor), label_(std::move(label)) {
    if (history_->busy()) {
        Debugger::log_warning(std::format(
            "Transaction '{}' rejected: another transaction is open or an undo is running",
            label_));
        state_ = TransactionState::RolledBack;
        return;
    }
    history_->transaction_open_ = true;
    owns_slot_ = true;
}

Transaction::~Transaction() {
    if (is_active()) {
        rollback("went out of scope without commit()");
    }
    release_slot();
}

void Transaction::release_slot() {
    if (owns_slot_) {
        history_->transaction_open_ = false;
        owns_slot_ = false;
    }
}

bool Transaction::fail(std::string_view reason) {
    rollback(reason);
    return false;
}

void Transaction::rollback(std::string_view reason) {
    if (!is_active()) {
        return;
    }
    Debugger::log_warning(
        std::format("Transaction '{}' rolled back: {}", label_, std::string(reason)));

    // queued structural edits are simply dropped; live property edits need to be in changes_
    for (NoteSession& s : sessions_) {
        if (!s.flushed) {
            finalize_props(s);
        }
    }

    state_ = TransactionState::RolledBack;
    for (std::size_t i = changes_.size(); i-- > 0;) {
        if (!executor_->apply(changes_[i])) {
            Debugger::log_error("Rollback failed to revert a change; the project may be "
                                "inconsistent");
        }
    }
    changes_.clear();
    sessions_.clear();
    selection_snapshots_.clear();
    release_slot();
}

std::size_t Transaction::track_count() const {
    return executor_->context().project->get_tracks()->size();
}

std::vector<Note>& Transaction::notes_of(TrackId track) {
    return (*executor_->context().project->get_tracks())[track].get_notes_mut();
}

bool Transaction::apply_and_record(Change change) {
    if (!executor_->apply(change)) {
        return fail("a change could not be applied (invalid track or note index)");
    }
    changes_.push_back(std::move(change));
    return true;
}

// =============================================================================================
// Transaction: stage transitions
// =============================================================================================

TrackMutator Transaction::tracks() {
    if (is_active()) {
        if (state_ != TransactionState::TrackEditing) {
            fail("track mutations are not allowed after note editing has begun");
        } else if (tracks_finalized_) {
            fail("tracks() requested again after finalize_tracks()");
        } else {
            tracks_requested_ = true;
        }
    }
    return TrackMutator(this);
}

NoteMutator Transaction::notes(TrackId track, std::span<const NoteId> ids) {
    if (!is_active()) {
        return NoteMutator(this, kNoSession);
    }
    if (state_ == TransactionState::SelectionChange) {
        fail("notes() requested after selection()");
        return NoteMutator(this, kNoSession);
    }
    if (tracks_requested_ && !tracks_finalized_) {
        fail("notes() requested before finalize_tracks()");
        return NoteMutator(this, kNoSession);
    }
    if (track >= track_count()) {
        fail(std::format("notes() on track {} which does not exist", track));
        return NoteMutator(this, kNoSession);
    }
    for (const NoteSession& s : sessions_) {
        if (s.track == track) {
            fail(std::format("notes() requested twice for track {}", track));
            return NoteMutator(this, kNoSession);
        }
    }

    std::vector<NoteId> touched = sorted_unique(ids);
    if (!touched.empty() && touched.back() >= notes_of(track).size()) {
        fail(std::format("notes() was given a note id past the end of track {}", track));
        return NoteMutator(this, kNoSession);
    }

    state_ = TransactionState::NoteEditing;

    NoteSession& s = sessions_.emplace_back();
    s.track = track;
    s.touched = std::move(touched);
    s.props_change = changes_.size();
    changes_.emplace_back(NotePropertyChange{track, {}});

    return NoteMutator(this, sessions_.size() - 1);
}

SelectionMutator Transaction::selection() {
    if (!is_active()) {
        return SelectionMutator(this);
    }
    if (tracks_requested_ && !tracks_finalized_) {
        fail("selection() requested before finalize_tracks()");
        return SelectionMutator(this);
    }
    if (state_ != TransactionState::SelectionChange) {
        if (!flush_all()) {
            return SelectionMutator(this);
        }
        state_ = TransactionState::SelectionChange;
    }
    return SelectionMutator(this);
}

bool Transaction::commit() {
    if (!is_active()) {
        if (state_ == TransactionState::Committed) {
            Debugger::log_warning(std::format("Transaction '{}' was already committed", label_));
        }
        return false;
    }
    if (tracks_requested_ && !tracks_finalized_) {
        // harmless, nothing came after the track edits, but it is worth flagging
        Debugger::log_warning(
            std::format("Transaction '{}': finalize_tracks() was never called", label_));
    }
    if (!flush_all()) {
        return false;
    }

    // drop changes that turned out to be no-ops
    const auto is_noop = [this](Change& change) {
        if (auto* props = std::get_if<NotePropertyChange>(&change)) {
            return props->edits.empty();
        }
        if (auto* sel = std::get_if<SelectionSwapChange>(&change)) {
            const std::vector<NoteId> now = current_selection(sel->track);
            return now == sel->ids;
        }
        if (auto* pres = std::get_if<NotePresenceChange>(&change)) {
            return pres->remove_at.empty() && pres->insert_at.empty();
        }
        return false;
    };
    changes_.erase(std::remove_if(changes_.begin(), changes_.end(), is_noop), changes_.end());

    state_ = TransactionState::Committed;
    release_slot();

    if (!changes_.empty()) {
        Action action;
        action.label = label_;
        action.changes = std::move(changes_);
        history_->push(std::move(action));
    }
    changes_.clear();
    return true;
}

// =============================================================================================
// Transaction: note sessions
// =============================================================================================

Transaction::NoteSession* Transaction::session_for_edit(std::size_t index, std::string_view op) {
    if (!is_active() || index == kNoSession) {
        return nullptr;
    }
    if (state_ != TransactionState::NoteEditing || index >= sessions_.size()) {
        fail(std::format("{} used outside the note editing stage", op));
        return nullptr;
    }
    NoteSession& s = sessions_[index];
    if (s.flushed) {
        fail(std::format("{} used on a note mutator that was already flushed", op));
        return nullptr;
    }
    return &s;
}

bool Transaction::check_touched(const NoteSession& session, NoteId id, std::string_view op) {
    if (!std::binary_search(session.touched.begin(), session.touched.end(), id)) {
        fail(std::format("{} on note {} which was not declared in notes()", op, id));
        return false;
    }
    return true;
}

void Transaction::finalize_props(NoteSession& s) {
    auto& change = std::get<NotePropertyChange>(changes_[s.props_change]);
    change.edits.clear();
    change.edits.reserve(s.prop_edits.size());

    for (auto& [id, edit] : s.prop_edits) {
        if (edit.before == edit.after) {
            continue;
        }
        // stored in inverse form: applying it writes the ORIGINAL values back
        NotePropertyEdit inverse;
        inverse.index = id;
        inverse.before = edit.after;
        inverse.after = edit.before;
        change.edits.push_back(inverse);
    }
    s.prop_edits.clear();

    std::sort(change.edits.begin(), change.edits.end(),
              [](const NotePropertyEdit& a, const NotePropertyEdit& b) { return a.index < b.index; });

    if (!change.edits.empty()) {
        executor_->notify({ChangeKind::Notes, s.track});
    }
}

bool Transaction::flush(NoteSession& s) {
    if (s.flushed) {
        return true;
    }
    finalize_props(s);

    std::vector<Note>& notes = notes_of(s.track);

    std::sort(s.deletes.begin(), s.deletes.end());
    s.deletes.erase(std::unique(s.deletes.begin(), s.deletes.end()), s.deletes.end());

    struct Insert {
        Note note;
        std::size_t origin;  // original id for a moved note, add slot for an added one
        bool moved;
    };

    std::vector<NoteId> remove_at = s.deletes;
    std::vector<Insert> inserts;

    // deterministic order regardless of hash iteration
    std::vector<std::pair<NoteId, PendingMove>> moves(s.moves.begin(), s.moves.end());
    std::sort(moves.begin(), moves.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [id, move] : moves) {
        if (std::binary_search(s.deletes.begin(), s.deletes.end(), id)) {
            continue;  // deleting wins over moving
        }
        Note moved = notes[id];  // includes any property edits made through this mutator
        moved.start = move.start;
        moved.key = move.key;
        remove_at.push_back(id);
        inserts.push_back({moved, id, true});
    }
    for (std::size_t slot = 0; slot < s.adds.size(); ++slot) {
        inserts.push_back({s.adds[slot], slot, false});
    }

    std::sort(remove_at.begin(), remove_at.end());
    std::stable_sort(inserts.begin(), inserts.end(),
                     [](const Insert& a, const Insert& b) { return a.note.start < b.note.start; });

    // Where does each insert land? Among the notes that stay, it goes after every note whose
    // start is <= its own; earlier inserts of the same pass sit in front of it.
    std::vector<NoteId> insert_at(inserts.size());
    s.added_final.assign(s.adds.size(), 0);
    s.moved_final.clear();
    for (std::size_t j = 0; j < inserts.size(); ++j) {
        const MIDITick start = inserts[j].note.start;
        const auto upper = std::upper_bound(notes.begin(), notes.end(), start,
                                            [](MIDITick t, const Note& n) { return t < n.start; });
        const auto upper_index = static_cast<std::size_t>(upper - notes.begin());
        const auto removed_before = static_cast<std::size_t>(
            std::lower_bound(remove_at.begin(), remove_at.end(), upper_index) - remove_at.begin());

        insert_at[j] = upper_index - removed_before + j;
        if (inserts[j].moved) {
            s.moved_final[inserts[j].origin] = insert_at[j];
        } else {
            s.added_final[inserts[j].origin] = insert_at[j];
        }
    }

    s.remove_at = remove_at;
    s.insert_at = insert_at;

    if (!remove_at.empty() || !inserts.empty()) {
        NotePresenceChange change;
        change.track = s.track;
        change.remove_at = std::move(remove_at);
        change.insert_at = std::move(insert_at);
        change.payload.reserve(inserts.size());
        for (Insert& ins : inserts) {
            change.payload.push_back(std::move(ins.note));
        }
        if (!apply_and_record(std::move(change))) {
            return false;  // apply_and_record already rolled back
        }
    }

    s.deletes.clear();
    s.moves.clear();
    s.adds.clear();
    s.flushed = true;
    return true;
}

bool Transaction::flush_all() {
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        if (!flush(sessions_[i])) {
            return false;
        }
    }
    return true;
}

// =============================================================================================
// Transaction: selection helpers
// =============================================================================================

bool Transaction::selection_ready(std::string_view op) {
    if (!is_active()) {
        return false;
    }
    if (state_ != TransactionState::SelectionChange) {
        fail(std::format("{} used outside the selection stage", op));
        return false;
    }
    return true;
}

bool Transaction::valid_selection_ids(TrackId track, const std::vector<NoteId>& ids,
                                      std::string_view op) {
    if (track >= track_count()) {
        fail(std::format("{} on track {} which does not exist", op, track));
        return false;
    }
    if (!ids.empty() && ids.back() >= notes_of(track).size()) {
        fail(std::format("{} was given a note id past the end of track {}", op, track));
        return false;
    }
    return true;
}

std::vector<NoteId> Transaction::current_selection(TrackId track) const {
    const std::vector<NoteId>* now = executor_->context().selection->get_selected_ids_in_track(track);
    return now != nullptr ? *now : std::vector<NoteId>{};
}

void Transaction::snapshot_selection(TrackId track) {
    if (selection_snapshots_.contains(track)) {
        return;
    }
    selection_snapshots_[track] = changes_.size();
    // inverse form: applying it restores the selection as it was before the transaction
    changes_.emplace_back(SelectionSwapChange{track, current_selection(track)});
}

void Transaction::write_selection(TrackId track, std::vector<NoteId> ids) {
    SharedSelectedNotes* selection = executor_->context().selection;
    (void)selection->take_selected_from_track(track);
    if (!ids.empty()) {
        selection->set_selected_in_track(std::move(ids), track);
    }
    executor_->notify({ChangeKind::Selection, track});
}

// =============================================================================================
// TrackMutator
// =============================================================================================

bool Transaction::track_ops_allowed(std::string_view op) {
    if (!is_active()) {
        return false;
    }
    if (state_ != TransactionState::TrackEditing || !tracks_requested_ || tracks_finalized_) {
        fail(std::format("{} used outside the track editing stage", op));
        return false;
    }
    return true;
}

TrackId TrackMutator::add_track(MIDITrack track) {
    if (!tx_->track_ops_allowed("add_track")) {
        return 0;
    }
    const std::size_t count = tx_->track_count();
    if (count >= 0x10000) {
        tx_->fail("add_track would exceed the 65,536 track limit");
        return 0;
    }
    return insert_track(static_cast<TrackId>(count), std::move(track));
}

TrackId TrackMutator::insert_track(TrackId index, MIDITrack track) {
    if (!tx_->track_ops_allowed("insert_track")) {
        return 0;
    }
    if (tx_->track_count() >= 0x10000) {
        tx_->fail("insert_track would exceed the 65,536 track limit");
        return 0;
    }
    tx_->apply_and_record(TrackPresenceChange{index, std::move(track)});
    return index;
}

void TrackMutator::remove_track(TrackId index) {
    if (!tx_->track_ops_allowed("remove_track")) {
        return;
    }
    tx_->apply_and_record(TrackPresenceChange{index, std::nullopt});
}

void TrackMutator::swap_tracks(TrackId a, TrackId b) {
    if (!tx_->track_ops_allowed("swap_tracks")) {
        return;
    }
    tx_->apply_and_record(TrackSwapChange{a, b});
}

void TrackMutator::move_track(TrackId from, TrackId to) {
    if (!tx_->track_ops_allowed("move_track")) {
        return;
    }
    tx_->apply_and_record(TrackMoveChange{from, to});
}

void TrackMutator::finalize_tracks() {
    if (!tx_->track_ops_allowed("finalize_tracks")) {
        return;
    }
    tx_->tracks_finalized_ = true;
}

// =============================================================================================
// NoteMutator
// =============================================================================================

template <typename F>
void NoteMutator::edit_props(NoteId id, std::string_view op, F&& edit) {
    Transaction::NoteSession* s = tx_->session_for_edit(session_, op);
    if (s == nullptr || !tx_->check_touched(*s, id, op)) {
        return;
    }

    Note& note = tx_->notes_of(s->track)[id];
    auto [it, inserted] = s->prop_edits.try_emplace(id);
    if (inserted) {
        it->second.index = id;
        it->second.before = props_of(note);
    }
    edit(note);
    it->second.after = props_of(note);
}

void NoteMutator::set_length(NoteId id, MIDITick length) {
    edit_props(id, "set_length", [length](Note& n) { n.length = length < 1 ? 1 : length; });
}
void NoteMutator::set_key(NoteId id, std::uint8_t key) {
    edit_props(id, "set_key", [key](Note& n) { n.key = key > 127 ? 127 : key; });
}
void NoteMutator::set_velocity(NoteId id, std::uint8_t velocity) {
    edit_props(id, "set_velocity", [velocity](Note& n) { n.velocity = velocity; });
}
void NoteMutator::set_channel(NoteId id, std::uint8_t channel) {
    edit_props(id, "set_channel", [channel](Note& n) { n.channel = channel > 15 ? 15 : channel; });
}

void NoteMutator::set_length(std::span<const NoteId> ids, MIDITick length) {
    for (const NoteId id : ids) set_length(id, length);
}
void NoteMutator::set_key(std::span<const NoteId> ids, std::uint8_t key) {
    for (const NoteId id : ids) set_key(id, key);
}
void NoteMutator::set_velocity(std::span<const NoteId> ids, std::uint8_t velocity) {
    for (const NoteId id : ids) set_velocity(id, velocity);
}
void NoteMutator::set_channel(std::span<const NoteId> ids, std::uint8_t channel) {
    for (const NoteId id : ids) set_channel(id, channel);
}

void NoteMutator::set_length(std::span<const NoteId> ids, std::span<MIDITick> lengths) {
    for (size_t i = 0; i < ids.size(); i++) set_length(ids[i], lengths[i]);
}
void NoteMutator::set_key(std::span<const NoteId> ids, std::span<std::uint8_t> keys) {
    for (size_t i = 0; i < ids.size(); i++) set_key(ids[i], keys[i]);
}
void NoteMutator::set_velocity(std::span<const NoteId> ids, std::span<std::uint8_t> velocities) {
    for (size_t i = 0; i < ids.size(); i++) set_velocity(ids[i], velocities[i]);
}
void NoteMutator::set_channel(std::span<const NoteId> ids, std::span<std::uint8_t> channels) {
    for (size_t i = 0; i < ids.size(); i++) set_channel(ids[i], channels[i]);
}

void NoteMutator::move_note(NoteId id, SignedMIDITick delta_ticks, std::int16_t delta_keys) {
    Transaction::NoteSession* s = tx_->session_for_edit(session_, "move_note");
    if (s == nullptr || !tx_->check_touched(*s, id, "move_note")) {
        return;
    }

    const Note& note = tx_->notes_of(s->track)[id];
    const auto [it, inserted] = s->moves.try_emplace(
        id, Transaction::PendingMove{note.start, note.key});
    (void)inserted;

    const SignedMIDITick start = static_cast<SignedMIDITick>(it->second.start) + delta_ticks;
    const int key = static_cast<int>(it->second.key) + delta_keys;
    it->second.start = static_cast<MIDITick>(start < 0 ? 0 : start);
    it->second.key = static_cast<std::uint8_t>(key < 0 ? 0 : (key > 127 ? 127 : key));
}

void NoteMutator::move_notes(std::span<const NoteId> ids, SignedMIDITick delta_ticks,
                             std::int16_t delta_keys) {
    for (const NoteId id : ids) move_note(id, delta_ticks, delta_keys);
}

void NoteMutator::set_position(NoteId id, MIDITick start, std::uint8_t key) {
    Transaction::NoteSession* s = tx_->session_for_edit(session_, "set_position");
    if (s == nullptr || !tx_->check_touched(*s, id, "set_position")) {
        return;
    }
    s->moves[id] = Transaction::PendingMove{start, static_cast<std::uint8_t>(key > 127 ? 127 : key)};
}

void NoteMutator::delete_note(NoteId id) {
    Transaction::NoteSession* s = tx_->session_for_edit(session_, "delete_note");
    if (s == nullptr || !tx_->check_touched(*s, id, "delete_note")) {
        return;
    }
    s->deletes.push_back(id);
}

void NoteMutator::delete_notes(std::span<const NoteId> ids) {
    for (const NoteId id : ids) delete_note(id);
}

AddedNote NoteMutator::add_note(const Note& note) {
    Transaction::NoteSession* s = tx_->session_for_edit(session_, "add_note");
    if (s == nullptr) {
        return AddedNote{0};
    }
    s->adds.push_back(note);
    return AddedNote{s->adds.size() - 1};
}

namespace {

const std::vector<NoteId> kNoIds;

}

std::optional<NoteId> NoteMutator::final_id(NoteId original) const {
    if (session_ == Transaction::kNoSession || session_ >= tx_->sessions_.size()) {
        return std::nullopt;
    }
    const Transaction::NoteSession& s = tx_->sessions_[session_];
    if (!s.flushed) {
        Debugger::log_warning("final_id() called before the note mutator was flushed");
        return std::nullopt;
    }
    std::vector<NoteId> mapped = remap_ids({original});
    if (mapped.empty()) {
        return std::nullopt;
    }
    return mapped.front();
}

NoteId NoteMutator::final_id(AddedNote added) const {
    if (session_ == Transaction::kNoSession || session_ >= tx_->sessions_.size()) {
        return 0;
    }
    const Transaction::NoteSession& s = tx_->sessions_[session_];
    if (!s.flushed || added.slot >= s.added_final.size()) {
        Debugger::log_warning("final_id() called before the note mutator was flushed");
        return 0;
    }
    return s.added_final[added.slot];
}

std::vector<NoteId> NoteMutator::remap_ids(std::vector<NoteId> original_ids) const {
    std::vector<NoteId> out;
    if (session_ == Transaction::kNoSession || session_ >= tx_->sessions_.size()) {
        return out;
    }
    const Transaction::NoteSession& s = tx_->sessions_[session_];
    if (!s.flushed) {
        Debugger::log_warning("remap_ids() called before the note mutator was flushed");
        return out;
    }

    std::sort(original_ids.begin(), original_ids.end());
    original_ids.erase(std::unique(original_ids.begin(), original_ids.end()), original_ids.end());
    out.reserve(original_ids.size());

    std::size_t removed = 0;   // entries of remove_at below the current id
    std::size_t inserted = 0;  // entries of insert_at at or before the current final index
    for (const NoteId id : original_ids) {
        while (removed < s.remove_at.size() && s.remove_at[removed] < id) {
            ++removed;
        }
        if (removed < s.remove_at.size() && s.remove_at[removed] == id) {
            if (const auto moved = s.moved_final.find(id); moved != s.moved_final.end()) {
                out.push_back(moved->second);
            }
            continue;  // deleted
        }

        const std::size_t kept_index = id - removed;
        while (inserted < s.insert_at.size() && s.insert_at[inserted] <= kept_index + inserted) {
            ++inserted;
        }
        out.push_back(kept_index + inserted);
    }

    std::sort(out.begin(), out.end());
    return out;
}

const std::vector<NoteId>& NoteMutator::inserted_ids() const {
    if (session_ == Transaction::kNoSession || session_ >= tx_->sessions_.size() ||
        !tx_->sessions_[session_].flushed) {
        return kNoIds;
    }
    return tx_->sessions_[session_].insert_at;
}

// =============================================================================================
// SelectionMutator
// =============================================================================================

void SelectionMutator::set(TrackId track, std::span<const NoteId> ids) {
    if (!tx_->selection_ready("selection.set")) {
        return;
    }
    std::vector<NoteId> sorted = sorted_unique(ids);
    if (!tx_->valid_selection_ids(track, sorted, "selection.set")) {
        return;
    }
    tx_->snapshot_selection(track);
    tx_->write_selection(track, std::move(sorted));
}

void SelectionMutator::add(TrackId track, std::span<const NoteId> ids) {
    if (!tx_->selection_ready("selection.add")) {
        return;
    }
    const std::vector<NoteId> sorted = sorted_unique(ids);
    if (!tx_->valid_selection_ids(track, sorted, "selection.add")) {
        return;
    }
    tx_->snapshot_selection(track);

    const std::vector<NoteId> current = tx_->current_selection(track);
    std::vector<NoteId> merged;
    merged.reserve(current.size() + sorted.size());
    std::set_union(current.begin(), current.end(), sorted.begin(), sorted.end(),
                   std::back_inserter(merged));
    tx_->write_selection(track, std::move(merged));
}

void SelectionMutator::remove(TrackId track, std::span<const NoteId> ids) {
    if (!tx_->selection_ready("selection.remove")) {
        return;
    }
    const std::vector<NoteId> sorted = sorted_unique(ids);
    if (track >= tx_->track_count()) {
        tx_->fail(std::format("selection.remove on track {} which does not exist", track));
        return;
    }
    tx_->snapshot_selection(track);

    const std::vector<NoteId> current = tx_->current_selection(track);
    std::vector<NoteId> kept;
    kept.reserve(current.size());
    std::set_difference(current.begin(), current.end(), sorted.begin(), sorted.end(),
                        std::back_inserter(kept));
    tx_->write_selection(track, std::move(kept));
}

void SelectionMutator::clear_track(TrackId track) { set(track, {}); }

void SelectionMutator::clear_all() {
    if (!tx_->selection_ready("selection.clear_all")) {
        return;
    }
    const std::vector<std::uint16_t> tracks =
        tx_->executor_->context().selection->get_active_selected_tracks();
    for (const std::uint16_t track : tracks) {
        clear_track(track);
    }
}

void SelectionMutator::remap(const NoteMutator& notes) {
    if (!tx_->selection_ready("selection.remap")) {
        return;
    }
    if (notes.session_ == Transaction::kNoSession || notes.session_ >= tx_->sessions_.size()) {
        return;
    }
    const TrackId track = tx_->sessions_[notes.session_].track;

    tx_->snapshot_selection(track);
    std::vector<NoteId> remapped = notes.remap_ids(tx_->current_selection(track));
    tx_->write_selection(track, std::move(remapped));
}

}