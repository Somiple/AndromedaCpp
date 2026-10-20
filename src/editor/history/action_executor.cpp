#include "editor/history/action_executor.h"

#include <algorithm>
#include <format>
#include <functional>
#include <utility>

#include "util/debugger.h"

namespace andromeda::editor::history {

using midi::MIDITrack;
using midi::Note;
using util::Debugger;

namespace {

template <typename V>
bool strictly_ascending(const V& v) {
    return std::adjacent_find(v.begin(), v.end(), std::greater_equal<>{}) == v.end();
}

// moves the items at `positions` (ascending) into `out`, compacting the rest in place
template <typename Item>
void erase_at(std::vector<Item>& items, const std::vector<EventId>& positions,
              std::vector<Item>& out) {
    out.reserve(positions.size());
    if (positions.empty()) {
        return;
    }

    std::size_t write = positions.front();
    std::size_t next = 0;
    for (std::size_t read = positions.front(); read < items.size(); ++read) {
        if (next < positions.size() && read == positions[next]) {
            out.push_back(std::move(items[read]));
            ++next;
        } else {
            if (write != read) {
                items[write] = std::move(items[read]);
            }
            ++write;
        }
    }
    items.resize(write);
}

// puts payload[j] at final index positions[j] (ascending), filling from the back so every
// existing item moves at most once and nothing is allocated beyond the resize
template <typename Item>
void insert_at(std::vector<Item>& items, const std::vector<EventId>& positions,
               std::vector<Item>& payload) {
    const std::size_t count = positions.size();
    if (count == 0) {
        return;
    }

    const std::size_t old_size = items.size();
    items.resize(old_size + count);

    std::size_t dst = old_size + count;
    std::size_t src = old_size;
    for (std::size_t j = count; j-- > 0;) {
        const std::size_t target = positions[j];
        while (dst > target + 1) {
            items[--dst] = std::move(items[--src]);
        }
        items[--dst] = std::move(payload[j]);
    }
}

std::string track_name(TrackId track) {
    return track == kGlobalTrack ? std::string("global") : std::to_string(track);
}

std::string describe(const Change& change, ProjectManager& project) {
    auto& tracks = *project.get_tracks();
    const auto last = [](const std::vector<EventId>& v) -> std::string {
        return v.empty() ? "-" : std::to_string(v.back());
    };

    return std::visit(
        [&](const auto& c) -> std::string {
            using T = std::decay_t<decltype(c)>;
            if constexpr (is_presence_change_v<T>) {
                using Traits = EventTraits<typename T::item_type>;
                const auto* items = items_for<typename T::item_type>(project, c.track);
                const std::string count =
                    items != nullptr ? std::to_string(items->size()) : "no such track";
                return std::format(
                    "{} presence, track {}: track has {} items now | remove_at {} (last {}) | "
                    "insert_at {} (last {}) | payload {}",
                    Traits::accessor, track_name(c.track), count, c.remove_at.size(), last(c.remove_at),
                    c.insert_at.size(), last(c.insert_at), c.payload.size());
            } else if constexpr (is_property_change_v<T>) {
                using Traits = EventTraits<typename T::item_type>;
                return std::format("{} property edit, track {} ({} edits, {} tracks in project)",
                                   Traits::accessor, track_name(c.track), c.edits.size(),
                                   tracks.size());
            } else if constexpr (std::is_same_v<T, SelectionSwapChange>) {
                return std::format("SelectionSwap track {} ({} tracks in project)", c.track,
                                   tracks.size());
            } else if constexpr (std::is_same_v<T, TrackPresenceChange>) {
                return std::format("TrackPresence index {} ({}), project has {} tracks", c.index,
                                   c.payload ? "insert" : "remove", tracks.size());
            } else {
                return std::format("change type #{}", Change(c).index());
            }
        },
        change);
}

// heap held by the elements themselves, beyond sizeof(Item)
template <typename Item>
std::size_t extra_heap(const Item&) {
    return 0;
}
std::size_t extra_heap(const midi::MetaEvent& e) { return e.data.capacity(); }
std::size_t extra_heap(const MetaEventProps& p) { return p.data.capacity(); }

}

std::size_t approx_bytes(Change& change) {
    return std::visit(
        [](auto& c) -> std::size_t {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, TrackPresenceChange>) {
                std::size_t bytes = sizeof(T);
                if (c.payload) {
                    bytes += c.payload->get_notes().size() * sizeof(Note);
                }
                return bytes;
            } else if constexpr (is_property_change_v<T>) {
                std::size_t bytes = sizeof(T) + c.edits.capacity() * sizeof(c.edits[0]);
                for (const auto& e : c.edits) {
                    bytes += extra_heap(e.before) + extra_heap(e.after);
                }
                return bytes;
            } else if constexpr (is_presence_change_v<T>) {
                // payload is empty in one of the two directions, but the items live in the
                // history in the other one, so budget for whichever side is larger
                const std::size_t count =
                    std::max({c.payload.size(), c.remove_at.size(), c.insert_at.size()});
                std::size_t bytes = sizeof(T) +
                                    count * (sizeof(typename T::item_type) + sizeof(EventId)) +
                                    c.insert_at.size() * sizeof(EventId);
                for (const auto& item : c.payload) {
                    bytes += extra_heap(item);
                }
                return bytes;
            } else if constexpr (std::is_same_v<T, SelectionSwapChange>) {
                return sizeof(T) + c.ids.capacity() * sizeof(NoteId);
            } else {
                return sizeof(T);
            }
        },
        change);
}

