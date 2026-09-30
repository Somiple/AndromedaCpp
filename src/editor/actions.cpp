#include "editor/actions.h"

#include <algorithm>
#include <utility>

#include "util/debugger.h"

namespace andromeda::editor {

using util::Debugger;

namespace {

template <typename T>
std::vector<T> negate_all(const std::vector<T>& v) {
    std::vector<T> out;
    out.reserve(v.size());
    for (const T& x : v) {
        out.push_back(static_cast<T>(-x));
    }
    return out;
}

std::vector<std::pair<SignedMIDITick, std::int16_t>> negate_pairs(
    const std::vector<std::pair<SignedMIDITick, std::int16_t>>& v) {
    std::vector<std::pair<SignedMIDITick, std::int16_t>> out;
    out.reserve(v.size());
    for (const auto& d : v) {
        out.emplace_back(-d.first, static_cast<std::int16_t>(-d.second));
    }
    return out;
}

}

void EditorActions::register_action(EditorAction action) {
    while (undo_depth_ > 0) {
        actions_.pop_back();
        undo_depth_ -= 1;
    }

    if (static_cast<std::uint16_t>(actions_.size()) == max_actions_) {
        actions_.pop_front();
    }

    actions_.push_back(std::move(action));
}

EditorAction* EditorActions::undo_action() {
    if (!get_can_undo()) {
        Debugger::log("Nothing to undo");
        return nullptr;
    }

    undo_depth_ += 1;

    const std::size_t lastmost_undo_index = actions_.size() - static_cast<std::size_t>(undo_depth_);
    EditorAction action_to_undo =
        std::move(actions_[lastmost_undo_index]);
    actions_.erase(actions_.begin() + static_cast<std::ptrdiff_t>(lastmost_undo_index));
    action_to_undo = invert_action(std::move(action_to_undo));
    actions_.insert(actions_.begin() + static_cast<std::ptrdiff_t>(lastmost_undo_index),
                    std::move(action_to_undo));

    return &actions_[lastmost_undo_index];
}

EditorAction* EditorActions::redo_action() {
    if (!get_can_redo()) {
        Debugger::log("Nothing to redo");
        return nullptr;
    }

    const std::size_t lastmost_redo_index = actions_.size() - static_cast<std::size_t>(undo_depth_);
    EditorAction action_to_redo = std::move(actions_[lastmost_redo_index]);
    actions_.erase(actions_.begin() + static_cast<std::ptrdiff_t>(lastmost_redo_index));
    action_to_redo = invert_action(std::move(action_to_redo));
    actions_.insert(actions_.begin() + static_cast<std::ptrdiff_t>(lastmost_redo_index),
                    std::move(action_to_redo));

    undo_depth_ -= 1;

    return &actions_[lastmost_redo_index];
}

bool EditorActions::get_can_undo() const {
    if (actions_.empty() || undo_depth_ == max_actions_ ||
        undo_depth_ == static_cast<std::uint16_t>(actions_.size())) {
        return false;
    }
    return true;
}

bool EditorActions::get_can_redo() const { return undo_depth_ != 0; }

EditorAction EditorActions::invert_action(EditorAction action) {
    return std::visit(
        [this](auto&& a) -> EditorAction {
            using T = std::decay_t<decltype(a)>;

            if constexpr (std::is_same_v<T, PlaceNotes>) {
                return DeleteNotes{std::move(a.note_ids), std::move(a.notes), a.note_group};
            } else if constexpr (std::is_same_v<T, DeleteNotes>) {
                return PlaceNotes{std::move(a.note_ids), std::move(a.notes), a.note_group};
            } else if constexpr (std::is_same_v<T, PlaceNotesMultiTrack>) {
                return DeleteNotesMultiTrack{std::move(a.note_ids), std::move(a.notes),
                                             std::move(a.note_groups)};
            } else if constexpr (std::is_same_v<T, DeleteNotesMultiTrack>) {
                return PlaceNotesMultiTrack{std::move(a.note_ids), std::move(a.notes),
                                            std::move(a.note_groups)};
            } else if constexpr (std::is_same_v<T, LengthChange>) {
                return LengthChange{std::move(a.note_ids), negate_all(a.length_delta),
                                    a.note_group};
            } else if constexpr (std::is_same_v<T, ChannelChange>) {
                return ChannelChange{std::move(a.note_ids), negate_all(a.channel_delta),
                                     a.note_group};
            } else if constexpr (std::is_same_v<T, VelocityChange>) {
                return VelocityChange{std::move(a.note_ids), negate_all(a.velocity_delta),
                                      a.note_group};
            } else if constexpr (std::is_same_v<T, KeyChange>) {
                return KeyChange{std::move(a.note_ids), negate_all(a.key_delta), a.note_group};
            } else if constexpr (std::is_same_v<T, NotesMove>) {
                return NotesMove{std::move(a.note_ids), negate_pairs(a.midi_pos_delta),
                                 a.note_group, a.update_selected_ids};
            } else if constexpr (std::is_same_v<T, NotesMoveImmediate>) {
                return NotesMoveImmediate{std::move(a.note_ids), negate_pairs(a.midi_pos_delta),
                                          a.note_group};
            } else if constexpr (std::is_same_v<T, NotesMoveMultiTrack>) {
                return NotesMoveMultiTrack{
                    std::move(a.note_track_ids),
                    {-a.moved_by.first, static_cast<std::int16_t>(-a.moved_by.second)}};
            } else if constexpr (std::is_same_v<T, Select>) {
                return Deselect{std::move(a.note_ids), a.note_group};
            } else if constexpr (std::is_same_v<T, Deselect>) {
                return Select{std::move(a.note_ids), a.note_group};
            } else if constexpr (std::is_same_v<T, Duplicate>) {
                return DeleteNotes{std::move(a.note_ids), std::nullopt, a.dest_track};
            } else if constexpr (std::is_same_v<T, AddMeta>) {
                return DeleteMeta{std::move(a.meta_ids), std::move(a.metas)};
            } else if constexpr (std::is_same_v<T, DeleteMeta>) {
                return AddMeta{std::move(a.meta_ids), std::move(a.metas)};
            } else if constexpr (std::is_same_v<T, AddTrack>) {
                return RemoveTrack{a.track, std::move(a.deleted_tracks), a.last_track};
            } else if constexpr (std::is_same_v<T, RemoveTrack>) {
                return AddTrack{a.track, std::move(a.deleted_tracks), a.last_track};
            } else if constexpr (std::is_same_v<T, SwapTracks>) {
                return SwapTracks{a.track_1, a.track_2};
            } else if constexpr (std::is_same_v<T, DecomposeTrack>) {
                return ComposeTrack{a.track, a.channel_count};
            } else if constexpr (std::is_same_v<T, ComposeTrack>) {
                return DecomposeTrack{a.track, a.channel_count};
            } else {
                static_assert(std::is_same_v<T, Bulk>);

                std::vector<EditorAction> inv_actions;
                inv_actions.reserve(a.actions.size());
                for (EditorAction& inner : a.actions) {
                    inv_actions.push_back(invert_action(std::move(inner)));
                }
                std::reverse(inv_actions.begin(), inv_actions.end());
                return Bulk{std::move(inv_actions)};
            }
        },
        std::move(action.node));
}

void EditorActions::clear_actions() {
    actions_.clear();
    undo_depth_ = 0;
}

}
