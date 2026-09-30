#include "editor/editing/note_editing.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <shared_mutex>

#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "editor/settings/editor_settings.h"
#include "util/debugger.h"

namespace andromeda::editor {

using namespace note_edit_flags;
using app::EditorTool;
using midi::Note;
using util::Debugger;
namespace ns = note_seq;

NoteEditing::NoteEditing(util::SharedPtr<std::vector<midi::MIDITrack>> tracks,
                         util::SharedPtr<PianoRollNavigation> nav,
                         std::shared_ptr<app::EditorToolSettings> editor_tool,
                         std::shared_ptr<EditorActions> editor_actions,
                         std::shared_ptr<app::ToolBarSettings> toolbar_settings,
                         std::shared_ptr<SharedClipboard> shared_clipboard,
                         std::shared_ptr<SharedSelectedNotes> shared_selected_note_ids)
    : tracks_(std::move(tracks)),
      ghost_notes_(util::make_shared_mut<std::vector<Note>>()),
      shared_selected_note_ids_(std::move(shared_selected_note_ids)),
      nav_(std::move(nav)),
      editor_tool_(std::move(editor_tool)),
      editor_actions_(std::move(editor_actions)),
      toolbar_settings_(std::move(toolbar_settings)),
      shared_clipboard_(std::move(shared_clipboard)) {}

void NoteEditing::on_event(const app::AndromedaEvent& event) {
    if (const auto* ppq_changed = std::get_if<app::PPQChanged>(&event)) {
        ppq = ppq_changed->new_ppq;
    }
}

void NoteEditing::update(const ViewRect& rect, float mouse_x, float mouse_y, bool mouse_over,
                         bool shift_down) {
    PianoRollNavigation nav_copy;
    {
        std::shared_lock lock(nav_->mutex);
        nav_copy = nav_->value;
    }

    const MousePianoRollPos mouse =
        get_mouse_midi_pos(rect, mouse_x, mouse_y, mouse_over, nav_copy);

    mouse_info_.mouse_midi_pos = {mouse.tick, mouse.key};
    mouse_info_.mouse_midi_pos_rounded = {mouse.tick, mouse.key_rounded};

    disable_flag(NOTE_EDIT_MOUSE_OVER_NOTE);
    if (get_flag(NOTE_EDIT_MOUSE_OVER_UI)) {
        mouse_info_.note_hover_idx = std::nullopt;
        return;
    }

    const auto curr_track = get_current_track();

    std::optional<std::size_t> mouse_note_hover_idx;
    {
        std::shared_lock lock(tracks_->mutex);
        if (static_cast<std::size_t>(curr_track) < tracks_->value.size()) {
            const std::vector<Note>& notes = tracks_->value[curr_track].get_notes();
            if (!notes.empty()) {
                mouse_note_hover_idx = find_note_at(notes, mouse.tick, mouse.key);
            }
        }
    }

    if (mouse_note_hover_idx.has_value()) {
        enable_flag(NOTE_EDIT_MOUSE_OVER_NOTE);

        std::shared_lock lock(tracks_->mutex);
        const Note& note = tracks_->value[curr_track].get_notes()[*mouse_note_hover_idx];

        const float note_screen_width =
            (static_cast<float>(note.get_length()) / nav_copy.zoom_ticks_smoothed) * rect.width;
        const float dist_to_end = (static_cast<float>(note.end()) - static_cast<float>(mouse.tick)) /
                                  nav_copy.zoom_ticks_smoothed * rect.width;

        mouse_info_.is_at_note_end = note_screen_width > MIN_DRAGGABLE_WIDTH
                                         ? (dist_to_end >= 0.0f && dist_to_end < END_REGION)
                                         : false;
    } else {
        mouse_info_.is_at_note_end = false;
    }

    mouse_info_.note_hover_idx = mouse_note_hover_idx;
    set_flag(NOTE_EDIT_SHIFT_DOWN, shift_down);
}

void NoteEditing::on_mouse_down() {
    if (get_flag(NOTE_EDIT_MOUSE_OVER_UI)) {
        enable_flag(NOTE_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (get_flag(NOTE_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }

    update_clicked_note();
    update_latest_note_start();

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:   pencil_mouse_down(); break;
    case EditorTool::Eraser:   eraser_mouse_down(); break;
    case EditorTool::Selector: select_mouse_down(); break;
    }
}

void NoteEditing::on_right_mouse_down() {
    if (get_flag(NOTE_EDIT_MOUSE_OVER_UI)) {
        enable_flag(NOTE_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (get_flag(NOTE_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }

    update_clicked_note();
    update_latest_note_start();

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:
    case EditorTool::Eraser:
        eraser_mouse_down();
        break;
    default:
        break;
    }
}

void NoteEditing::on_mouse_move() {
    if (get_flag(NOTE_EDIT_MOUSE_DOWN_ON_UI)) {
        return;
    }
    if (get_flag(NOTE_EDIT_ANY_DIALOG_OPEN | NOTE_EDIT_MOUSE_OVER_UI)) {
        return;
    }

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:   pencil_mouse_move(); break;
    case EditorTool::Eraser:   eraser_mouse_move(); break;
    case EditorTool::Selector: select_mouse_move(); break;
    }
}

void NoteEditing::on_mouse_up() {
    if (get_flag(NOTE_EDIT_MOUSE_DOWN_ON_UI)) {
        disable_flag(NOTE_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (get_flag(NOTE_EDIT_MOUSE_OVER_UI | NOTE_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:   pencil_mouse_up(); break;
    case EditorTool::Eraser:   eraser_mouse_up(); break;
    case EditorTool::Selector: select_mouse_up(); break;
    }
}

void NoteEditing::on_key_down(const KeyState& keys) {
    const auto curr_track = get_current_track();
    if (get_flag(NOTE_EDIT_ANY_DIALOG_OPEN | NOTE_EDIT_MOUSE_OVER_UI)) {
        return;
    }

    if (keys.copy) {
        Debugger::log("Copied");
        copy_notes(curr_track);
    }

    if (keys.cut) {
        Debugger::log("Cut");
        cut_selected_notes(curr_track);
    }

    if (keys.paste) {
        if (!shared_clipboard_->is_empty) {
            Debugger::log("Starting paste operation");
            paste_notes(curr_track);
            Debugger::log("Pasted");
        } else {
            Debugger::log("Nothing to paste");
        }
    }

    if (keys.duplicate) {
        if (shared_selected_note_ids_->is_any_note_selected()) {
            Debugger::log("Duplicating...");
            duplicate_selected_notes();
            Debugger::log("Done");
        } else {
            Debugger::log("Nothing to duplicate");
        }
    }

    if (keys.del) {
        delete_notes_no_remap(shared_selected_note_ids_->take_selected_from_track(curr_track));
    }
}

void NoteEditing::update_clicked_note() {
    const auto curr_track = get_current_track();

    mouse_info_.last_clicked_note_idx = mouse_info_.note_hover_idx;
    mouse_info_.last_mouse_click_pos = mouse_info_.mouse_midi_pos;

    if (mouse_info_.last_clicked_note_idx.has_value()) {
        std::shared_lock lock(tracks_->mutex);
        const Note& note =
            tracks_->value[curr_track].get_notes()[*mouse_info_.last_clicked_note_idx];
        mouse_info_.last_clicked_note_pos = {note.get_start(), note.get_key()};
    }
}

void NoteEditing::pencil_mouse_down() {
    if (const auto clicked_idx = get_clicked_note_idx()) {
        const auto curr_track = get_current_track();

        with_notes(curr_track, [&](const std::vector<Note>& notes) {
            update_toolbar_settings_from_note(notes[*clicked_idx]);
        });

        disable_flag(NOTE_EDIT_LENGTH_CHANGE | NOTE_EDIT_DRAGGING | NOTE_EDIT_MULTIEDIT);

        const bool is_multi = [&] {
            const std::vector<std::size_t>* ids =
                shared_selected_note_ids_->get_selected_ids_in_track(curr_track);
            const bool is_multi_selected =
                ids != nullptr &&
                std::find(ids->begin(), ids->end(), *clicked_idx) != ids->end() && ids->size() > 1;

            if (shared_selected_note_ids_->is_any_note_selected() && is_multi_selected) {
                return true;
            }
            shared_selected_note_ids_->clear_selected();
            return false;
        }();

        const std::vector<std::size_t> single{*clicked_idx};
        // fixed rust bug: unwrapped the selection; falls back to the clicked note
        const auto& ids = [&]() -> const std::vector<std::size_t>& {
            if (is_multi) {
                if (const auto* sel =
                        shared_selected_note_ids_->get_selected_ids_in_track(curr_track)) {
                    return *sel;
                }
            }
            return single;
        }();

        if (mouse_info_.is_at_note_end) {
            enable_flag(NOTE_EDIT_LENGTH_CHANGE);
            note_old_lengths_ = get_note_lengths(ids);
        } else {
            enable_flag(NOTE_EDIT_DRAGGING);
            note_old_positions_ = get_note_positions(ids);

            if (is_multi) {
                selected_notes_to_ghost_notes();
            } else {
                note_id_as_first_ghost_note(*clicked_idx);
            }

            enable_flag(NOTE_EDIT_SYNTH_PLAY);
        }

        if (is_multi) {
            enable_flag(NOTE_EDIT_MULTIEDIT);
        }
    } else {
        clear_selected();
        update_first_ghost_note();
        enable_flag(NOTE_EDIT_IS_EDITING);

        enable_flag(NOTE_EDIT_SYNTH_PLAY);
    }
}

void NoteEditing::pencil_mouse_move() {
    if (get_flag(NOTE_EDIT_IS_EDITING)) {
        update_first_ghost_note();
    } else if (get_flag(NOTE_EDIT_DRAGGING)) {
        offset_ghost_notes_tmp();
    } else if (get_flag(NOTE_EDIT_LENGTH_CHANGE)) {
        offset_notes_length_tmp();
    }
}

void NoteEditing::pencil_mouse_up() {
    if (get_flag(NOTE_EDIT_IS_EDITING)) {
        apply_ghost_place_notes();
        disable_flag(NOTE_EDIT_IS_EDITING);
        disable_flag(NOTE_EDIT_SYNTH_PLAY);
    } else if (get_flag(NOTE_EDIT_DRAGGING)) {
        apply_ghost_move_notes(get_ghost_notes_pos_delta());
        disable_flag(NOTE_EDIT_DRAGGING);
        disable_flag(NOTE_EDIT_SYNTH_PLAY);
    } else if (get_flag(NOTE_EDIT_LENGTH_CHANGE)) {
        apply_note_length_change();
        disable_flag(NOTE_EDIT_LENGTH_CHANGE);

        if (!get_flag(NOTE_EDIT_MULTIEDIT)) {
            update_toolbar_settings_from_clicked_note();
        }
    }
}

void NoteEditing::select_mouse_down() {
    disable_flag(NOTE_EDIT_DRAGGING | NOTE_EDIT_LENGTH_CHANGE | NOTE_EDIT_MULTIEDIT);

    if (const auto clicked_idx = get_clicked_note_idx()) {
        const auto curr_track = get_current_track();

        const bool should_modify_selected = [&] {
            const std::vector<std::size_t>* selected =
                shared_selected_note_ids_->get_selected_ids_in_track(curr_track);
            return selected != nullptr && !selected->empty() &&
                   std::find(selected->begin(), selected->end(), *clicked_idx) != selected->end();
        }();

        if (should_modify_selected) {
            enable_flag(NOTE_EDIT_MULTIEDIT);
        }

        const std::vector<std::size_t> single{*clicked_idx};
        const auto& ids = [&]() -> const std::vector<std::size_t>& {
            if (should_modify_selected) {
                if (const auto* sel =
                        shared_selected_note_ids_->get_selected_ids_in_track(curr_track)) {
                    return *sel;
                }
            }
            return single;
        }();

        if (mouse_info_.is_at_note_end) {
            enable_flag(NOTE_EDIT_LENGTH_CHANGE);
            note_old_lengths_ = get_note_lengths(ids);
        } else {
            enable_flag(NOTE_EDIT_DRAGGING);
            note_old_positions_ = get_note_positions(ids);

            if (should_modify_selected) {
                selected_notes_to_ghost_notes();
            } else {
                clear_selected();
                note_id_as_first_ghost_note(*clicked_idx);
            }
        }
    } else {
        init_selection_box(mouse_info_.mouse_midi_pos_rounded);
    }
}

void NoteEditing::select_mouse_move() {
    if (get_flag(NOTE_EDIT_DRAGGING)) {
        offset_ghost_notes_tmp();
    } else if (get_flag(NOTE_EDIT_LENGTH_CHANGE)) {
        offset_notes_length_tmp();
    } else if (draw_select_box_) {
        update_selection_box(mouse_info_.mouse_midi_pos_rounded);
    }
}

void NoteEditing::select_mouse_up() {
    if (get_flag(NOTE_EDIT_DRAGGING)) {
        apply_ghost_move_notes(get_ghost_notes_pos_delta());
        disable_flag(NOTE_EDIT_DRAGGING);
    } else if (get_flag(NOTE_EDIT_LENGTH_CHANGE)) {
        apply_note_length_change();
        disable_flag(NOTE_EDIT_LENGTH_CHANGE);

        if (!get_flag(NOTE_EDIT_MULTIEDIT)) {
            update_toolbar_settings_from_clicked_note();
        }
    } else {
        draw_select_box_ = false;

        const auto [min_tick, max_tick, min_key, max_key] = get_selection_range();
        const auto curr_track = get_current_track();

        const auto selected = with_notes(curr_track, [&](const std::vector<Note>& notes) {
            return get_notes_in_range(notes, min_tick, max_tick, min_key, max_key, true);
        });

        select_notes(curr_track, selected,
                     get_flag(NOTE_EDIT_SHIFT_DOWN) ? SelectionOp::AppendSelection
                                                    : SelectionOp::NewSelection);
    }
}

void NoteEditing::offset_ghost_notes_tmp() {
    const auto [clicked_note_start, clicked_note_key] = get_clicked_note_pos();

    const SignedMIDITick mouse_delta_ticks =
        static_cast<SignedMIDITick>(mouse_info_.mouse_midi_pos.first) -
        static_cast<SignedMIDITick>(mouse_info_.last_mouse_click_pos.first);
    const SignedMIDITick snapped_delta = snap_tick(mouse_delta_ticks);

    if (get_flag(NOTE_EDIT_MULTIEDIT)) {
        const auto offset_key = static_cast<std::int16_t>(
            static_cast<std::int16_t>(mouse_info_.mouse_midi_pos.second) -
            static_cast<std::int16_t>(clicked_note_key));

        offset_ghost_notes({snapped_delta, offset_key});
    } else {
        SignedMIDITick ghost_start =
            static_cast<SignedMIDITick>(clicked_note_start) + snapped_delta;
        if (ghost_start < 0) {
            ghost_start = 0;
        }

        const auto key_offs = static_cast<std::int16_t>(
            static_cast<std::int16_t>(clicked_note_key) -
            static_cast<std::int16_t>(mouse_info_.last_clicked_note_pos.second));

        auto ghost_key = static_cast<std::int16_t>(
            static_cast<std::int16_t>(mouse_info_.mouse_midi_pos.second) + key_offs);
        if (ghost_key < 0) {
            ghost_key = 0;
        } else if (ghost_key > 127) {
            ghost_key = 127;
        }

        set_first_ghost_note_pos(static_cast<MIDITick>(ghost_start),
                                 static_cast<std::uint8_t>(ghost_key));
    }
}

void NoteEditing::offset_notes_length_tmp() {
    const SignedMIDITick mouse_delta_ticks =
        static_cast<SignedMIDITick>(mouse_info_.mouse_midi_pos.first) -
        static_cast<SignedMIDITick>(mouse_info_.last_mouse_click_pos.first);

    offset_note_lengths(snap_tick(mouse_delta_ticks));
}

void NoteEditing::init_selection_box(std::pair<MIDITick, std::uint8_t> start_pos) {
    const auto snapped_tick =
        static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(start_pos.first)));
    selection_range = {snapped_tick, snapped_tick, start_pos.second, start_pos.second};
    draw_select_box_ = true;
}

void NoteEditing::update_selection_box(std::pair<MIDITick, std::uint8_t> new_pos) {
    std::get<1>(selection_range) =
        static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(new_pos.first)));
    std::get<3>(selection_range) = new_pos.second;
}

std::tuple<MIDITick, MIDITick, std::uint8_t, std::uint8_t> NoteEditing::get_selection_range() const {
    const auto [r0, r1, r2, r3] = selection_range;

    const auto [min_tick, max_tick] = r0 > r1 ? std::pair{r1, r0} : std::pair{r0, r1};
    const auto [min_key, max_key] = r2 > r3 ? std::pair{r3, r2} : std::pair{r2, r3};

    return {min_tick, max_tick, min_key, max_key};
}

void NoteEditing::clear_selected() { shared_selected_note_ids_->clear_selected(); }

void NoteEditing::select_notes(std::uint16_t track, std::vector<std::size_t> ids,
                               SelectionOp selection_op) {
    switch (selection_op) {
    case SelectionOp::NewSelection: {
        bool has_selection = false;

        auto old_selected_ids = shared_selected_note_ids_->take_selected_from_track(track);
        const bool had_old_selection = !old_selected_ids.empty();

        if (!ids.empty()) {
            shared_selected_note_ids_->set_selected_in_track(ids, track);
            has_selection = true;
        }

        std::vector<EditorAction> actions;
        actions.reserve(2);
        if (had_old_selection) {
            actions.emplace_back(Deselect{std::move(old_selected_ids), track});
        }
        if (has_selection) {
            actions.emplace_back(Select{std::move(ids), track});
        }

        editor_actions_->register_action(Bulk{std::move(actions)});
        break;
    }
    case SelectionOp::AppendSelection: {
        if (ids.empty()) {
            break;
        }

        const std::vector<std::size_t>* old_selected_ids =
            shared_selected_note_ids_->get_selected_ids_in_track(track);

        if (old_selected_ids != nullptr) {
            auto added_sel_ids = ns::exclude(ids, *old_selected_ids);

            if (!added_sel_ids.empty()) {
                auto old_ids = shared_selected_note_ids_->take_selected_from_track(track);
                auto new_ids = ns::merge_unique(std::move(old_ids), added_sel_ids);
                shared_selected_note_ids_->set_selected_in_track(std::move(new_ids), track);

                editor_actions_->register_action(Select{std::move(added_sel_ids), track});
            }
        }
        break;
    }
    case SelectionOp::RemoveFromSelection:
        break;
    }
}

void NoteEditing::eraser_mouse_down() {
    if (const auto clicked_note_idx = get_clicked_note_idx()) {
        delete_notes({*clicked_note_idx});
    } else {
        enable_flag(NOTE_EDIT_ERASING);
        init_selection_box(mouse_info_.mouse_midi_pos);
    }
}

void NoteEditing::eraser_mouse_move() {
    if (draw_select_box_) {
        update_selection_box(mouse_info_.mouse_midi_pos);
    }
}

void NoteEditing::eraser_mouse_up() {
    disable_flag(NOTE_EDIT_ERASING);
    draw_select_box_ = false;

    const auto [min_tick, max_tick, min_key, max_key] = get_selection_range();
    const auto curr_track = get_current_track();

    auto selected = with_notes(curr_track, [&](const std::vector<Note>& notes) {
        return get_notes_in_range(notes, min_tick, max_tick, min_key, max_key, true);
    });

    if (!selected.empty()) {
        delete_notes(std::move(selected));
    }
}

std::vector<std::pair<MIDITick, std::uint8_t>> NoteEditing::get_note_positions(
    const std::vector<std::size_t>& ids) const {
    const auto curr_track = get_current_track();

    std::shared_lock lock(tracks_->mutex);
    const std::vector<Note>& notes = tracks_->value[curr_track].get_notes();

    std::vector<std::pair<MIDITick, std::uint8_t>> out;
    out.reserve(ids.size());
    for (const std::size_t id : ids) {
        out.emplace_back(notes[id].get_start(), notes[id].get_key());
    }
    return out;
}

std::vector<std::pair<std::size_t, MIDITick>> NoteEditing::get_note_lengths(
    const std::vector<std::size_t>& ids) const {
    const auto curr_track = get_current_track();

    std::shared_lock lock(tracks_->mutex);
    const std::vector<Note>& notes = tracks_->value[curr_track].get_notes();

    std::vector<std::pair<std::size_t, MIDITick>> out;
    out.reserve(ids.size());
    for (const std::size_t id : ids) {
        out.emplace_back(id, notes[id].get_length());
    }
    return out;
}

void NoteEditing::offset_note_lengths(SignedMIDITick length_delta) {
    const auto curr_track = get_current_track();

    std::unique_lock lock(tracks_->mutex);
    std::vector<Note>& notes = tracks_->value[curr_track].get_notes_mut();

    for (const auto& [note_id, old_length] : note_old_lengths_) {
        const SignedMIDITick new_length = static_cast<SignedMIDITick>(old_length) + length_delta;
        notes[note_id].length = new_length < 1 ? 1 : static_cast<MIDITick>(new_length);
    }
}

void NoteEditing::apply_note_length_change() {
    const auto curr_track = get_current_track();

    std::vector<std::size_t> note_ids;
    std::vector<SignedMIDITick> length_deltas;
    {
        std::shared_lock lock(tracks_->mutex);
        const std::vector<Note>& notes = tracks_->value[curr_track].get_notes();

        const auto old_lengths = std::move(note_old_lengths_);
        note_old_lengths_.clear();

        note_ids.reserve(old_lengths.size());
        length_deltas.reserve(old_lengths.size());
        for (const auto& [id, length] : old_lengths) {
            note_ids.push_back(id);
            length_deltas.push_back(static_cast<SignedMIDITick>(notes[id].get_length()) -
                                    static_cast<SignedMIDITick>(length));
        }
    }

    editor_actions_->register_action(
        LengthChange{std::move(note_ids), std::move(length_deltas), curr_track});
}

void NoteEditing::selected_notes_to_ghost_notes() {
    const auto curr_track = get_current_track();

    // fixed rust bug: unwrapped this; an empty selection now returns early
    const std::vector<std::size_t>* selected =
        shared_selected_note_ids_->get_selected_ids_in_track(curr_track);
    if (selected == nullptr || selected->empty()) {
        return;
    }

    std::vector<Note> old_notes;
    {
        std::unique_lock lock(tracks_->mutex);
        old_notes = std::move(tracks_->value[curr_track].get_notes_mut());
        tracks_->value[curr_track].get_notes_mut().clear();
    }

    auto [tmp_ghosts, new_notes] = ns::extract(std::move(old_notes), *selected);

    set_notes_in_track(curr_track, std::move(new_notes));

    (void)shared_selected_note_ids_->take_selected_from_track(curr_track);

    std::lock_guard lock(ghost_notes_->mutex);
    ghost_notes_->value = std::move(tmp_ghosts);
}

void NoteEditing::note_id_as_first_ghost_note(std::size_t id) {
    const auto curr_track = get_current_track();

    Note note;
    {
        std::unique_lock lock(tracks_->mutex);
        note = ns::remove_note(tracks_->value[curr_track].get_notes_mut(), id);
    }

    std::lock_guard lock(ghost_notes_->mutex);
    if (ghost_notes_->value.empty()) {
        ghost_notes_->value.push_back(note);
    } else {
        ghost_notes_->value[0] = note;
    }
}

void NoteEditing::update_first_ghost_note() {
    const auto [mouse_tick, mouse_key] = mouse_info_.mouse_midi_pos;
    const std::uint8_t key = mouse_key > 127 ? 127 : mouse_key;

    const auto gn_start =
        static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(mouse_tick)));

    set_first_ghost_note_pos(gn_start, key);
}

void NoteEditing::set_first_ghost_note_pos(MIDITick start, std::uint8_t key) {
    const auto [gn_channel, gn_length, gn_velocity] = get_tbs_values();

    std::lock_guard lock(ghost_notes_->mutex);
    if (ghost_notes_->value.empty()) {
        ghost_notes_->value.push_back(Note{start, gn_length, key, gn_velocity, gn_channel});
    } else {
        Note& ghost_note = ghost_notes_->value[0];
        ghost_note.start = start;
        ghost_note.length = gn_length;
        ghost_note.channel = gn_channel;
        ghost_note.key = key;
        ghost_note.velocity = gn_velocity;
    }
}

void NoteEditing::offset_ghost_notes(std::pair<SignedMIDITick, std::int16_t> pos_delta) {
    std::lock_guard lock(ghost_notes_->mutex);

    const std::size_t count = std::min(ghost_notes_->value.size(), note_old_positions_.size());

    for (std::size_t i = 0; i < count; ++i) {
        const SignedMIDITick ghost_start =
            static_cast<SignedMIDITick>(note_old_positions_[i].first) + pos_delta.first;
        const auto ghost_key = static_cast<std::int16_t>(
            static_cast<std::int16_t>(note_old_positions_[i].second) + pos_delta.second);

        Note& gn = ghost_notes_->value[i];
        gn.start = ghost_start < 0 ? 0 : static_cast<MIDITick>(ghost_start);
        gn.key = ghost_key < 0    ? 0
                 : ghost_key > 127 ? 127
                                   : static_cast<std::uint8_t>(ghost_key);
    }
}

std::vector<Note> NoteEditing::ghost_notes_into_notes() {
    std::lock_guard lock(ghost_notes_->mutex);
    std::vector<Note> taken = std::move(ghost_notes_->value);
    ghost_notes_->value.clear();
    return taken;
}

std::vector<std::size_t> NoteEditing::merge_ghost_notes(std::uint16_t track) {
    auto ghost_notes = ghost_notes_into_notes();
    auto curr_notes_track = take_notes_curr_track();

    auto [merged, ids] =
        ns::merge_notes_and_return_ids(std::move(curr_notes_track), std::move(ghost_notes));
    set_notes_in_track(track, std::move(merged));

    return ids;
}

void NoteEditing::apply_ghost_place_notes() {
    const auto curr_track = get_current_track();

    auto ids = merge_ghost_notes(curr_track);
    editor_actions_->register_action(PlaceNotes{std::move(ids), std::nullopt, curr_track});
}

void NoteEditing::apply_ghost_move_notes(
    std::vector<std::pair<SignedMIDITick, std::int16_t>> pos_deltas) {
    const auto curr_track = get_current_track();

    auto ids = merge_ghost_notes(curr_track);

    bool is_editing_selected = false;
    if (pos_deltas.size() > 1) {
        shared_selected_note_ids_->set_selected_in_track(ids, curr_track);
        is_editing_selected = true;
    }

    editor_actions_->register_action(
        NotesMove{std::move(ids), std::move(pos_deltas), curr_track, is_editing_selected});
}

std::vector<std::pair<SignedMIDITick, std::int16_t>> NoteEditing::get_ghost_notes_pos_delta()
    const {
    std::lock_guard lock(ghost_notes_->mutex);

    std::vector<std::pair<SignedMIDITick, std::int16_t>> out;
    out.reserve(ghost_notes_->value.size());

    for (std::size_t i = 0; i < ghost_notes_->value.size(); ++i) {
        // fixed rust bug: indexed past the end of note_old_positions_
        if (i >= note_old_positions_.size()) {
            break;
        }
        const Note& note = ghost_notes_->value[i];
        const auto& old_pos = note_old_positions_[i];

        out.emplace_back(static_cast<SignedMIDITick>(note.get_start()) -
                             static_cast<SignedMIDITick>(old_pos.first),
                         static_cast<std::int16_t>(static_cast<std::int16_t>(note.get_key()) -
                                                   static_cast<std::int16_t>(old_pos.second)));
    }

    return out;
}

std::vector<Note> NoteEditing::take_notes_curr_track() {
    return take_notes_in_track(get_current_track());
}

std::vector<Note> NoteEditing::take_notes_in_track(std::uint16_t track) {
    std::unique_lock lock(tracks_->mutex);
    if (static_cast<std::size_t>(track) >= tracks_->value.size()) {
        return {};
    }

    std::vector<Note> taken = std::move(tracks_->value[track].get_notes_mut());
    tracks_->value[track].get_notes_mut().clear();
    return taken;
}

void NoteEditing::set_notes_in_track(std::uint16_t track, std::vector<Note> notes) {
    with_notes_mut(track, [&](std::vector<Note>& dst) { dst = std::move(notes); });
}

void NoteEditing::duplicate_selected_notes() {
    const auto curr_track = get_current_track();

    const auto old_selected = shared_selected_note_ids_->take_selected_from_track(curr_track);
    auto new_selected = duplicate_notes(curr_track, old_selected);

    shared_selected_note_ids_->set_selected_in_track(std::move(new_selected), curr_track);
}

std::vector<std::size_t> NoteEditing::duplicate_notes(std::uint16_t track,
                                                      const std::vector<std::size_t>& ids) {
    if (ids.empty()) {
        return {};
    }

    auto copied_notes = clone_notes(track, ids);
    auto old_notes = take_notes_in_track(track);

    const MIDITick min_tick = old_notes[ids[0]].get_start();
    const MIDITick max_tick = get_absolute_max_tick_from_ids(old_notes, ids).value_or(min_tick);
    auto moved = ns::move_all_notes_by(
        std::move(copied_notes),
        {static_cast<SignedMIDITick>(max_tick) - static_cast<SignedMIDITick>(min_tick), 0});

    auto [merged, dupe_ids] =
        ns::merge_notes_and_return_ids(std::move(old_notes), std::move(moved));
    set_notes_in_track(track, std::move(merged));

    editor_actions_->register_action(PlaceNotes{dupe_ids, std::nullopt, track});

    return dupe_ids;
}

std::vector<Note> NoteEditing::clone_notes(std::uint16_t track,
                                           const std::vector<std::size_t>& ids) const {
    return with_notes(track, [&](const std::vector<Note>& notes) {
        std::vector<Note> copied;
        copied.reserve(ids.size());
        for (const std::size_t id : ids) {
            copied.push_back(notes[id]);
        }
        return copied;
    });
}

void NoteEditing::copy_notes(std::uint16_t track) {
    const std::vector<std::size_t>* selected =
        shared_selected_note_ids_->get_selected_ids_in_track(track);
    if (selected == nullptr || selected->empty()) {
        Debugger::log_warning("Nothing copied.");
        return;
    }

    auto copied_notes = clone_notes(track, *selected);
    shared_clipboard_->move_notes_to_clipboard(std::move(copied_notes), track, true);
}

void NoteEditing::cut_selected_notes(std::uint16_t track) {
    auto old_notes = take_notes_in_track(track);
    auto selected = shared_selected_note_ids_->take_selected_from_track(track);

    auto [notes_to_cut, new_notes] = ns::extract(std::move(old_notes), selected);
    set_notes_in_track(track, std::move(new_notes));

    shared_clipboard_->move_notes_to_clipboard(notes_to_cut, track, true);

    editor_actions_->register_action(
        DeleteNotes{std::move(selected), std::move(notes_to_cut), track});
}

void NoteEditing::paste_notes_offset(std::uint16_t track, MIDITick tick_pos) {
    auto clipboard = shared_clipboard_->get_notes_from_clipboard();
    if (clipboard.empty() || clipboard[0].second.empty()) {
        return;
    }

    auto copied_notes = std::move(clipboard[0].second);

    const MIDITick first_tick = copied_notes[0].get_start();
    for (Note& note : copied_notes) {
        note.start = (note.get_start() - first_tick) + tick_pos;
    }

    auto old_notes = take_notes_in_track(track);
    auto [new_notes, new_ids] =
        ns::merge_notes_and_return_ids(std::move(old_notes), std::move(copied_notes));
    set_notes_in_track(track, std::move(new_notes));

    editor_actions_->register_action(PlaceNotes{std::move(new_ids), std::nullopt, track});
}

void NoteEditing::paste_notes(std::uint16_t track) {
    auto clipboard = shared_clipboard_->get_notes_from_clipboard();
    if (clipboard.empty()) {
        return;
    }

    auto copied_notes = std::move(clipboard[0].second);

    auto old_notes = take_notes_in_track(track);
    auto [new_notes, new_ids] =
        ns::merge_notes_and_return_ids(std::move(old_notes), std::move(copied_notes));
    set_notes_in_track(track, std::move(new_notes));

    shared_selected_note_ids_->set_selected_in_track(new_ids, track);

    editor_actions_->register_action(PlaceNotes{std::move(new_ids), std::nullopt, track});
}

void NoteEditing::delete_notes(std::vector<std::size_t> ids) {
    const auto curr_track = get_current_track();

    auto old_notes = take_notes_in_track(curr_track);

    auto [deleted_notes, new_notes, selected_] = ns::extract_and_remap_ids(
        std::move(old_notes), ids,
        shared_selected_note_ids_->take_selected_from_track(curr_track));

    shared_selected_note_ids_->set_selected_in_track(std::move(selected_), curr_track);
    set_notes_in_track(curr_track, std::move(new_notes));

    editor_actions_->register_action(
        DeleteNotes{std::move(ids), std::move(deleted_notes), curr_track});
}

void NoteEditing::delete_notes_no_remap(std::vector<std::size_t> ids) {
    const auto curr_track = get_current_track();

    auto old_notes = take_notes_in_track(curr_track);

    auto [deleted_notes, new_notes] = ns::extract(std::move(old_notes), ids);
    set_notes_in_track(curr_track, std::move(new_notes));

    editor_actions_->register_action(
        DeleteNotes{std::move(ids), std::move(deleted_notes), curr_track});
}

void NoteEditing::apply_action(EditorAction& action) {
    if (auto* place = std::get_if<PlaceNotes>(&action.node)) {
        Debugger::log("Undoing or redoing note deletion");
        if (!place->notes.has_value()) {
            Debugger::log_error(
                "[PLACE_NOTES] Something has gone wrong while undoing/redoing note deletion.");
            return;
        }

        auto recovered_notes = std::move(*place->notes);
        place->notes.reset();

        auto old_notes = take_notes_in_track(place->note_group);
        auto merged = ns::merge_notes(std::move(old_notes), std::move(recovered_notes));
        set_notes_in_track(place->note_group, std::move(merged));
    } else if (auto* del = std::get_if<DeleteNotes>(&action.node)) {
        Debugger::log("Undoing or redoing placing notes");
        auto old_notes = take_notes_in_track(del->note_group);

        auto old_sel_ids = shared_selected_note_ids_->take_selected_from_track(del->note_group);
        auto [deleted, new_notes, new_ids] =
            ns::extract_and_remap_ids(std::move(old_notes), del->note_ids, old_sel_ids);
        shared_selected_note_ids_->set_selected_in_track(std::move(new_ids), del->note_group);

        set_notes_in_track(del->note_group, std::move(new_notes));
        del->notes = std::move(deleted);
    } else if (auto* move = std::get_if<NotesMove>(&action.node)) {
        Debugger::log("Undoing or redoing moving notes");
        auto old_notes = take_notes_in_track(move->note_group);
        auto [notes_to_move, remaining] = ns::extract(std::move(old_notes), move->note_ids);

        auto notes_with_dt = ns::move_each_note_by(std::move(notes_to_move), move->midi_pos_delta);

        std::vector<Note> moved;
        std::vector<std::pair<SignedMIDITick, std::int16_t>> notes_dt;
        moved.reserve(notes_with_dt.size());
        notes_dt.reserve(notes_with_dt.size());
        for (auto& [note, dt] : notes_with_dt) {
            moved.push_back(note);
            notes_dt.push_back(dt);
        }

        auto [merged, new_ids] =
            ns::merge_notes_and_return_ids(std::move(remaining), std::move(moved));
        set_notes_in_track(move->note_group, std::move(merged));

        if (move->update_selected_ids) {
            shared_selected_note_ids_->set_selected_in_track(new_ids, move->note_group);
        }

        move->note_ids = std::move(new_ids);
        move->midi_pos_delta = std::move(notes_dt);
    } else if (auto* chan = std::get_if<ChannelChange>(&action.node)) {
        Debugger::log("Undoing or redoing changing note channels");
        with_notes_mut(chan->note_group, [&](std::vector<Note>& notes) {
            for (std::size_t i = 0; i < chan->note_ids.size(); ++i) {
                Note& note = notes[chan->note_ids[i]];
                auto new_channel =
                    static_cast<std::int8_t>(static_cast<std::int8_t>(note.get_channel()) +
                                             chan->channel_delta[i]);
                if (new_channel < 0) {
                    new_channel = 0;
                }
                if (new_channel > 15) {
                    new_channel = 15;
                }

                chan->channel_delta[i] = static_cast<std::int8_t>(
                    new_channel - static_cast<std::int8_t>(note.get_channel()));
                note.channel = static_cast<std::uint8_t>(new_channel);
            }
        });
    } else if (auto* len = std::get_if<LengthChange>(&action.node)) {
        Debugger::log("Undoing or redoing changing lengths");
        with_notes_mut(len->note_group, [&](std::vector<Note>& notes) {
            for (std::size_t i = 0; i < len->note_ids.size(); ++i) {
                Note& note = notes[len->note_ids[i]];
                SignedMIDITick new_length =
                    static_cast<SignedMIDITick>(note.get_length()) + len->length_delta[i];
                if (new_length < 1) {
                    new_length = 1;
                }

                len->length_delta[i] =
                    new_length - static_cast<SignedMIDITick>(note.get_length());
                note.set_length(static_cast<MIDITick>(new_length));
            }
        });
    } else if (auto* vel = std::get_if<VelocityChange>(&action.node)) {
        Debugger::log("Undoing or redoing changing note velocities");
        with_notes_mut(vel->note_group, [&](std::vector<Note>& notes) {
            for (std::size_t i = 0; i < vel->note_ids.size(); ++i) {
                Note& note = notes[vel->note_ids[i]];
                auto new_velocity = static_cast<std::int8_t>(
                    static_cast<std::int8_t>(note.get_velocity()) + vel->velocity_delta[i]);
                if (new_velocity < 1) {
                    new_velocity = 1;
                }

                vel->velocity_delta[i] = static_cast<std::int8_t>(
                    new_velocity - static_cast<std::int8_t>(note.get_velocity()));
                note.set_velocity(static_cast<std::uint8_t>(new_velocity));
            }
        });
    } else if (auto* keyc = std::get_if<KeyChange>(&action.node)) {
        Debugger::log("Undoing or redoing changing note keys");
        with_notes_mut(keyc->note_group, [&](std::vector<Note>& notes) {
            for (std::size_t i = 0; i < keyc->note_ids.size(); ++i) {
                Note& note = notes[keyc->note_ids[i]];
                auto new_key = static_cast<SignedMIDIKey>(
                    static_cast<SignedMIDIKey>(note.get_key()) + keyc->key_delta[i]);
                if (new_key < 0) {
                    new_key = 0;
                }

                keyc->key_delta[i] = static_cast<SignedMIDIKey>(
                    new_key - static_cast<SignedMIDIKey>(note.get_key()));
                note.set_key(static_cast<std::uint8_t>(new_key));
            }
        });
    } else if (auto* sel = std::get_if<Select>(&action.node)) {
        Debugger::log("Undoing or redoing changing note slection");
        auto old_selection = shared_selected_note_ids_->take_selected_from_track(sel->note_group);

        if (old_selection.empty()) {
            shared_selected_note_ids_->set_selected_in_track(sel->note_ids, sel->note_group);
        } else {
            auto new_selection = ns::merge_unique(std::move(old_selection), sel->note_ids);
            shared_selected_note_ids_->set_selected_in_track(std::move(new_selection),
                                                             sel->note_group);
        }
    } else if (auto* desel = std::get_if<Deselect>(&action.node)) {
        Debugger::log("Undoing or redoing note deselection");
        auto old_selection = shared_selected_note_ids_->take_selected_from_track(desel->note_group);
        auto new_selection = ns::exclude(std::move(old_selection), desel->note_ids);
        shared_selected_note_ids_->set_selected_in_track(std::move(new_selection),
                                                         desel->note_group);
    } else if (auto* bulk = std::get_if<Bulk>(&action.node)) {
        Debugger::log(std::format("Applying {} actions in bulk", bulk->actions.size()));
        for (auto it = bulk->actions.rbegin(); it != bulk->actions.rend(); ++it) {
            apply_action(*it);
        }
    }
}

std::uint16_t NoteEditing::get_current_track() const {
    std::shared_lock lock(nav_->mutex);
    return nav_->value.curr_track;
}

void NoteEditing::update_toolbar_settings_from_note(const Note& note) const {
    toolbar_settings_->note_gate = static_cast<int>(note.get_length());
    toolbar_settings_->note_velocity = static_cast<int>(note.get_velocity());
    toolbar_settings_->note_channel = static_cast<int>(note.get_channel()) + 1;
}

void NoteEditing::update_toolbar_settings_from_clicked_note() const {
    const auto curr_track = get_current_track();
    if (const auto clicked_idx = get_clicked_note_idx()) {
        std::shared_lock lock(tracks_->mutex);
        update_toolbar_settings_from_note(tracks_->value[curr_track].get_notes()[*clicked_idx]);
    }
}

void NoteEditing::update_latest_note_start() {
    std::shared_lock lock(tracks_->mutex);

    MIDITick latest_start = 0;
    for (const midi::MIDITrack& track : tracks_->value) {
        const std::vector<Note>& notes = track.get_notes();
        if (notes.empty()) {
            continue;
        }

        if (notes.back().get_start() >= latest_start) {
            latest_start = notes.back().get_start();
        }
    }

    latest_note_start = latest_start + 38400;
}

std::tuple<std::uint8_t, MIDITick, std::uint8_t> NoteEditing::get_tbs_values() const {
    return {static_cast<std::uint8_t>(toolbar_settings_->note_channel - 1),
            static_cast<MIDITick>(toolbar_settings_->note_gate),
            static_cast<std::uint8_t>(toolbar_settings_->note_velocity)};
}

SignedMIDITick NoteEditing::snap_tick(SignedMIDITick tick) const {
    const auto snap = static_cast<SignedMIDITick>(get_min_snap_tick_length());
    if (snap == 1) {
        return tick;
    }

    const SignedMIDITick half = snap / 2;
    if (tick >= 0) {
        return ((tick + half) / snap) * snap;
    }
    return ((tick - half) / snap) * snap;
}

MIDITick NoteEditing::get_min_snap_tick_length() const {
    const auto snap_ratio = editor_tool_->snap_ratio;
    if (snap_ratio.first == 0 || snap_ratio.second == 0) {
        return 1;
    }
    return (static_cast<MIDITick>(ppq) * 4 * static_cast<MIDITick>(snap_ratio.first)) /
           static_cast<MIDITick>(snap_ratio.second);
}

std::pair<std::pair<float, float>, std::pair<float, float>> NoteEditing::get_selection_range_ui(
    const ViewRect& rect) const {
    const auto [r0, r1, r2, r3] = selection_range;

    const auto [min_tick, max_tick] = r0 > r1 ? std::pair{r1, r0} : std::pair{r0, r1};
    const auto [max_key, min_key] = r2 > r3 ? std::pair{r3, r2} : std::pair{r2, r3};

    return {midi_pos_to_ui_pos(rect, min_tick, min_key),
            midi_pos_to_ui_pos(rect, max_tick, max_key)};
}

std::pair<float, float> NoteEditing::midi_pos_to_ui_pos(const ViewRect& rect, MIDITick tick_pos,
                                                        std::uint8_t key_pos) const {
    std::shared_lock lock(nav_->mutex);
    const PianoRollNavigation& nav = nav_->value;

    const float keyboard_width = PR_KEYBOARD_WIDTH / rect.width;

    float ui_x = (static_cast<float>(tick_pos) - nav.tick_pos_smoothed) / nav.zoom_ticks_smoothed;
    float ui_y = (static_cast<float>(key_pos) - nav.key_pos_smoothed) / nav.zoom_keys_smoothed;

    ui_x = ui_x * (1.0f - keyboard_width) + keyboard_width;
    ui_x = ui_x * rect.width + rect.left;
    ui_y = (1.0f - ui_y) * rect.height + rect.top;

    return {ui_x, ui_y};
}

EditCursor NoteEditing::get_cursor() const {
    if (get_flag(NOTE_EDIT_MOUSE_OVER_UI)) {
        return EditCursor::Default;
    }

    const bool is_at_note_end = mouse_info_.is_at_note_end;

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:
        if (is_at_note_end) {
            return EditCursor::ResizeHorizontal;
        }
        if (get_flag(NOTE_EDIT_MOUSE_OVER_NOTE)) {
            return EditCursor::Move;
        }
        break;
    case EditorTool::Eraser:
        break;
    case EditorTool::Selector:
        if (is_at_note_end) {
            return EditCursor::ResizeHorizontal;
        }
        if (get_flag(NOTE_EDIT_MOUSE_OVER_NOTE)) {
            return EditCursor::Move;
        }
        return EditCursor::Crosshair;
    }

    return EditCursor::Default;
}

}
