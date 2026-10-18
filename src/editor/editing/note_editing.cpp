#include "editor/editing/note_editing.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <shared_mutex>

#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "editor/settings/editor_settings.h"
#include "util/debugger.h"
#include "editor/editor_controller.h"
#include "app/main_window.h"

namespace andromeda::editor {

using namespace note_edit_flags;
using namespace util::math;
using app::EditorTool;
using midi::Note;
using util::Debugger;
namespace ns = note_seq;

void NoteEditing::update() {
    const auto& work_rect = context().rect;
    const Vector2<float>& mouse_pos_norm = *_app->get_mouse_pos();
    const PianoRollNavigation& nav = _app->nav->value;
    const app::KeyModifierState& key_modifiers = *_app->get_key_modifier_state();
    bool is_mouse_over_ui = _app->mouse_over_ui;

    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    const float min_x = work_rect.left + PR_KEYBOARD_WIDTH;
    const float max_x = work_rect.left + work_rect.width;
    const float min_y = work_rect.top;
    const float max_y = work_rect.top + work_rect.height;

    const bool mouse_over_roll = !is_mouse_over_ui &&
        mouse_pos_norm.x >= min_x && mouse_pos_norm.x < max_x &&
        mouse_pos_norm.y >= min_y && mouse_pos_norm.y < max_y;
    set_flag(NOTE_EDIT_MOUSE_OVER_ROLL, mouse_over_roll);

    const float cx = std::clamp(mouse_pos_norm.x, min_x, max_x);
    const float cy = std::clamp(mouse_pos_norm.y, min_y, max_y);
    const MousePianoRollPos mouse = get_mouse_midi_pos(work_rect, cx, cy, nav);

    mouse_info_.mouse_midi_pos = {mouse.tick, mouse.key};
    mouse_info_.mouse_midi_pos_rounded = {mouse.tick, mouse.key_rounded};

    disable_flag(NOTE_EDIT_MOUSE_OVER_NOTE);
    if (get_flag(NOTE_EDIT_MOUSE_OVER_UI) || !mouse_over_roll) {
        mouse_info_.note_hover_idx = std::nullopt;
        mouse_info_.is_at_note_end = false;
        set_flag(NOTE_EDIT_SHIFT_DOWN, key_modifiers.shift);
        return;
    }

    const auto curr_track = _controller->get_active_track();

    std::optional<std::size_t> mouse_note_hover_idx;
    if (static_cast<std::size_t>(curr_track) < tracks->size()) {
        const std::vector<Note>* notes = &tracks->at(curr_track).get_notes();
        if (!notes->empty()) {
            mouse_note_hover_idx = find_note_at(*notes, mouse.tick, mouse.key);
        }
    }

    if (mouse_note_hover_idx.has_value()) {
        enable_flag(NOTE_EDIT_MOUSE_OVER_NOTE);

        const Note& note = tracks->at(curr_track).get_notes()[*mouse_note_hover_idx];

        const float note_screen_width =
            (static_cast<float>(note.get_length()) / nav.zoom_ticks_smoothed) * work_rect.width;
        const float dist_to_end = (static_cast<float>(note.end()) - static_cast<float>(mouse.tick)) /
                                  nav.zoom_ticks_smoothed * work_rect.width;

        mouse_info_.is_at_note_end = note_screen_width > MIN_DRAGGABLE_WIDTH
                                         ? (dist_to_end >= 0.0f && dist_to_end < END_REGION)
                                         : false;
    } else {
        mouse_info_.is_at_note_end = false;
    }

    mouse_info_.note_hover_idx = mouse_note_hover_idx;
    set_flag(NOTE_EDIT_SHIFT_DOWN, key_modifiers.shift);
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

    app::EditorToolSettings* editor_tool = _controller->get_editor_tool_settings();
    switch (editor_tool->curr_tool) {
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

    app::EditorToolSettings* editor_tool = _controller->get_editor_tool_settings();
    switch (editor_tool->curr_tool) {
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

    app::EditorToolSettings* editor_tool = _controller->get_editor_tool_settings();
    switch (editor_tool->curr_tool) {
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

    app::EditorToolSettings* editor_tool = _controller->get_editor_tool_settings();
    switch (editor_tool->curr_tool) {
    case EditorTool::Pencil:   pencil_mouse_up(); break;
    case EditorTool::Eraser:   eraser_mouse_up(); break;
    case EditorTool::Selector: select_mouse_up(); break;
    }
}

void NoteEditing::on_key_down(const KeyState& keys) {
    const MIDITrk curr_track = _controller->get_active_track();

    SharedClipboard* clipboard = _controller->get_clipboard();
    SharedSelectedNotes* selection = _controller->get_selection();

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
        if (!clipboard->is_empty) {
            Debugger::log("Starting paste operation");
            paste_notes(curr_track);
            Debugger::log("Pasted");
        } else {
            Debugger::log("Nothing to paste");
        }
    }

    if (keys.duplicate) {
        if (selection->is_any_note_selected()) {
            Debugger::log("Duplicating...");
            duplicate_selected_notes();
            Debugger::log("Done");
        } else {
            Debugger::log("Nothing to duplicate");
        }
    }

    if (keys.del) {
        auto sel = selection->get_selected_ids_in_track(curr_track);
        delete_notes(*sel);
    }

    /*if (keys.del) {
        delete_notes_no_remap(selection->take_selected_from_track(curr_track));
    }*/
}

void NoteEditing::update_clicked_note() {
    const MIDITrk curr_track = _controller->get_active_track();
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    mouse_info_.last_clicked_note_idx = mouse_info_.note_hover_idx;
    mouse_info_.last_mouse_click_pos = mouse_info_.mouse_midi_pos;

    if (mouse_info_.last_clicked_note_idx.has_value()) {
        const Note& note =
            tracks->at(curr_track).get_notes()[*mouse_info_.last_clicked_note_idx];
        mouse_info_.last_clicked_note_pos = {note.get_start(), note.get_key()};
    }
}

void NoteEditing::pencil_mouse_down() {
    if (const auto clicked_idx = get_clicked_note_idx()) {
        const MIDITrk curr_track = _controller->get_active_track();
        SharedSelectedNotes* selection = _controller->get_selection();

        with_notes(curr_track, [&](const std::vector<Note>& notes) {
            update_toolbar_settings_from_note(notes[*clicked_idx]);
        });

        disable_flag(NOTE_EDIT_LENGTH_CHANGE | NOTE_EDIT_DRAGGING | NOTE_EDIT_MULTIEDIT);

        const bool is_multi = [&] {
            const std::vector<std::size_t>* ids = selection->get_selected_ids_in_track(curr_track);
            const bool is_multi_selected =
                ids != nullptr &&
                std::find(ids->begin(), ids->end(), *clicked_idx) != ids->end() && ids->size() > 1;

            if (selection->is_any_note_selected() && is_multi_selected) {
                return true;
            }
            selection->clear_selected();
            return false;
        }();

        const std::vector<std::size_t> single{*clicked_idx};
        // fixed rust bug: unwrapped the selection; falls back to the clicked note
        const auto& ids = [&]() -> const std::vector<std::size_t>& {
            if (is_multi) {
                if (const auto* sel = selection->get_selected_ids_in_track(curr_track)) {
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
        const MIDITrk curr_track = _controller->get_active_track();
        SharedSelectedNotes* selection = _controller->get_selection();

        const bool should_modify_selected = [&] {
            const std::vector<std::size_t>* selected =
                selection->get_selected_ids_in_track(curr_track);
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
                        selection->get_selected_ids_in_track(curr_track)) {
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
        const MIDITrk curr_track = _controller->get_active_track();

        std::vector<size_t> selected = with_notes(curr_track, [&](const std::vector<Note>& notes) {
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
                                 static_cast<std::uint8_t>(ghost_key), false);
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

void NoteEditing::clear_selected() { _controller->get_selection()->clear_selected(); }

void NoteEditing::select_notes(std::uint16_t track, std::vector<std::size_t> ids,
                               SelectionOp selection_op) {
    auto tx = _controller->begin_action("New selection");

    auto selection = tx.selection();

    switch (selection_op) {
    case SelectionOp::NewSelection: {
        if (ids.empty()) selection.clear_track(track);
        selection.set(track, ids);
        break;
    }
    case SelectionOp::AppendSelection: {
        if (!ids.empty()) selection.add(track, ids);
        break;
    }
    case SelectionOp::RemoveFromSelection:
        if (!ids.empty()) selection.remove(track, ids);
        break;
    }

    tx.commit();
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
    const MIDITrk curr_track = _controller->get_active_track();

    auto selected = with_notes(curr_track, [&](const std::vector<Note>& notes) {
        return get_notes_in_range(notes, min_tick, max_tick, min_key, max_key, true);
    });

    if (!selected.empty()) {
        delete_notes(std::move(selected));
    }
}

std::vector<std::pair<MIDITick, std::uint8_t>> NoteEditing::get_note_positions(
    const std::vector<std::size_t>& ids) const {
    const MIDITrk curr_track = _controller->get_active_track();
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    const std::vector<Note>& notes = tracks->at(curr_track).get_notes();

    std::vector<std::pair<MIDITick, std::uint8_t>> out;
    out.reserve(ids.size());
    for (const std::size_t id : ids) {
        out.emplace_back(notes[id].get_start(), notes[id].get_key());
    }
    return out;
}

std::vector<std::pair<std::size_t, MIDITick>> NoteEditing::get_note_lengths(
    const std::vector<std::size_t>& ids) const {
    const MIDITrk curr_track = _controller->get_active_track();
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    const std::vector<Note>& notes = tracks->at(curr_track).get_notes();

    std::vector<std::pair<std::size_t, MIDITick>> out;
    out.reserve(ids.size());
    for (const std::size_t id : ids) {
        out.emplace_back(id, notes[id].get_length());
    }
    return out;
}

void NoteEditing::offset_note_lengths(SignedMIDITick length_delta) {
    const MIDITrk curr_track = _controller->get_active_track();
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    std::vector<Note>& notes = tracks->at(curr_track).get_notes_mut();

    for (const auto& [note_id, old_length] : note_old_lengths_) {
        const SignedMIDITick new_length = static_cast<SignedMIDITick>(old_length) + length_delta;
        notes[note_id].length = new_length < 1 ? 1 : static_cast<MIDITick>(new_length);
    }
}

void NoteEditing::apply_note_length_change() {
    // std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    // EditorActions* actions = _controller->get_actions();

    // TODO: we just need the ids!
    std::vector<std::size_t> note_ids;
    std::vector<MIDITick> new_lengths;
    note_ids.reserve(note_old_lengths_.size());
    new_lengths.reserve(note_old_lengths_.size());
    
    const MIDITrk curr_track = _controller->get_active_track();
    with_notes(curr_track, [&](const std::vector<Note>& notes) {
        for (const auto& [id, old_len] : note_old_lengths_) {
            if (id < notes.size()) {
                note_ids.push_back(id);
                new_lengths.push_back(notes[id].get_length());
            }
        }
    });

    // we do need to revert the live notes back to their original pre-drag lengths
    // so transactions can be committed correctly.
    with_notes_mut(curr_track, [&](std::vector<Note>& notes) {
        for (const auto& [id, old_len] : note_old_lengths_) {
            if (id < notes.size()) {
                notes[id].length = old_len;
            }
        }
    });

    auto tx = _controller->begin_action("Change note length");
    auto notes = tx.notes(curr_track, note_ids);
    notes.set_length(note_ids, new_lengths);

    if (!tx.commit()) {
        Debugger::log_error("Failed to commit note length change transaction");
    }

    note_old_lengths_.clear();
}

void NoteEditing::selected_notes_to_ghost_notes() {
    const MIDITrk curr_track = _controller->get_active_track();
    SharedSelectedNotes* selection = _controller->get_selection();
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    // fixed rust bug: unwrapped this; an empty selection now returns early
    const std::vector<std::size_t>* selected =
        selection->get_selected_ids_in_track(curr_track);
    if (selected == nullptr || selected->empty()) {
        return;
    }

    std::vector<Note> old_notes;
    {
        old_notes = std::move(tracks->at(curr_track).get_notes());
        tracks->at(curr_track).get_notes_mut().clear();
    }

    auto [tmp_ghosts, new_notes] = ns::extract(std::move(old_notes), *selected);

    set_notes_in_track(curr_track, std::move(new_notes));

    (void)selection->take_selected_from_track(curr_track);

    ghost_notes_ = std::move(tmp_ghosts);
}

void NoteEditing::note_id_as_first_ghost_note(std::size_t id) {
    const MIDITrk curr_track = _controller->get_active_track();
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    Note note;
    note = ns::remove_note(tracks->at(curr_track).get_notes_mut(), id);

    if (ghost_notes_.empty()) {
        ghost_notes_.push_back(note);
    } else {
        ghost_notes_[0] = note;
    }
}

void NoteEditing::update_first_ghost_note() {
    const auto [mouse_tick, mouse_key] = mouse_info_.mouse_midi_pos;
    const std::uint8_t key = mouse_key > 127 ? 127 : mouse_key;

    const auto gn_start =
        static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(mouse_tick)));

    set_first_ghost_note_pos(gn_start, key);
}

void NoteEditing::set_first_ghost_note_pos(MIDITick start, std::uint8_t key, bool apply_tbs) {
    const auto [gn_channel, gn_length, gn_velocity] = get_tbs_values();

    if (ghost_notes_.empty()) {
        ghost_notes_.push_back(Note{start, gn_length, key, gn_velocity, gn_channel});
    } else {
        Note& ghost_note = ghost_notes_[0];
        ghost_note.start = start;
        ghost_note.key = key;

        if (apply_tbs) {
            ghost_note.length = gn_length;
            ghost_note.channel = gn_channel;
            ghost_note.velocity = gn_velocity;
        }
    }
}

void NoteEditing::offset_ghost_notes(std::pair<SignedMIDITick, std::int16_t> pos_delta) {
    const std::size_t count = std::min(ghost_notes_.size(), note_old_positions_.size());

    for (std::size_t i = 0; i < count; ++i) {
        const SignedMIDITick ghost_start =
            static_cast<SignedMIDITick>(note_old_positions_[i].first) + pos_delta.first;
        const auto ghost_key = static_cast<std::int16_t>(
            static_cast<std::int16_t>(note_old_positions_[i].second) + pos_delta.second);

        Note& gn = ghost_notes_[i];
        gn.start = ghost_start < 0 ? 0 : static_cast<MIDITick>(ghost_start);
        gn.key = ghost_key < 0    ? 0
                 : ghost_key > 127 ? 127
                                   : static_cast<std::uint8_t>(ghost_key);
    }
}

std::vector<Note> NoteEditing::ghost_notes_into_notes() {
    std::vector<Note> taken = std::move(ghost_notes_);
    ghost_notes_.clear();
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
    const MIDITrk curr_track = _controller->get_active_track();
    auto tx = _controller->begin_action("Place note(s)");

    // TODO: maybe a specialized "add_many_notes()" function so it's faster
    std::vector<Note> converted_notes = ghost_notes_into_notes();
    auto notes = tx.notes(curr_track, {});
    for (auto& note : converted_notes) {
        notes.add_note(std::move(note));
    }

    tx.commit();
}

void NoteEditing::apply_ghost_move_notes(
    std::vector<std::pair<SignedMIDITick, std::int16_t>> pos_deltas) {
    const MIDITrk curr_track = _controller->get_active_track();
    SharedSelectedNotes* selection = _controller->get_selection();

    // 1. Temporarily restore ghost notes to their old positions
    const std::size_t count = std::min(ghost_notes_.size(), note_old_positions_.size());
    for (std::size_t i = 0; i < count; ++i) {
        ghost_notes_[i].start = note_old_positions_[i].first;
        ghost_notes_[i].key = note_old_positions_[i].second;
    }

    // 2. Merge them back into the track to recover their original IDs
    std::vector<std::size_t> original_ids = merge_ghost_notes(curr_track);
    if (original_ids.empty()) {
        note_old_positions_.clear();
        return;
    }

    auto tx = _controller->begin_action("Move note(s)");
    auto notes_mutator = tx.notes(curr_track, original_ids);
    for (size_t i = 0; i < original_ids.size(); i++) {
        const auto [old_start, old_key] = note_old_positions_[i];
        const auto [delta_ticks, delta_keys] = pos_deltas[i];

        const SignedMIDITick new_start = static_cast<SignedMIDITick>(old_start) + delta_ticks;
        const int new_key = static_cast<int>(old_key) + delta_keys;

        const MIDITick clamped_start = new_start < 0 ? 0 : static_cast<MIDITick>(new_start);
        const uint8_t clamped_key = new_key < 0 ? 0 : (new_key > 127 ? 127 : static_cast<uint8_t>(new_key));

        notes_mutator.set_position(original_ids[i], clamped_start, clamped_key);
    }

    if (original_ids.size() > 1) {
        auto sel_mutator = tx.selection();
        sel_mutator.remap(notes_mutator);
    }
    
    if (!tx.commit()) {
        Debugger::log_error("Failed to commit move notes transaction.");
    }

    note_old_positions_.clear();
}

std::vector<std::pair<SignedMIDITick, std::int16_t>> NoteEditing::get_ghost_notes_pos_delta()
    const {
    std::vector<std::pair<SignedMIDITick, std::int16_t>> out;
    out.reserve(ghost_notes_.size());

    for (std::size_t i = 0; i < ghost_notes_.size(); ++i) {
        // fixed rust bug: indexed past the end of note_old_positions_
        if (i >= note_old_positions_.size()) {
            break;
        }
        const Note& note = ghost_notes_[i];
        const auto& old_pos = note_old_positions_[i];

        out.emplace_back(static_cast<SignedMIDITick>(note.get_start()) -
                             static_cast<SignedMIDITick>(old_pos.first),
                         static_cast<std::int16_t>(static_cast<std::int16_t>(note.get_key()) -
                                                   static_cast<std::int16_t>(old_pos.second)));
    }

    return out;
}

std::vector<Note> NoteEditing::take_notes_curr_track() {
    return take_notes_in_track(_controller->get_active_track());
}

std::vector<Note> NoteEditing::take_notes_in_track(std::uint16_t track) {
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    if (static_cast<std::size_t>(track) >= tracks->size()) {
        return {};
    }

    std::vector<Note> taken = std::move(tracks->at(track).get_notes());
    tracks->at(track).get_notes_mut().clear();
    return taken;
}

void NoteEditing::set_notes_in_track(std::uint16_t track, std::vector<Note> notes) {
    with_notes_mut(track, [&](std::vector<Note>& dst) { dst = std::move(notes); });
}

void NoteEditing::duplicate_selected_notes() {
    const MIDITrk curr_track = _controller->get_active_track();
    SharedSelectedNotes* selection = _controller->get_selection();

    const auto old_selected = selection->take_selected_from_track(curr_track);
    auto new_selected = duplicate_notes(curr_track, old_selected);

    selection->set_selected_in_track(std::move(new_selected), curr_track);
}

std::vector<std::size_t> NoteEditing::duplicate_notes(std::uint16_t track,
                                                      const std::vector<std::size_t>& ids) {
    if (ids.empty()) {
        return {};
    }

    auto copied_notes = clone_notes(track, ids);
    
    MIDITick min_tick = 0;
    MIDITick max_tick = 0;
    with_notes(track, [&](const std::vector<Note>& notes) {
        min_tick = notes[ids[0]].get_start();
        max_tick = get_absolute_max_tick_from_ids(notes, ids).value_or(min_tick);
    });

    auto moved = ns::move_all_notes_by(
        std::move(copied_notes),
        { static_cast<SignedMIDITick>(max_tick) - static_cast<SignedMIDITick>(min_tick), 0 });

    auto tx = _controller->begin_action("Duplicate notes");
    auto notes_mutator = tx.notes(track, {});

    std::vector<history::AddedNote> added_tickets;
    added_tickets.reserve(moved.size());
    for (const auto& note : moved) {
        added_tickets.push_back(notes_mutator.add_note(note));
    }

    auto sel_mutator = tx.selection();
    std::vector<std::size_t> dupe_ids;
    dupe_ids.reserve(added_tickets.size());
    for (const auto& ticket : added_tickets) {
        dupe_ids.push_back(notes_mutator.final_id(ticket));
    }
    sel_mutator.set(track, dupe_ids);

    if (!tx.commit()) {
        Debugger::log_error("Failed to commit duplicate notes transaction");
        return {};
    }

    return dupe_ids;
}

std::vector<Note> NoteEditing::clone_notes(std::uint16_t track, const std::vector<std::size_t>& ids) {
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
    SharedSelectedNotes* selection = _controller->get_selection();
    SharedClipboard* clipboard = _controller->get_clipboard();

    const std::vector<std::size_t>* selected =
        selection->get_selected_ids_in_track(track);
    if (selected == nullptr || selected->empty()) {
        Debugger::log_warning("Nothing copied.");
        return;
    }

    auto copied_notes = clone_notes(track, *selected);
    clipboard->move_notes_to_clipboard(std::move(copied_notes), track, true);
}

void NoteEditing::cut_selected_notes(std::uint16_t track) {
    SharedSelectedNotes* selection = _controller->get_selection();

    const std::vector<std::size_t>* selected = selection->get_selected_ids_in_track(track);
    if (selected == nullptr || selected->empty()) {
        Debugger::log_warning("Nothing cut.");
        return;
    }

    // just like before, we clone the notes in advance
    copy_notes(track);

    // but this time... remove the notes using the transaction api thingy
    auto tx = _controller->begin_action("Cut notes");
    auto notes_mutator = tx.notes(track, *selected);

    notes_mutator.delete_notes(*selected);
    auto sel_mutator = tx.selection();
    sel_mutator.remap(notes_mutator);

    if (!tx.commit()) {
        Debugger::log_error("Failed to commit cut notes transaction");
    }
}

void NoteEditing::paste_notes_offset(std::uint16_t track, MIDITick tick_pos) {
    SharedClipboard* clipboard = _controller->get_clipboard();
    auto _clipboard = clipboard->get_notes_from_clipboard();
    if (_clipboard.empty() || _clipboard[0].second.empty()) {
        return;
    }

    auto copied_notes = std::move(_clipboard[0].second);
    const MIDITick first_tick = copied_notes[0].get_start();
    for (Note& note : copied_notes) {
        note.start = (note.get_start() - first_tick) + tick_pos;
    }

    auto tx = _controller->begin_action("Paste notes");
    auto notes_mutator = tx.notes(track, {});
    for (const Note& note : copied_notes) {
        notes_mutator.add_note(note);
    }

    // remap selection just in case
    auto sel_mutator = tx.selection();
    sel_mutator.remap(notes_mutator);

    if (!tx.commit()) {
        Debugger::log_error("Failed to commit paste notes transaction");
    }
}

void NoteEditing::paste_notes(std::uint16_t track) {
    SharedSelectedNotes* selection = _controller->get_selection();
    SharedClipboard* clipboard = _controller->get_clipboard();
    auto _clipboard = clipboard->get_notes_from_clipboard();
    if (_clipboard.empty() || _clipboard[0].second.empty()) {
        return;
    }

    auto copied_notes = std::move(_clipboard[0].second);
    auto old_notes = take_notes_in_track(track);
    auto tx = _controller->begin_action("Paste notes");
    auto notes_mutator = tx.notes(track, {});

    std::vector<history::AddedNote> tickets{};
    tickets.reserve(copied_notes.size());
    for (const Note& note : copied_notes) {
        tickets.push_back(notes_mutator.add_note(note));
    }

    auto sel_mutator = tx.selection();
    std::vector<size_t> final_ids{};
    final_ids.reserve(tickets.size());
    for (const auto& ticket : tickets) {
        final_ids.push_back(notes_mutator.final_id(ticket));
    }

    sel_mutator.set(track, final_ids);

    if (!tx.commit()) {
        Debugger::log_error("Failed to commit paste notes transaction");
    }
}

void NoteEditing::delete_notes(std::vector<std::size_t> ids) {
    const MIDITrk curr_track = _controller->get_active_track();
    auto tx = _controller->begin_action("Delete note(s)");
    auto notes_mutator = tx.notes(curr_track, ids);
    notes_mutator.delete_notes(ids);
    if (!tx.commit()) {
        Debugger::log_error("Failed to commit notes deletion transaction");
    }
}

void NoteEditing::delete_notes_no_remap(std::vector<std::size_t> ids) {
    Debugger::log_error("DEPRECATED: use delete_notes instead");
}

void NoteEditing::apply_action(/*EditorAction& action*/) {
    Debugger::log_error("DEPRECATED: use new transaction api for applying actions (via undo/redo)");
}

void NoteEditing::update_toolbar_settings_from_note(const Note& note) const {
    app::ToolBarSettings* settings = _controller->get_toolbar_settings();

    settings->note_gate = static_cast<int>(note.get_length());
    settings->note_velocity = static_cast<int>(note.get_velocity());
    settings->note_channel = static_cast<int>(note.get_channel()) + 1;
}

void NoteEditing::update_toolbar_settings_from_clicked_note() const {
    const MIDITrk curr_track = _controller->get_active_track();
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    if (const auto clicked_idx = get_clicked_note_idx()) {
        update_toolbar_settings_from_note(tracks->at(curr_track).get_notes()[*clicked_idx]);
    }
}

void NoteEditing::update_latest_note_start() {
    std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    MIDITick latest_start = 0;
    for (midi::MIDITrack& track : *tracks) {
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
    app::ToolBarSettings* settings = _controller->get_toolbar_settings();

    return {static_cast<std::uint8_t>(settings->note_channel - 1),
            static_cast<MIDITick>(settings->note_gate),
            static_cast<std::uint8_t>(settings->note_velocity)};
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
    const app::EditorToolSettings* editor_tool = _controller->get_editor_tool_settings();
    const uint16_t ppq = _controller->get_project_manager()->get_ppq();

    const auto snap_ratio = editor_tool->snap_ratio;
    if (snap_ratio.first == 0 || snap_ratio.second == 0) {
        return 1;
    }
    return (static_cast<MIDITick>(ppq) * 4 * static_cast<MIDITick>(snap_ratio.first)) /
           static_cast<MIDITick>(snap_ratio.second);
}

std::pair<util::math::Vector2<float>, util::math::Vector2<float>> NoteEditing::get_selection_range_ui() const {
    const auto [r0, r1, r2, r3] = selection_range;

    const auto [min_tick, max_tick] = r0 > r1 ? std::pair{r1, r0} : std::pair{r0, r1};
    const auto [max_key, min_key] = r2 > r3 ? std::pair{r3, r2} : std::pair{r2, r3};

    return {midi_pos_to_ui_pos(min_tick, min_key),
            midi_pos_to_ui_pos(max_tick, max_key)};
}

util::math::Vector2<float>
NoteEditing::midi_pos_to_ui_pos(MIDITick tick_pos, std::uint8_t key_pos) const {
    PianoRollNavigation& nav = _app->nav->value;
    const editor::ViewRect& edit_region = context().rect;

    const float keyboard_width = PR_KEYBOARD_WIDTH / edit_region.width;
    util::math::Vector2<float> ui_pos = nav.midi_to_nav({ tick_pos, key_pos });

    ui_pos.x = ui_pos.x * (1.0f - keyboard_width) + keyboard_width;
    ui_pos.x = ui_pos.x * edit_region.width + edit_region.left;
    ui_pos.y = (1.0f - ui_pos.y) * edit_region.height + edit_region.top;

    return ui_pos;
}

EditCursor NoteEditing::get_cursor() const {
    if (get_flag(NOTE_EDIT_MOUSE_OVER_UI)) {
        return EditCursor::Default;
    }

    const bool is_at_note_end = mouse_info_.is_at_note_end;
    const app::EditorToolSettings* editor_tool = _controller->get_editor_tool_settings();

    switch (editor_tool->curr_tool) {
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

editor::ProjectManager* NoteEditing::get_project_manager() {
    return _controller->get_project_manager();
}

}