bool ActionExecutor::apply(Change& change) {
    if (ctx_.project == nullptr || ctx_.selection == nullptr) {
        Debugger::log_error("ActionExecutor has no project or selection");
        return false;
    }

    const bool ok = std::visit([this](auto& c) { return apply_one(c); }, change);
    if (!ok) {
        return false;
    }

    std::visit(
        [this](auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, TrackPresenceChange> ||
                          std::is_same_v<T, TrackSwapChange> ||
                          std::is_same_v<T, TrackMoveChange>) {
                notify({ChangeKind::Tracks, 0});
            } else if constexpr (std::is_same_v<T, SelectionSwapChange>) {
                notify({ChangeKind::Selection, c.track});
            } else if constexpr (std::is_same_v<T, ActiveTrackChange>) {
                // c.track now holds the previous value; the new one is in the controller
                notify({ChangeKind::ActiveTrack, *ctx_.active_track});
            } else {
                // property / presence changes: the kind comes from the element type
                using Item = typename T::item_type;
                notify({kind_for<Item>(c.track), c.track});
            }
        },
        change);
    return true;
}

bool ActionExecutor::run(Action& action, bool reversed) {
    const std::size_t count = action.changes.size();
    const auto at = [&](std::size_t i) -> Change& {
        return reversed ? action.changes[count - 1 - i] : action.changes[i];
    };

    std::size_t done = 0;
    for (; done < count; ++done) {
        if (!apply(at(done))) {
            break;
        }
    }
    if (done == count) {
        return true;
    }

    Debugger::log_error(std::format("Action '{}' could not be {}; restoring the previous state",
                                    action.label, reversed ? "undone" : "redone"));
    Debugger::log_error(std::format("  change {} of {} failed: {}", done + 1, count,
                                    describe(at(done), *ctx_.project)));

    while (done > 0) {
        --done;
        if (!apply(at(done))) {
            Debugger::log_error("Failed to restore a change; the project may be inconsistent");
        }
    }
    return false;
}

bool ActionExecutor::apply_one(TrackPresenceChange& c) {
    auto& tracks = *ctx_.project->get_tracks();

    if (c.payload) {
        if (c.index > tracks.size()) {
            return false;
        }
        tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(c.index),
                      std::move(*c.payload));
        c.payload.reset();
        return true;
    }

    // the project always keeps at least one track
    if (c.index >= tracks.size() || tracks.size() <= 1) {
        return false;
    }
    c.payload = std::move(tracks[c.index]);
    tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(c.index));
    return true;
}

bool ActionExecutor::apply_one(TrackSwapChange& c) {
    auto& tracks = *ctx_.project->get_tracks();
    if (c.a >= tracks.size() || c.b >= tracks.size()) {
        return false;
    }
    if (c.a != c.b) {
        std::swap(tracks[c.a], tracks[c.b]);
    }
    return true;
}

bool ActionExecutor::apply_one(TrackMoveChange& c) {
    auto& tracks = *ctx_.project->get_tracks();
    if (c.from >= tracks.size() || c.to >= tracks.size()) {
        return false;
    }

    const auto it = [&](std::size_t i) { return tracks.begin() + static_cast<std::ptrdiff_t>(i); };
    if (c.from < c.to) {
        std::rotate(it(c.from), it(c.from + 1), it(c.to + 1));
    } else if (c.to < c.from) {
        std::rotate(it(c.to), it(c.from), it(c.from + 1));
    }
    std::swap(c.from, c.to);
    return true;
}

template <typename Item>
bool ActionExecutor::apply_one(PropertyChange<Item>& c) {
    using Traits = EventTraits<Item>;

    std::vector<Item>* items_ptr = items_for<Item>(*ctx_.project, c.track);
    if (items_ptr == nullptr) {
        return false;
    }
    auto& items = *items_ptr;

    for (const PropertyEdit<Item>& e : c.edits) {
        if (e.index >= items.size()) {
            return false;
        }
    }

    for (PropertyEdit<Item>& e : c.edits) {
        Traits::write(items[e.index], e.after);
        std::swap(e.before, e.after);
    }
    return true;
}

template <typename Item>
bool ActionExecutor::apply_one(PresenceChange<Item>& c) {
    std::vector<Item>* items_ptr = items_for<Item>(*ctx_.project, c.track);
    if (items_ptr == nullptr) {
        return false;
    }
    auto& items = *items_ptr;
    const std::size_t size = items.size();

    if (c.payload.size() != c.insert_at.size() || !strictly_ascending(c.remove_at) ||
        !strictly_ascending(c.insert_at)) {
        return false;
    }
    if (!c.remove_at.empty() && c.remove_at.back() >= size) {
        return false;
    }
    const std::size_t final_size = size - c.remove_at.size() + c.insert_at.size();
    if (!c.insert_at.empty() && c.insert_at.back() >= final_size) {
        return false;
    }

    std::vector<Item> pulled;
    erase_at(items, c.remove_at, pulled);
    insert_at(items, c.insert_at, c.payload);

    c.payload = std::move(pulled);
    c.remove_at.swap(c.insert_at);
    return true;
}

bool ActionExecutor::apply_one(SelectionSwapChange& c) {
    if (c.track >= ctx_.project->get_tracks()->size()) {
        return false;
    }

    std::vector<NoteId> previous = ctx_.selection->take_selected_from_track(c.track);
    if (!c.ids.empty()) {
        ctx_.selection->set_selected_in_track(std::move(c.ids), c.track);
    }
    c.ids = std::move(previous);
    return true;
}

bool ActionExecutor::apply_one(ActiveTrackChange& c) {
    if (ctx_.active_track == nullptr) {
        return false;
    }
    std::swap(*ctx_.active_track, c.track);
    return true;
}

}