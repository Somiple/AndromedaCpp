#include "editor/history/transaction.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <utility>

#include "util/debugger.h"

namespace andromeda::editor::history {

using midi::ChannelEvent;
using midi::MetaEvent;
using midi::MetaEventType;
using midi::MIDITrack;
using midi::Note;
using util::Debugger;

namespace {

std::vector<EventId> sorted_unique(std::span<const EventId> ids) {
    std::vector<EventId> out(ids.begin(), ids.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::uint8_t clamp7(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 127)); }

const std::vector<EventId> kNoIds;

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

template <typename Item>
void Transaction::finalize_unflushed() {
    for (EventSession<Item>& s : sessions<Item>()) {
        if (!s.flushed) {
            finalize_props(s);
        }
    }
}

void Transaction::rollback(std::string_view reason) {
    if (!is_active()) {
        return;
    }
    Debugger::log_warning(
        std::format("Transaction '{}' rolled back: {}", label_, std::string(reason)));

    // queued structural edits are simply dropped; live property edits need to be in changes_
    finalize_unflushed<Note>();
    finalize_unflushed<ChannelEvent>();
    finalize_unflushed<MetaEvent>();

    state_ = TransactionState::RolledBack;
    for (std::size_t i = changes_.size(); i-- > 0;) {
        if (!executor_->apply(changes_[i])) {
            Debugger::log_error("Rollback failed to revert a change; the project may be "
                                "inconsistent");
        }
    }
    changes_.clear();
    sessions<Note>().clear();
    sessions<ChannelEvent>().clear();
    sessions<MetaEvent>().clear();
    selection_snapshots_.clear();
    release_slot();
}

std::size_t Transaction::track_count() const {
    return executor_->context().project->get_tracks()->size();
}

bool Transaction::apply_and_record(Change change) {
    if (!executor_->apply(change)) {
        return fail("a change could not be applied (invalid track or element index)");
    }
    changes_.push_back(std::move(change));
    return true;
}

// =============================================================================================
// Transaction: event sessions (generic over notes / channel events / meta events)
// =============================================================================================

template <typename Item>
std::size_t Transaction::open_session(TrackId track, std::span<const EventId> ids) {
    using Traits = EventTraits<Item>;
    constexpr std::string_view fn = Traits::accessor;

    if (!is_active()) {
        return kNoSession;
    }
    if (state_ == TransactionState::SelectionChange) {
        fail(std::format("{} requested after selection()", fn));
        return kNoSession;
    }
    if (tracks_requested_ && !tracks_finalized_) {
        fail(std::format("{} requested before finalize_tracks()", fn));
        return kNoSession;
    }
    if (!event_track_exists<Item>(track)) {
        fail(std::format("{} on track {} which does not exist", fn, track));
        return kNoSession;
    }
    for (const EventSession<Item>& s : sessions<Item>()) {
        if (s.track == track) {
            fail(std::format("{} requested twice for track {}", fn, track));
            return kNoSession;
        }
    }

    std::vector<EventId> touched = sorted_unique(ids);
    if (!touched.empty() && touched.back() >= items_of<Item>(track).size()) {
        fail(std::format("{} was given an id past the end of track {}", fn, track));
        return kNoSession;
    }

    // Moving on to another kind of element seals the previous kind, so edits never intertwine.
    const TransactionState wanted = editing_state(Traits::kind);
    if (state_ != wanted) {
        if (!flush_all()) {
            return kNoSession;  // flush already rolled back
        }
        state_ = wanted;
    }

    auto& list = sessions<Item>();
    EventSession<Item>& s = list.emplace_back();
    s.track = track;
    s.touched = std::move(touched);
    s.props_change = changes_.size();
    changes_.emplace_back(PropertyChange<Item>{track, {}});

    return list.size() - 1;
}

template <typename Item>
EventSession<Item>* Transaction::session_for_edit(std::size_t index, std::string_view op) {
    if (!is_active() || index == kNoSession) {
        return nullptr;
    }
    auto& list = sessions<Item>();
    if (state_ != editing_state(EventTraits<Item>::kind) || index >= list.size()) {
        fail(std::format("{} used outside its editing stage (another stage has begun)", op));
        return nullptr;
    }
    EventSession<Item>& s = list[index];
    if (s.flushed) {
        fail(std::format("{} used on a mutator that was already flushed", op));
        return nullptr;
    }
    return &s;
}

template <typename Item>
bool Transaction::check_touched(const EventSession<Item>& session, EventId id,
                                std::string_view op) {
    if (!std::binary_search(session.touched.begin(), session.touched.end(), id)) {
        fail(std::format("{} on {} id {} which was not declared when the mutator was requested",
                         op, EventTraits<Item>::accessor, id));
        return false;
    }
    return true;
}

// Turns the live property edits into the PropertyChange recorded at session creation. Safe to
// call twice (a flush that fails half-way is followed by a rollback, which calls it again).
template <typename Item>
void Transaction::finalize_props(EventSession<Item>& s) {
    if (s.props_finalized) {
        return;
    }
    s.props_finalized = true;

    auto& change = std::get<PropertyChange<Item>>(changes_[s.props_change]);
    change.edits.clear();
    change.edits.reserve(s.prop_edits.size());

    for (auto& [id, edit] : s.prop_edits) {
        if (edit.before == edit.after) {
            continue;
        }
        // stored in inverse form: applying it writes the ORIGINAL values back
        PropertyEdit<Item> inverse;
        inverse.index = id;
        inverse.before = std::move(edit.after);
        inverse.after = std::move(edit.before);
        change.edits.push_back(std::move(inverse));
    }
    s.prop_edits.clear();

    std::sort(change.edits.begin(), change.edits.end(),
              [](const PropertyEdit<Item>& a, const PropertyEdit<Item>& b) {
                  return a.index < b.index;
              });

    if (!change.edits.empty()) {
        // live property edits bypass the executor, so derived state is refreshed here
        if (s.track == kGlobalTrack) {
            executor_->refresh_global_meta();
        }
        executor_->notify({kind_for<Item>(s.track), s.track});
    }
}

template <typename Item>
bool Transaction::flush(EventSession<Item>& s) {
    using Traits = EventTraits<Item>;

    if (s.flushed) {
        return true;
    }
    finalize_props(s);

    std::vector<Item>& items = items_of<Item>(s.track);

    std::sort(s.deletes.begin(), s.deletes.end());
    s.deletes.erase(std::unique(s.deletes.begin(), s.deletes.end()), s.deletes.end());

    struct Insert {
        Item item;
        std::size_t origin;  // original id for a moved element, add slot for an added one
        bool moved;
    };

    std::vector<EventId> remove_at = s.deletes;
    std::vector<Insert> inserts;

    // deterministic order regardless of hash iteration
    std::vector<std::pair<EventId, typename Traits::Position>> moves(s.moves.begin(),
                                                                       s.moves.end());
    std::sort(moves.begin(), moves.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [id, position] : moves) {
        if (std::binary_search(s.deletes.begin(), s.deletes.end(), id)) {
            continue;  // deleting wins over moving
        }
        Item moved = items[id];  // includes any property edits made through this mutator
        Traits::place(moved, position);
        remove_at.push_back(id);
        inserts.push_back({std::move(moved), id, true});
    }
    for (std::size_t slot = 0; slot < s.adds.size(); ++slot) {
        inserts.push_back({s.adds[slot], slot, false});
    }

    std::sort(remove_at.begin(), remove_at.end());
    std::stable_sort(inserts.begin(), inserts.end(), [](const Insert& a, const Insert& b) {
        return Traits::time(a.item) < Traits::time(b.item);
    });

    // Where does each insert land? Among the elements that stay, it goes after every element
    // whose time is <= its own; earlier inserts of the same pass sit in front of it.
    std::vector<EventId> insert_at(inserts.size());
    s.added_final.assign(s.adds.size(), 0);
    s.moved_final.clear();
    for (std::size_t j = 0; j < inserts.size(); ++j) {
        const MIDITick time = Traits::time(inserts[j].item);
        const auto upper = std::upper_bound(
            items.begin(), items.end(), time,
            [](MIDITick t, const Item& item) { return t < Traits::time(item); });
        const auto upper_index = static_cast<std::size_t>(upper - items.begin());
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
        PresenceChange<Item> change;
        change.track = s.track;
        change.remove_at = std::move(remove_at);
        change.insert_at = std::move(insert_at);
        change.payload.reserve(inserts.size());
        for (Insert& ins : inserts) {
            change.payload.push_back(std::move(ins.item));
        }
        if (!apply_and_record(std::move(change))) {
            return false;  // apply_and_record already rolled back (and invalidated `s`)
        }
    }

    s.deletes.clear();
    s.moves.clear();
    s.adds.clear();
    s.flushed = true;
    return true;
}

template <typename Item>
bool Transaction::flush_kind() {
    auto& list = sessions<Item>();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!flush(list[i])) {
            return false;
        }
    }
    return true;
}

bool Transaction::flush_all() {
    return flush_kind<Note>() && flush_kind<ChannelEvent>() && flush_kind<MetaEvent>();
}

// =============================================================================================
// Transaction: stage transitions
// =============================================================================================

TrackMutator Transaction::tracks() {
    if (is_active()) {
        if (state_ != TransactionState::TrackEditing) {
            fail("track mutations are not allowed after element editing has begun");
        } else if (tracks_finalized_) {
            fail("tracks() requested again after finalize_tracks()");
        } else {
            tracks_requested_ = true;
        }
    }
    return TrackMutator(this);
}

void Transaction::set_active_track(TrackId track) {
    if (!is_active()) {
        return;
    }
    apply_and_record(ActiveTrackChange{track});
}

NoteMutator Transaction::notes(TrackId track, std::span<const NoteId> ids) {
    return NoteMutator(this, open_session<Note>(track, ids));
}

ChannelEventMutator Transaction::channel_events(TrackId track, std::span<const EventId> ids) {
    return ChannelEventMutator(this, open_session<ChannelEvent>(track, ids));
}

MetaEventMutator Transaction::meta_events(TrackId track, std::span<const EventId> ids) {
    return MetaEventMutator(this, open_session<MetaEvent>(track, ids));
}

MetaEventMutator Transaction::global_meta_events(std::span<const EventId> ids) {
    return MetaEventMutator(this, open_session<MetaEvent>(kGlobalTrack, ids));
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
        return std::visit(
            [this](auto& c) -> bool {
                using T = std::decay_t<decltype(c)>;
                if constexpr (is_property_change_v<T>) {
                    return c.edits.empty();
                } else if constexpr (is_presence_change_v<T>) {
                    return c.remove_at.empty() && c.insert_at.empty();
                } else if constexpr (std::is_same_v<T, SelectionSwapChange>) {
                    return current_selection(c.track) == c.ids;
                } else if constexpr (std::is_same_v<T, ActiveTrackChange>) {
                    // stored in inverse form: it holds the previous value, so equal means
                    // unchanged
                    const std::uint16_t* now = executor_->context().active_track;
                    return now != nullptr && *now == c.track;
                } else {
                    return false;
                }
            },
            change);
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

// TODO: make selection support channel events
bool Transaction::valid_selection_ids(TrackId track, const std::vector<NoteId>& ids,
                                      std::string_view op) {
    if (track >= track_count()) {
        fail(std::format("{} on track {} which does not exist", op, track));
        return false;
    }
    if (!ids.empty() && ids.back() >= items_of<Note>(track).size()) {
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
    if (count >= kGlobalTrack) {
        tx_->fail("add_track would exceed the 65,535 track limit");
        return 0;
    }
    return insert_track(static_cast<TrackId>(count), std::move(track));
}

TrackId TrackMutator::insert_track(TrackId index, MIDITrack track) {
    if (!tx_->track_ops_allowed("insert_track")) {
        return 0;
    }
    if (tx_->track_count() >= kGlobalTrack) {
        tx_->fail("insert_track would exceed the 65,535 track limit");
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
// EventMutatorBase: shared by every element kind
// =============================================================================================

template <typename Item>
template <typename F>
void EventMutatorBase<Item>::edit_props(EventId id, std::string_view op, F&& edit) {
    EventSession<Item>* s = tx_->session_for_edit<Item>(session_, op);
    if (s == nullptr || !tx_->check_touched(*s, id, op)) {
        return;
    }

    Item& item = tx_->items_of<Item>(s->track)[id];
    auto [it, inserted] = s->prop_edits.try_emplace(id);
    if (inserted) {
        it->second.index = id;
        it->second.before = Traits::props(item);
    }
    edit(item);
    it->second.after = Traits::props(item);
}

template <typename Item>
typename EventMutatorBase<Item>::Position* EventMutatorBase<Item>::pending_position(
    EventId id, std::string_view op) {
    EventSession<Item>* s = tx_->session_for_edit<Item>(session_, op);
    if (s == nullptr || !tx_->check_touched(*s, id, op)) {
        return nullptr;
    }
    const Item& item = tx_->items_of<Item>(s->track)[id];
    return &s->moves.try_emplace(id, Traits::position(item)).first->second;
}

template <typename Item>
void EventMutatorBase<Item>::set_pending_position(EventId id, const Position& position,
                                                  std::string_view op) {
    EventSession<Item>* s = tx_->session_for_edit<Item>(session_, op);
    if (s == nullptr || !tx_->check_touched(*s, id, op)) {
        return;
    }
    s->moves[id] = position;
}

template <typename Item>
void EventMutatorBase<Item>::erase(EventId id, std::string_view op) {
    EventSession<Item>* s = tx_->session_for_edit<Item>(session_, op);
    if (s == nullptr || !tx_->check_touched(*s, id, op)) {
        return;
    }
    s->deletes.push_back(id);
}

template <typename Item>
void EventMutatorBase<Item>::erase_many(std::span<const EventId> ids, std::string_view op) {
    for (const EventId id : ids) {
        erase(id, op);
    }
}

template <typename Item>
template <typename Pred>
void EventMutatorBase<Item>::erase_if(std::span<const EventId> ids, Pred&& pred,
                                      std::string_view op) {
    EventSession<Item>* s = tx_->session_for_edit<Item>(session_, op);
    if (s == nullptr) {
        return;
    }
    for (const EventId id : ids) {
        if (!tx_->check_touched(*s, id, op)) {
            return;  // rolled back, `s` is gone
        }
        const Item& item = tx_->items_of<Item>(s->track)[id];
        if (pred(item)) {
            s->deletes.push_back(id);
        }
    }
}

template <typename Item>
Added<Item> EventMutatorBase<Item>::add(const Item& item, std::string_view op) {
    EventSession<Item>* s = tx_->session_for_edit<Item>(session_, op);
    if (s == nullptr) {
        return Added<Item>{0};
    }
    s->adds.push_back(item);
    return Added<Item>{s->adds.size() - 1};
}

template <typename Item>
const EventSession<Item>* EventMutatorBase<Item>::flushed_session(std::string_view op) const {
    if (session_ == Transaction::kNoSession || session_ >= tx_->sessions<Item>().size()) {
        return nullptr;
    }
    const EventSession<Item>& s = tx_->sessions<Item>()[session_];
    if (!s.flushed) {
        Debugger::log_warning(std::format("{}() called before the mutator was flushed", op));
        return nullptr;
    }
    return &s;
}

template <typename Item>
std::optional<EventId> EventMutatorBase<Item>::final_id(EventId original) const {
    if (flushed_session("final_id") == nullptr) {
        return std::nullopt;
    }
    const std::vector<EventId> mapped = remap_ids({original});
    if (mapped.empty()) {
        return std::nullopt;
    }
    return mapped.front();
}

template <typename Item>
EventId EventMutatorBase<Item>::final_id(Added<Item> added) const {
    const EventSession<Item>* s = flushed_session("final_id");
    if (s == nullptr || added.slot >= s->added_final.size()) {
        return 0;
    }
    return s->added_final[added.slot];
}

template <typename Item>
std::vector<EventId> EventMutatorBase<Item>::remap_ids(std::vector<EventId> original_ids) const {
    std::vector<EventId> out;
    const EventSession<Item>* sp = flushed_session("remap_ids");
    if (sp == nullptr) {
        return out;
    }
    const EventSession<Item>& s = *sp;

    std::sort(original_ids.begin(), original_ids.end());
    original_ids.erase(std::unique(original_ids.begin(), original_ids.end()), original_ids.end());
    out.reserve(original_ids.size());

    std::size_t removed = 0;   // entries of remove_at below the current id
    std::size_t inserted = 0;  // entries of insert_at at or before the current final index
    for (const EventId id : original_ids) {
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

template <typename Item>
const std::vector<EventId>& EventMutatorBase<Item>::inserted_ids() const {
    if (session_ == Transaction::kNoSession || session_ >= tx_->sessions<Item>().size() ||
        !tx_->sessions<Item>()[session_].flushed) {
        return kNoIds;
    }
    return tx_->sessions<Item>()[session_].insert_at;
}

// =============================================================================================
// NoteMutator
// =============================================================================================

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
    for (std::size_t i = 0; i < ids.size(); i++) set_length(ids[i], lengths[i]);
}
void NoteMutator::set_key(std::span<const NoteId> ids, std::span<std::uint8_t> keys) {
    for (std::size_t i = 0; i < ids.size(); i++) set_key(ids[i], keys[i]);
}
void NoteMutator::set_velocity(std::span<const NoteId> ids, std::span<std::uint8_t> velocities) {
    for (std::size_t i = 0; i < ids.size(); i++) set_velocity(ids[i], velocities[i]);
}
void NoteMutator::set_channel(std::span<const NoteId> ids, std::span<std::uint8_t> channels) {
    for (std::size_t i = 0; i < ids.size(); i++) set_channel(ids[i], channels[i]);
}

void NoteMutator::move_note(NoteId id, SignedMIDITick delta_ticks, std::int16_t delta_keys) {
    NotePosition* p = pending_position(id, "move_note");
    if (p == nullptr) {
        return;
    }
    const SignedMIDITick start = static_cast<SignedMIDITick>(p->tick) + delta_ticks;
    const int key = static_cast<int>(p->key) + delta_keys;
    p->tick = static_cast<MIDITick>(start < 0 ? 0 : start);
    p->key = clamp7(key);
}

void NoteMutator::move_notes(std::span<const NoteId> ids, SignedMIDITick delta_ticks,
                             std::int16_t delta_keys) {
    for (const NoteId id : ids) move_note(id, delta_ticks, delta_keys);
}

void NoteMutator::set_position(NoteId id, MIDITick start, std::uint8_t key) {
    set_pending_position(id, NotePosition{start, clamp7(key)}, "set_position");
}

// =============================================================================================
// ChannelEventMutator
// =============================================================================================

template <typename T, typename F>
void ChannelEventMutator::edit_as(EventId id, std::string_view op, F&& edit) {
    edit_props(id, op, [&edit](ChannelEvent& e) {
        if (T* value = std::get_if<T>(&e.event_type)) {
            edit(*value);
        }
    });
}

void ChannelEventMutator::set_poly_pressure(EventId id, std::uint8_t key, std::uint8_t pressure) {
    edit_as<midi::NoteAftertouch>(id, "set_poly_pressure", [=](midi::NoteAftertouch& v) {
        v.key = clamp7(key);
        v.pressure = clamp7(pressure);
    });
}
void ChannelEventMutator::set_poly_pressure_key(EventId id, std::uint8_t key) {
    edit_as<midi::NoteAftertouch>(id, "set_poly_pressure_key",
                                  [=](midi::NoteAftertouch& v) { v.key = clamp7(key); });
}
void ChannelEventMutator::set_poly_pressure_value(EventId id, std::uint8_t pressure) {
    edit_as<midi::NoteAftertouch>(id, "set_poly_pressure_value",
                                  [=](midi::NoteAftertouch& v) { v.pressure = clamp7(pressure); });
}

void ChannelEventMutator::set_poly_pressure(std::span<const EventId> ids, std::uint8_t key,
                                            std::uint8_t pressure) {
    for (const EventId id : ids) set_poly_pressure(id, key, pressure);
}
void ChannelEventMutator::set_poly_pressure_key(std::span<const EventId> ids, std::uint8_t key) {
    for (const EventId id : ids) set_poly_pressure_key(id, key);
}
void ChannelEventMutator::set_poly_pressure_value(std::span<const EventId> ids,
                                                  std::uint8_t pressure) {
    for (const EventId id : ids) set_poly_pressure_value(id, pressure);
}

void ChannelEventMutator::set_control_change(EventId id, std::uint8_t controller,
                                             std::uint8_t value) {
    edit_as<midi::Controller>(id, "set_control_change", [=](midi::Controller& v) {
        v.controller = clamp7(controller);
        v.value = clamp7(value);
    });
}
void ChannelEventMutator::set_controller(EventId id, std::uint8_t controller) {
    edit_as<midi::Controller>(id, "set_controller",
                              [=](midi::Controller& v) { v.controller = clamp7(controller); });
}
void ChannelEventMutator::set_controller_value(EventId id, std::uint8_t value) {
    edit_as<midi::Controller>(id, "set_controller_value",
                              [=](midi::Controller& v) { v.value = clamp7(value); });
}

void ChannelEventMutator::set_control_change(std::span<const EventId> ids,
                                             std::uint8_t controller, std::uint8_t value) {
    for (const EventId id : ids) set_control_change(id, controller, value);
}
void ChannelEventMutator::set_controller(std::span<const EventId> ids, std::uint8_t controller) {
    for (const EventId id : ids) set_controller(id, controller);
}
void ChannelEventMutator::set_controller_value(std::span<const EventId> ids, std::uint8_t value) {
    for (const EventId id : ids) set_controller_value(id, value);
}

void ChannelEventMutator::set_program(EventId id, std::uint8_t program) {
    edit_as<midi::ProgramChange>(id, "set_program",
                                 [=](midi::ProgramChange& v) { v.program = clamp7(program); });
}
void ChannelEventMutator::set_program(std::span<const EventId> ids, std::uint8_t program) {
    for (const EventId id : ids) set_program(id, program);
}

void ChannelEventMutator::set_channel_pressure(EventId id, std::uint8_t pressure) {
    edit_as<midi::ChannelAftertouch>(
        id, "set_channel_pressure",
        [=](midi::ChannelAftertouch& v) { v.amount = clamp7(pressure); });
}
void ChannelEventMutator::set_channel_pressure(std::span<const EventId> ids,
                                               std::uint8_t pressure) {
    for (const EventId id : ids) set_channel_pressure(id, pressure);
}

void ChannelEventMutator::set_pitch_bend(EventId id, std::uint16_t value) {
    const std::uint16_t raw = std::min<std::uint16_t>(value, 0x3FFF);
    edit_as<midi::PitchBend>(id, "set_pitch_bend", [raw](midi::PitchBend& v) {
        v.lsb = static_cast<std::uint8_t>(raw & 0x7F);
        v.msb = static_cast<std::uint8_t>((raw >> 7) & 0x7F);
    });
}
void ChannelEventMutator::set_pitch_bend(std::span<const EventId> ids, std::uint16_t value) {
    for (const EventId id : ids) set_pitch_bend(id, value);
}

void ChannelEventMutator::set_pitch_bend_normalized(EventId id, float value) {
    if (std::isnan(value)) {
        return;
    }
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const long raw = std::lround(8192.0f + clamped * 8192.0f);
    set_pitch_bend(id, static_cast<std::uint16_t>(std::clamp(raw, 0L, 16383L)));
}
void ChannelEventMutator::set_pitch_bend_normalized(std::span<const EventId> ids, float value) {
    for (const EventId id : ids) set_pitch_bend_normalized(id, value);
}

void ChannelEventMutator::set_channel(EventId id, std::uint8_t channel) {
    edit_props(id, "set_channel",
               [channel](ChannelEvent& e) { e.channel = channel > 15 ? 15 : channel; });
}
void ChannelEventMutator::set_channel(std::span<const EventId> ids, std::uint8_t channel) {
    for (const EventId id : ids) set_channel(id, channel);
}

void ChannelEventMutator::move_event(EventId id, SignedMIDITick delta_ticks) {
    EventPosition* p = pending_position(id, "move_event");
    if (p == nullptr) {
        return;
    }
    const SignedMIDITick tick = static_cast<SignedMIDITick>(p->tick) + delta_ticks;
    p->tick = static_cast<MIDITick>(tick < 0 ? 0 : tick);
}
void ChannelEventMutator::move_events(std::span<const EventId> ids, SignedMIDITick delta_ticks) {
    for (const EventId id : ids) move_event(id, delta_ticks);
}
void ChannelEventMutator::set_time(EventId id, MIDITick tick) {
    set_pending_position(id, EventPosition{tick}, "set_time");
}

// =============================================================================================
// MetaEventMutator
// =============================================================================================

void MetaEventMutator::set_data(EventId id, std::span<const std::uint8_t> data) {
    edit_props(id, "set_data", [data](MetaEvent& e) {
        // fixed-size payloads must stay well formed (the tempo map and get_value_string() index
        // into them)
        const auto required = [&]() -> std::size_t {
            switch (e.event_type) {
            case MetaEventType::Tempo:         return 3;
            case MetaEventType::TimeSignature: return 4;
            case MetaEventType::KeySignature:  return 2;
            default:                           return data.size();
            }
        }();
        if (data.size() != required) {
            return;
        }
        e.data.assign(data.begin(), data.end());
    });
}

void MetaEventMutator::set_text(EventId id, std::string_view text) {
    edit_props(id, "set_text", [text](MetaEvent& e) {
        const auto type = static_cast<std::uint8_t>(e.event_type);
        if (type < static_cast<std::uint8_t>(MetaEventType::Text) ||
            type > static_cast<std::uint8_t>(MetaEventType::DeviceName)) {
            return;
        }
        e.data.assign(text.begin(), text.end());
    });
}

void MetaEventMutator::set_tempo(EventId id, float bpm) {
    if (!(bpm > 0.0f)) {
        return;
    }
    const long micros = std::clamp(std::lround(60000000.0 / static_cast<double>(bpm)), 1L,
                                   0xFFFFFFL);
    edit_props(id, "set_tempo", [micros](MetaEvent& e) {
        if (e.event_type != MetaEventType::Tempo) {
            return;
        }
        e.data = {static_cast<std::uint8_t>((micros >> 16) & 0xFF),
                  static_cast<std::uint8_t>((micros >> 8) & 0xFF),
                  static_cast<std::uint8_t>(micros & 0xFF)};
    });
}

void MetaEventMutator::set_time_signature(EventId id, std::uint8_t numerator,
                                          std::uint8_t denominator_power,
                                          std::uint8_t clocks_per_click,
                                          std::uint8_t notated_32nds) {
    edit_props(id, "set_time_signature", [=](MetaEvent& e) {
        if (e.event_type != MetaEventType::TimeSignature) {
            return;
        }
        e.data = {numerator, denominator_power, clocks_per_click, notated_32nds};
    });
}

void MetaEventMutator::set_key_signature(EventId id, std::int8_t sharps_flats, bool minor) {
    const int sf = std::clamp<int>(sharps_flats, -7, 7);
    edit_props(id, "set_key_signature", [sf, minor](MetaEvent& e) {
        if (e.event_type != MetaEventType::KeySignature) {
            return;
        }
        e.data = {static_cast<std::uint8_t>(static_cast<std::int8_t>(sf)),
                  static_cast<std::uint8_t>(minor ? 1 : 0)};
    });
}

void MetaEventMutator::move_event(EventId id, SignedMIDITick delta_ticks) {
    EventPosition* p = pending_position(id, "move_event");
    if (p == nullptr) {
        return;
    }
    const SignedMIDITick tick = static_cast<SignedMIDITick>(p->tick) + delta_ticks;
    p->tick = static_cast<MIDITick>(tick < 0 ? 0 : tick);
}
void MetaEventMutator::move_events(std::span<const EventId> ids, SignedMIDITick delta_ticks) {
    for (const EventId id : ids) move_event(id, delta_ticks);
}
void MetaEventMutator::set_time(EventId id, MIDITick tick) {
    set_pending_position(id, EventPosition{tick}, "set_time");
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
    auto& list = tx_->sessions<Note>();
    if (notes.session_ == Transaction::kNoSession || notes.session_ >= list.size()) {
        return;
    }
    const TrackId track = list[notes.session_].track;

    tx_->snapshot_selection(track);
    std::vector<NoteId> remapped = notes.remap_ids(tx_->current_selection(track));
    tx_->write_selection(track, std::move(remapped));
}

// The generic mutator code lives in this file, so instantiate it for every element kind here.
template class EventMutatorBase<Note>;
template class EventMutatorBase<ChannelEvent>;
template class EventMutatorBase<MetaEvent>;

}