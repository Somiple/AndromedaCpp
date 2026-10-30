#include "editor/editing/track_editing.h"

#include <algorithm>
#include <limits>
#include <map>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <algorithm>

#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "core/debugger.h"
#include "core/numeric.h"
#include "editor/editor_controller.h"
#include "ui/app/main_window.h"
// REFACTOR: <ui/app/editor_app.h> dropped — this pass now reads the transport through
// editor::IPlaybackProvider instead of reaching into the window's EditorApp.
#include "ui/app/managers.h"

namespace andromeda::editor {

namespace {

using TrackIds = std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>>;

// Deletes notes in any number of tracks as ONE action. Transaction::notes() is rejected once
// selection() has been requested, so all mutators are created first and the selection is
// remapped afterwards.
bool delete_notes_in_tracks(EditorController* controller, std::string label,
                            const TrackIds& targets) {
    auto tx = controller->begin_action(std::move(label));

    std::vector<history::NoteMutator> mutators;
    for (const auto& [trk, ids] : targets) {
        if (ids.empty()) continue;
        auto m = tx.notes(trk, ids);
        m.delete_notes(ids);
        mutators.push_back(std::move(m));
    }

    auto sel = tx.selection();
    for (const auto& m : mutators) sel.remap(m);

    return tx.commit();
}
}

using app::EditorTool;
using midi::ChannelEvent;
using midi::MIDITrack;
using midi::Note;
using note_seq::extract;
using note_seq::extract_and_remap_ids;
using note_seq::merge_notes;
using note_seq::merge_notes_and_return_ids;
using util::Debugger;
using util::saturating_cast;

using namespace track_flags;
using namespace util::math;

void TrackEditing::update() {
    // pretty weird but we could initialize stuff here
    {
        if (!_overlay_drawer) _overlay_drawer = std::make_unique<overlays::OverlayDrawer>(&context());
    }

    const Vector2<float>& mouse_pos_norm = *mouse_pos();
    mouse_info_.mouse_pos = { mouse_pos_norm.x, mouse_pos_norm.y };
    mouse_info_.mouse_midi_track_pos = screen_pos_to_midi_track_pos(mouse_pos_norm);

    // REFACTOR: same value as get_playback_manager()->get_playback_start_ticks(), reached
    // through the narrow provider so this TU no longer depends on the ui layer.
    _playhead_tick = static_cast<MIDITick>(playback()->playback_start_tick());

    if (!committed_selection_boxes.empty() &&
        _controller->get_selection()->get_active_selected_tracks().empty()) {
        clear_committed_selection_boxes();
    }
}

std::pair<MIDITick, std::uint16_t> TrackEditing::screen_pos_to_midi_track_pos(Vector2<float> screen_pos) const {
    const auto& work_rect = context().rect;
    const auto& nav = track_navigation()->value;

    const float screen_x_norm = (screen_pos.x - work_rect.left) / work_rect.width;
    const float screen_y_norm = (screen_pos.y - work_rect.top) / work_rect.height;

    const auto screen_x_tick = saturating_cast<MIDITick>(
        screen_x_norm * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed);
    const auto screen_y_trck = saturating_cast<std::uint16_t>(
        screen_y_norm * nav.zoom_tracks_smoothed + nav.track_pos_smoothed);

    return {screen_x_tick, screen_y_trck};
}

void TrackEditing::on_mouse_down() {
    if (get_flag(TRACK_EDIT_MOUSE_OVER_UI)) {
        enable_flag(TRACK_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    const EditorTool editor_tool = _controller->get_editor_tool_settings()->curr_tool;

    {
        const auto midi_pos = get_mouse_midi_pos_snapped();
        change_track(midi_pos.second);

        // TODO: main window now controls playhead position on click
        /*if (playhead_) {
            playhead_->set_start(midi_pos.first);
        }*/
    }

    switch (editor_tool) {
    case EditorTool::Pencil:
        break;
    case EditorTool::Selector:
        select_mouse_down();
        break;
    case EditorTool::Eraser:
        eraser_mouse_down();
        break;
    }
}

void TrackEditing::on_right_mouse_down() {
    if (get_flag(TRACK_EDIT_MOUSE_OVER_UI)) {
        return;
    }

    right_clicked_track_ = get_mouse_track_pos();
}

void TrackEditing::on_mouse_move() {
    if (get_flag(TRACK_EDIT_MOUSE_DOWN_ON_UI)) {
        return;
    }
    if (get_flag(TRACK_EDIT_ANY_DIALOG_OPEN | TRACK_EDIT_MOUSE_OVER_UI)) {
        return;
    }

    const EditorTool editor_tool = _controller->get_editor_tool_settings()->curr_tool;
    switch (editor_tool) {
    case EditorTool::Pencil:
        break;
    case EditorTool::Selector:
        select_mouse_move();
        break;
    case EditorTool::Eraser:
        eraser_mouse_move();
        break;
    }
}

void TrackEditing::on_mouse_up() {
    if (get_flag(TRACK_EDIT_MOUSE_DOWN_ON_UI)) {
        disable_flag(TRACK_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (get_flag(TRACK_EDIT_MOUSE_OVER_UI | TRACK_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }
    
    const EditorTool editor_tool = _controller->get_editor_tool_settings()->curr_tool;
    switch (editor_tool) {
    case EditorTool::Pencil:
        break;
    case EditorTool::Selector:
        select_mouse_up();
        break;
    case EditorTool::Eraser:
        eraser_mouse_up();
        break;
    }
}

void TrackEditing::on_key_down(const KeyState& keys) {
    if (get_flag(TRACK_EDIT_ANY_DIALOG_OPEN | TRACK_EDIT_MOUSE_OVER_UI)) {
        return;
    }

    const std::uint16_t curr_track = _controller->get_active_track();

    if (keys.track_up && curr_track > 0) {
        change_track(static_cast<std::uint16_t>(curr_track - 1));
    }

    if (keys.track_down && curr_track < std::numeric_limits<std::uint16_t>::max()) {
        change_track(static_cast<std::uint16_t>(curr_track + 1));
    }

    if (keys.del) {
        delete_selection();
    }

    if (keys.copy) {
        Debugger::log("Copied");
        copy_notes();
    }

    if (keys.cut) {
        Debugger::log("Cut");
        cut_notes();
    }

    if (keys.paste) {
        SharedClipboard* clipboard = _controller->get_clipboard();
        const bool is_empty = clipboard ? clipboard->is_empty : true;

        if (!is_empty) {
            Debugger::log("Starting paste operation");
            paste_notes(curr_track);
            Debugger::log("Pasted");
        } else {
            Debugger::log("Nothing to paste");
        }
    }
}

void TrackEditing::select_mouse_down() {
    // refactor: the verb is latched here from the modifiers the widget published, so releasing
    // alt or shift before the mouse button cannot change what the gesture means.
    m_active_gesture = selection_gesture_of(get_flag(TRACK_EDIT_SHIFT_DOWN),
                                            get_flag(TRACK_EDIT_ALT_DOWN));

    const bool mouse_over_selection = is_mouse_over_select_area();

    if (!mouse_over_selection) {
        overlays::SelectionType selection_type = overlays::SelectionType::AddSelect;

        switch (m_active_gesture) {
        case SelectionGesture::Subtract:
            selection_type = overlays::SelectionType::RemoveSelect;
            break;
        case SelectionGesture::Add:
        case SelectionGesture::Replace:
            selection_type = overlays::SelectionType::AddSelect;
            break;
        default:
            break;
        }

        begin_live_selection_box(mouse_info_.mouse_midi_track_pos, selection_type);
    } else {
        selected_notes_to_ghost_notes();

        mouse_info_.last_mouse_click_pos = get_mouse_midi_pos_snapped();
    }

    set_flag(TRACK_EDIT_SELECTION_MOVE, mouse_over_selection);
}

void TrackEditing::select_mouse_move() {
    if (get_flag(TRACK_EDIT_SELECTION_MOVE)) {
        if (!is_moving_selection()) return;

        const auto [last_tick, last_track] = mouse_info_.last_mouse_click_pos;
        const auto [raw_tick, curr_track] = mouse_info_.mouse_midi_track_pos;

        const editor::timeline::Timeline* timeline = _controller->get_project_manager()->get_timeline();
        const auto curr_tick =
            static_cast<MIDITick>(timeline->snap_nearest(raw_tick));

        const SignedMIDITick tick_change =
            static_cast<SignedMIDITick>(curr_tick) - static_cast<SignedMIDITick>(last_tick);
        const auto track_change = static_cast<std::int16_t>(
            static_cast<SignedMIDITrk>(curr_track) - static_cast<SignedMIDITrk>(last_track));

        offset_render_ghost_notes({tick_change, track_change});
        offset_committed_selection_boxes(tick_change, track_change);
    } else {
        update_live_selection_box(mouse_info_.mouse_midi_track_pos);
    }
}

void TrackEditing::select_mouse_up() {
    SharedSelectedNotes* selection = _controller->get_selection();

    const SelectionGesture gesture = m_active_gesture;
    m_active_gesture = SelectionGesture::Replace;

    const bool was_moving = get_flag(TRACK_EDIT_SELECTION_MOVE);
    disable_flag(TRACK_EDIT_SELECTION_MOVE);

    if (was_moving) {
        offset_committed_selection_boxes(0, 0);

        const editor::timeline::Timeline* timeline = _controller->get_project_manager()->get_timeline();

        const auto [last_tick, last_track] = mouse_info_.last_mouse_click_pos;
        const auto [raw_tick, curr_track] = mouse_info_.mouse_midi_track_pos;

        const auto curr_tick =
            static_cast<MIDITick>(timeline->snap_nearest(raw_tick));

        const SignedMIDITick tick_change =
            static_cast<SignedMIDITick>(curr_tick) - static_cast<SignedMIDITick>(last_tick);
        const auto track_change = static_cast<std::int16_t>(
            static_cast<SignedMIDITrk>(curr_track) - static_cast<SignedMIDITrk>(last_track));

        GhostTrackNotes ghost_notes;
        {
            std::lock_guard lock(ghost_notes_->mutex);
            ghost_notes = std::exchange(ghost_notes_->value, {});
        }
        reset_ghost_note_offset();
        if (ghost_notes.empty()) {
            return;
        }

        // clamp the move so nothing leaves the project (a negative track index wraps to ~65535
        // as uint16 and used to make the track loop below create thousands of tracks)
        int min_src = 0xFFFF;
        int max_src = 0;
        MIDITick min_start = std::numeric_limits<MIDITick>::max();
        for (const auto& [trk, notes] : ghost_notes) {
            min_src = std::min<int>(min_src, trk);
            max_src = std::max<int>(max_src, trk);
            for (const Note& n : notes) min_start = std::min(min_start, n.get_start());
        }
        const int track_delta = std::clamp<int>(track_change, -min_src, 0xFFFF - max_src);
        const SignedMIDITick tick_delta =
            std::max<SignedMIDITick>(tick_change, -static_cast<SignedMIDITick>(min_start));
        if (tick_delta == 0 && track_delta == 0) {
            return;
        }

        // One NoteMutator per track: a track can be a source and a destination at once (move
        // tracks 1+2 down by one), and Transaction::notes() may only be called once per track.
        struct TrackWork {
            std::vector<history::NoteId> deletes;  // selected notes currently in this track
            std::vector<Note> adds;                // moved-in notes (cross-track moves only)
        };
        std::map<std::uint16_t, TrackWork> work;
        for (auto& [src, notes] : ghost_notes) {
            const std::vector<std::size_t>* ids = selection->get_selected_ids_in_track(src);
            if (!ids || ids->size() != notes.size()) {
                util::Debugger::log_error("Track view move aborted: selection changed mid-drag");
                return;
            }
            auto& w = work[src];
            w.deletes = *ids;
            std::sort(w.deletes.begin(), w.deletes.end());

            if (track_delta != 0) {
                auto& adds = work[static_cast<std::uint16_t>(src + track_delta)].adds;
                for (Note& n : notes) {
                    n.set_start(static_cast<MIDITick>(static_cast<SignedMIDITick>(n.get_start()) +
                                                      tick_delta));
                    adds.push_back(std::move(n));
                }
            }
        }

        auto tx = _controller->begin_action("Move notes (track view)");

        const int tracks_needed = max_src + track_delta + 1;
        if (static_cast<int>(get_used_track_count()) < tracks_needed) {
            auto tm = tx.tracks();
            for (int n = get_used_track_count(); n < tracks_needed; ++n) {
                tm.add_track(MIDITrack::new_empty());
            }
            tm.finalize_tracks();
        }

        std::vector<std::pair<std::uint16_t, history::NoteMutator>> mutators;
        for (auto& [track, w] : work) {
            auto m = tx.notes(track, w.deletes);
            if (track_delta == 0) {
                m.move_notes(w.deletes, tick_delta, 0);
            } else {
                m.delete_notes(w.deletes);
                for (const Note& n : w.adds) m.add_note(n);
            }
            mutators.emplace_back(track, std::move(m));
        }

        // selection stage: only after ALL notes() calls. Every selected note of an affected
        // track was moved, so the new selection is exactly what each mutator inserted.
        auto sel = tx.selection();
        for (auto& [track, m] : mutators) {
            const std::vector<history::NoteId>& moved = m.inserted_ids();
            if (moved.empty()) {
                sel.clear_track(track);
            } else {
                sel.set(track, moved);
            }
        }

        if (tx.commit()) {
            // selection box follows the notes
            translate_committed_selection_boxes(tick_delta, track_delta);
        } else {
            util::Debugger::log_error("Failed to commit track view note move transaction");
        }
    } else if (gesture == SelectionGesture::Subtract) {
        // refactor: the subtractive gesture removes the notes inside the swept region, and only
        // on release, so one gesture produces exactly one undo step and no rectangle is drawn.
        const auto region = get_selection_range();          // must read the live box before ending it
        const auto notes_to_remove = get_note_ids_in_region(region);

        end_live_selection_box();
        if (notes_to_remove.empty()) {
            return;
        }

        history::Transaction tx = _controller->begin_action("Deselect notes (track view)");
        history::SelectionMutator mutator = tx.selection();
        for (const auto& [track, note_ids] : notes_to_remove) {
            mutator.remove(track, note_ids);
        }
        tx.commit();
    } else {
        const bool extend_selection = gesture == SelectionGesture::Add;
        const auto region = get_selection_range();          // must read the live box before ending it
        auto notes_to_select = get_note_ids_in_region(region);
        const bool has_new_selection = !notes_to_select.empty();

        end_live_selection_box();

        history::Transaction tx = _controller->begin_action(
            has_new_selection ? "Select notes (track view)" : "Deselect notes (track view)");
        history::SelectionMutator mutator = tx.selection();
        if (!extend_selection) mutator.clear_all();
        if (has_new_selection) {
            for (const auto& [track, note_ids] : notes_to_select) mutator.add(track, note_ids);
        }
        tx.commit();

        if (!extend_selection) clear_committed_selection_boxes();
        if (has_new_selection) add_committed_selection_box(region);
    }
}

void TrackEditing::eraser_mouse_down() {
    enable_flag(TRACK_EDIT_ERASING);
    begin_live_selection_box(mouse_info_.mouse_midi_track_pos, overlays::SelectionType::Delete);
    deselect_all();
}

void TrackEditing::eraser_mouse_move() { update_live_selection_box(mouse_info_.mouse_midi_track_pos); }

void TrackEditing::eraser_mouse_up() {
    disable_flag(TRACK_EDIT_ERASING);
    const auto region = get_selection_range();
    end_live_selection_box();

    const auto selected_ids_with_track = get_note_ids_in_region(region);
    if (selected_ids_with_track.empty()) {
        return;
    }

    if (!delete_notes_in_tracks(_controller, "Delete selection (track view)",
                                selected_ids_with_track)) {
        util::Debugger::log_error("Failed to commit multi-track delete transaction");
    }
}

void TrackEditing::begin_live_selection_box(std::pair<MIDITick, std::uint16_t> start,
    overlays::SelectionType type) {
    end_live_selection_box();

    const editor::timeline::Timeline* timeline = _controller->get_project_manager()->get_timeline();
    const auto tick = timeline->snap_nearest(start.first);
    live_box_anchor_row_ = start.second;

    live_selection_box = overlay_drawer()->emplace_overlay<overlays::SelectionBox>(OVERLAY_LAYER_LIVE);
    auto& info = live_selection_box->info;
    info.type = type;
    info.start = Vector2<int64_t>(tick, live_box_anchor_row_);
    info.end = Vector2<int64_t>(tick, live_box_anchor_row_ + 1);   // the one row under the mouse
}

void TrackEditing::end_live_selection_box() {
    overlay_drawer()->clear_layer(OVERLAY_LAYER_LIVE);
    live_selection_box = nullptr;
}

void TrackEditing::add_committed_selection_box(
    const std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t>& region) {
    const auto [min_tick, max_tick, min_track, max_track] = region;
    auto* box = overlay_drawer()->emplace_overlay<overlays::SelectionBox>(OVERLAY_LAYER_COMMITTED);
    box->info.type = overlays::SelectionType::Default;
    box->info.start = Vector2<int64_t>(static_cast<int64_t>(min_tick), static_cast<int64_t>(min_track));
    box->info.end = Vector2<int64_t>(static_cast<int64_t>(max_tick), static_cast<int64_t>(max_track));
    committed_selection_boxes.push_back(box);
}

void TrackEditing::clear_committed_selection_boxes() {
    overlay_drawer()->clear_layer(OVERLAY_LAYER_COMMITTED);
    committed_selection_boxes.clear();
}

void TrackEditing::offset_committed_selection_boxes(SignedMIDITick tick, std::int64_t track) {
    for (auto* box : committed_selection_boxes) box->info.offset = Vector2<int64_t>(tick, track);
}

void TrackEditing::translate_committed_selection_boxes(SignedMIDITick tick, std::int64_t track) {
    for (auto* box : committed_selection_boxes) {
        auto& i = box->info;
        i.start = Vector2<int64_t>(i.start.x + tick, i.start.y + track);
        i.end = Vector2<int64_t>(i.end.x + tick, i.end.y + track);
        i.offset = Vector2<int64_t>(0, 0);
    }
}

void TrackEditing::update_live_selection_box(std::pair<MIDITick, std::uint16_t> new_pos) {
    if (!live_selection_box) return;
    auto& info = live_selection_box->info;

    const editor::timeline::Timeline* timeline = _controller->get_project_manager()->get_timeline();
    const auto tick = static_cast<int64_t>(timeline->snap_nearest(new_pos.first));
    const std::int64_t row = new_pos.second;

    // y values are row EDGES: the box covers the row the drag began in AND the row the mouse is in
    const bool down = row >= live_box_anchor_row_;
    info.start = Vector2<int64_t>(info.start.x, down ? live_box_anchor_row_ : live_box_anchor_row_ + 1);
    info.end = Vector2<int64_t>(tick, down ? row + 1 : row);
}

std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t> TrackEditing::get_selection_range() const {
    if (!live_selection_box) return { 0, 0, 0, 0 };

    const auto [lo, hi] = live_selection_box->bounds();
    return { static_cast<MIDITick>(std::max(lo.x, int64_t{0})),
             static_cast<MIDITick>(std::max(hi.x, int64_t{0})),
             static_cast<std::uint16_t>(std::clamp(lo.y, int64_t{0}, int64_t{0xFFFF})),
             static_cast<std::uint16_t>(std::clamp(hi.y, int64_t{0}, int64_t{0xFFFF})) };
}

std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t> TrackEditing::get_range_between(
    std::pair<MIDITick, std::uint16_t> start, std::pair<MIDITick, std::uint16_t> end) const {
    const editor::timeline::Timeline* timeline = _controller->get_project_manager()->get_timeline();

    const auto start_tick = static_cast<std::int64_t>(timeline->snap_nearest(start.first));
    const auto end_tick = static_cast<std::int64_t>(timeline->snap_nearest(end.first));

    // y values are row EDGES, matching update_live_selection_box: the region covers the row the
    // drag began in AND the row the cursor is in, so a single-row sweep still spans one row.
    const std::int64_t start_row = static_cast<std::int64_t>(start.second);
    const std::int64_t end_row = static_cast<std::int64_t>(end.second);
    const std::int64_t lo_row = std::min(start_row, end_row);
    const std::int64_t hi_row = std::max(start_row, end_row) + 1;

    return { static_cast<MIDITick>(std::max(std::min(start_tick, end_tick), std::int64_t{0})),
             static_cast<MIDITick>(std::max(std::max(start_tick, end_tick), std::int64_t{0})),
             static_cast<std::uint16_t>(std::clamp(lo_row, std::int64_t{0}, std::int64_t{0xFFFF})),
             static_cast<std::uint16_t>(std::clamp(hi_row, std::int64_t{0}, std::int64_t{0xFFFF})) };
}

std::pair<Vector2<float>, Vector2<float>> TrackEditing::get_selection_range_ui() const {

    const auto [min_tick, max_tick, min_track, max_track] = get_selection_range();

    const auto tl = midi_track_pos_to_screen_pos({min_tick, min_track});
    const auto br = midi_track_pos_to_screen_pos({max_tick, max_track});

    return {tl, br};
}

bool TrackEditing::is_mouse_over_select_area() const {
    const auto mx = static_cast<int64_t>(mouse_info_.mouse_midi_track_pos.first);
    const auto my = static_cast<int64_t>(mouse_info_.mouse_midi_track_pos.second);
    for (const auto* box : committed_selection_boxes) {
        const auto [lo, hi] = box->bounds();
        if (mx > lo.x && mx < hi.x && my >= lo.y && my < hi.y) return true;
    }
    return false;
}

std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>>
TrackEditing::get_note_ids_in_region(
    const std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t>& region) const {
    const auto [min_tick, max_tick, min_track, max_track] = region;
    if (min_tick == max_tick) {
        return {};
    }

    std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> all_ids;
    all_ids.reserve(static_cast<std::size_t>(max_track - min_track + 1));

    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    if (min_track >= tracks->size()) {
        return {};
    }

    for (std::uint16_t trk = min_track; trk < max_track; ++trk) {
        if (trk >= tracks->size()) {
            break;
        }

        const std::vector<Note>& notes = tracks->at(trk).get_notes();
        if (notes.empty()) {
            continue;
        }

        std::vector<std::size_t> ids = get_notes_in_range(notes, min_tick, max_tick, 0, 128, true);
        if (!ids.empty()) {
            all_ids.emplace_back(trk, std::move(ids));
        }
    }

    return all_ids;
}

void TrackEditing::deselect_all() {
    SharedSelectedNotes* selection = _controller->get_selection();
    clear_committed_selection_boxes();
    selection->clear_selected();
}

void TrackEditing::delete_selection() {
    SharedSelectedNotes* selected = _controller->get_selection();

    // read, don't take: the transaction snapshots the selection so undo can restore it
    TrackIds targets;
    const std::uint16_t track_count = get_used_track_count();
    for (std::uint16_t trk = 0; trk < track_count; ++trk) {
        const std::vector<std::size_t>* ids = selected->get_selected_ids_in_track(trk);
        if (ids && !ids->empty()) targets.emplace_back(trk, *ids);
    }
    if (targets.empty()) {
        deselect_all();
        return;
    }

    if (!delete_notes_in_tracks(_controller, "Delete selection (track view)", targets)) {
        util::Debugger::log_error("Failed to commit multi-track delete transaction");
    }
}

void TrackEditing::offset_render_ghost_notes(SignedMIDITrkVec offset) const {
    std::unique_lock lock(ghost_notes_render_offset_->mutex);
    ghost_notes_render_offset_->value = offset;
}

void TrackEditing::reset_ghost_note_offset() const {
    std::unique_lock lock(ghost_notes_render_offset_->mutex);
    ghost_notes_render_offset_->value = {0, 0};
}

void TrackEditing::move_ghost_notes(SignedMIDITick tick_change, std::int16_t track_change) {
    std::lock_guard lock(ghost_notes_->mutex);
    for (auto& gn_track : ghost_notes_->value) {
        gn_track.first = static_cast<std::uint16_t>(static_cast<std::int16_t>(gn_track.first) +
                                                    track_change);
        for (Note& gn_note : gn_track.second) {
            gn_note.set_start(static_cast<MIDITick>(
                static_cast<SignedMIDITick>(gn_note.get_start()) + tick_change));
        }
    }
}

void TrackEditing::selected_notes_to_ghost_notes() {
    SharedSelectedNotes* selection = _controller->get_selection();

    // Copies only. The project and the selection are NOT touched while dragging; the move is
    // one transaction on mouse-up. (It used to pull the notes out of the project here, which no
    // transaction recorded, so undo could never bring them back.)
    GhostTrackNotes ghost_notes;
    const std::uint16_t track_count = get_used_track_count();
    for (std::uint16_t trk = 0; trk < track_count; ++trk) {
        const std::vector<std::size_t>* ids = selection->get_selected_ids_in_track(trk);
        if (!ids || ids->empty()) {
            continue;
        }
        ghost_notes.emplace_back(trk, clone_notes(trk, *ids));
    }

    std::lock_guard lock(ghost_notes_->mutex);
    ghost_notes_->value = std::move(ghost_notes);
}

std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> TrackEditing::apply_ghost_notes() {
    GhostTrackNotes ghost_notes;
    {
        std::lock_guard lock(ghost_notes_->mutex);
        ghost_notes = std::exchange(ghost_notes_->value, {});
    }

    std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> track_ids;
    track_ids.reserve(ghost_notes.size());

    for (auto& [track, notes] : ghost_notes) {
        if (auto existing = take_notes_in_track(track)) {
            auto [merged, ids] = merge_notes_and_return_ids(std::move(*existing), std::move(notes));
            set_notes_in_track(track, std::move(merged));

            track_ids.emplace_back(track, std::move(ids));
        } else {
            while (!track_exists(track)) {
                append_empty_track();
            }

            std::vector<std::size_t> ids(notes.size());
            for (std::size_t i = 0; i < ids.size(); ++i) {
                ids[i] = i;
            }
            set_notes_in_track(track, std::move(notes));
            track_ids.emplace_back(track, std::move(ids));
        }
    }

    return track_ids;
}

bool TrackEditing::track_exists(std::uint16_t track) const {
    return track < get_used_track_count();
}

std::optional<std::vector<Note>> TrackEditing::take_notes_in_track(std::uint16_t track) {
    if (track >= get_used_track_count()) {
        return std::nullopt;
    }

    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    return std::exchange(tracks->at(track).get_notes_mut(), {});
}

std::optional<std::pair<std::vector<Note>, std::vector<Note>>>
TrackEditing::take_some_notes_in_track(std::uint16_t track, const std::vector<std::size_t>& ids) {
    if (track >= get_used_track_count()) {
        return std::nullopt;
    }

    auto taken_notes = take_notes_in_track(track);
    if (!taken_notes) {
        return std::nullopt;
    }
    return extract(std::move(*taken_notes), ids);
}

void TrackEditing::set_notes_in_track(std::uint16_t track, std::vector<Note> notes) {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    if (track >= tracks->size()) {
        return;
    }
    tracks->at(track).get_notes_mut() = std::move(notes);
}

void TrackEditing::insert_notes_and_ch_evs(std::uint16_t track, std::vector<Note> notes,
                                           std::vector<ChannelEvent> ch_evs) {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    tracks->insert(
        tracks->begin() + static_cast<std::ptrdiff_t>(track),
        MIDITrack(std::move(notes), std::move(ch_evs), {}));
}

void TrackEditing::insert_track_at(std::uint16_t track_idx, MIDITrack track) {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    tracks->insert(
        tracks->begin() + static_cast<std::ptrdiff_t>(track_idx), std::move(track));
}

void TrackEditing::insert_track(std::uint16_t track) {
    const std::uint16_t track_count = get_used_track_count();
    if (track >= track_count) {
        return;
    }

    auto tx = _controller->begin_action("Add track");
    auto tm = tx.tracks();
    tm.insert_track(track, MIDITrack::new_empty());
    tm.finalize_tracks();

    if (!tx.commit()) {
        util::Debugger::log_error("Failed to commit insert track transaction");
    }
}

void TrackEditing::remove_track(std::uint16_t track) {
    std::uint16_t track_count = get_used_track_count();
    if (track >= track_count || track_count <= 1) {
        return; // project always keeps at least one track
    }

    auto tx = _controller->begin_action("Remove track");
    auto tm = tx.tracks();
    tm.remove_track(track);
    tm.finalize_tracks();

    if (!tx.commit()) {
        util::Debugger::log_error("Failed to commit remove track transaction");
        return;
    }

    const MIDITrk active_track = _controller->get_active_track();
    if (active_track > track) {
        change_track(static_cast<std::uint16_t>(active_track - 1));
    }
}

MIDITrack TrackEditing::remove_track_at(std::uint16_t track) {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    // fixed rust bug: vec remove panicked on an index past the end
    if (track >= tracks->size()) {
        return MIDITrack{};
    }

    MIDITrack removed = std::move(tracks->at(track));
    tracks->erase(tracks->begin() + static_cast<std::ptrdiff_t>(track));
    return removed;
}

void TrackEditing::decompose_track(std::uint16_t track) {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    if (track >= tracks->size()) {
        return;
    }

    // 1. Inspect and group the notes and channel events by channel (0 to 15)
    const MIDITrack& source_track_ref = tracks->at(track);
    const std::vector<Note>& notes = source_track_ref.get_notes();
    const std::vector<ChannelEvent>& ch_evs = source_track_ref.get_channel_evs();

    if (notes.empty() && ch_evs.empty()) {
        return;
    }

    std::vector<std::pair<std::vector<Note>, std::vector<ChannelEvent>>> decomposed(16);
    for (const Note& note : notes) {
        decomposed[note.get_channel() & 0x0F].first.push_back(note);
    }
    for (const ChannelEvent& ev : ch_evs) {
        decomposed[ev.channel & 0x0F].second.push_back(ev);
    }

    // clear out unused channels
    std::erase_if(decomposed, [](const auto& t) { return t.first.empty() && t.second.empty(); });
    if (decomposed.empty()) {
        return;
    }

    const std::size_t decomposed_count = decomposed.size();
    if (decomposed_count <= 1) {
        return; // only one channel present, no decomposition needed
    }

    auto tx = _controller->begin_action("Decompose track");
    auto track_mutator = tx.tracks();

    // channel 0 stays on the original track index; channels 1 to N-1 get inserted right after
    for (std::size_t i = 1; i < decomposed_count; ++i) {
        track_mutator.insert_track(static_cast<std::uint16_t>(track + i), MIDITrack::new_empty());
    }
    track_mutator.finalize_tracks();

    for (std::size_t i = 0; i < decomposed_count; ++i) {
        const std::uint16_t target_track_idx = static_cast<std::uint16_t>(track + i);
        auto& [channel_notes, channel_evs] = decomposed[i];

        // Open a note mutator for this target track
        auto notes_mutator = tx.notes(target_track_idx, {});
        for (const auto& note : channel_notes) {
            notes_mutator.add_note(note);
        }
        
        // TODO: channel events mutator too
    }

    if (!tx.commit()) {
        util::Debugger::log_error("Failed to commit decompose track transaction");
    }
}

void TrackEditing::append_empty_track() {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    tracks->push_back(MIDITrack::new_empty());
}

void TrackEditing::pop_track() {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    if (!tracks->empty())
        tracks->pop_back();
}

void TrackEditing::remove_right_clicked_track() { remove_track(right_clicked_track_); }

std::uint16_t TrackEditing::get_used_track_count() const {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
    return static_cast<std::uint16_t>(tracks->size());
}

void TrackEditing::change_track(std::uint16_t new_track) {
    _controller->set_active_track(new_track);
}

void TrackEditing::swap_tracks_and_register(std::uint16_t track_1, std::uint16_t track_2,
                                            bool allow_register) {
    Debugger::log_error("DEPRECATED: just use swap_tracks()");
}

void TrackEditing::swap_tracks(std::uint16_t track_1, std::uint16_t track_2) {
    auto tx = _controller->begin_action("Swap tracks");
    auto tm = tx.tracks();
    tm.swap_tracks(track_1, track_2);
    tm.finalize_tracks();

    if (!tx.commit()) {
        util::Debugger::log_error("Failed to commit swap tracks transaction");
        return;
    }

    const std::uint16_t curr_track = _controller->get_active_track();
    if (curr_track == track_1) {
        change_track(track_2);
    }
    else if (curr_track == track_2) {
        change_track(track_1);
    }
}

std::vector<Note> TrackEditing::clone_notes(std::uint16_t track,
                                            const std::vector<std::size_t>& ids) const {
    std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

    std::vector<Note> copied;
    if (track >= tracks->size()) {
        return copied;
    }

    const std::vector<Note>& notes = tracks->at(track).get_notes();
    copied.reserve(ids.size());
    for (const std::size_t id : ids) {
        if (id < notes.size()) {
            copied.push_back(notes[id]);
        }
    }

    return copied;
}

// bugfix: previously computed the start tick against an EMPTY clipboard
// this caused everything to get pasted 1 tick before the playhead, so we split it into two functions
void TrackEditing::prepare_clipboard() {
    _controller->get_clipboard()->clear_clipboard();
}

void TrackEditing::finish_clipboard() {
    SharedClipboard* clipboard = _controller->get_clipboard();
    const MIDITick start = clipboard->get_clipboard_start_tick();
    clipboard->offset_from_playhead =
        static_cast<SignedMIDITick>(start) - static_cast<SignedMIDITick>(_playhead_tick);
}

void TrackEditing::with_clipboard(std::function<void(SharedClipboard*)> func) {
    SharedClipboard* clipboard = _controller->get_clipboard();
    {
        prepare_clipboard();
        func(clipboard);
        finish_clipboard();
    }
}

void TrackEditing::copy_notes() {
    SharedSelectedNotes* selection = _controller->get_selection();

    const std::vector<std::uint16_t> active_tracks =
        selection->get_active_selected_tracks();

    if (active_tracks.empty()) {
        return;
    }

    with_clipboard([&](SharedClipboard* clipboard) {
        for (const std::uint16_t track : active_tracks) {
            const std::vector<std::size_t>* selected =
                selection->get_selected_ids_in_track(track);
            if (!selected) {
                continue;
            }

            std::vector<Note> copied_notes = clone_notes(track, *selected);
            clipboard->move_notes_to_clipboard(std::move(copied_notes), track, false);
        }
    });    
}

void TrackEditing::cut_notes() {
    SharedSelectedNotes* selection = _controller->get_selection();
    const std::vector<std::uint16_t> active_tracks = selection->get_active_selected_tracks();

    if (active_tracks.empty()) {
        return;
    }

    // copy to clipboard first
    with_clipboard([&](SharedClipboard* clipboard) {
        for (const std::uint16_t track : active_tracks) {
            const std::vector<std::size_t>* selected = selection->get_selected_ids_in_track(track);
            if (!selected || selected->empty()) continue;

            std::vector<Note> copied_notes = clone_notes(track, *selected);
            clipboard->move_notes_to_clipboard(std::move(copied_notes), track, false);
        }
    });

    TrackIds targets;
    for (const std::uint16_t track : active_tracks) {
        const std::vector<std::size_t>* ids = selection->get_selected_ids_in_track(track);
        if (ids && !ids->empty()) targets.emplace_back(track, *ids);
    }

    if (!delete_notes_in_tracks(_controller, "Cut notes (track view)", targets)) {
        util::Debugger::log_error("Failed to commit cut notes transaction");
    }
}

void TrackEditing::paste_notes(std::uint16_t base_track) {
    SharedClipboard* clipboard = _controller->get_clipboard();
    auto copied_notes = clipboard->get_notes_from_clipboard();
    if (copied_notes.empty()) {
        return;
    }
    std::stable_sort(copied_notes.begin(), copied_notes.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    const std::uint16_t first_track = copied_notes[0].first;
    const MIDITick clip_start = clipboard->get_clipboard_start_tick();

    auto tx = _controller->begin_action("Paste notes (track view)");
    auto track_mutator = tx.tracks();

    // 1. Ensure required destination tracks exist first (Tracks stage)[cite: 7]
    std::uint16_t current_max_tracks = get_used_track_count();
    for (auto& [src_track, notes_vec] : copied_notes) {
        const auto rel = static_cast<std::uint16_t>(src_track - first_track);
        const auto dest_track = static_cast<std::uint16_t>(base_track + rel);

        while (dest_track >= current_max_tracks) {
            track_mutator.add_track(MIDITrack::new_empty());
            current_max_tracks += 1;
        }
    }
    track_mutator.finalize_tracks(); // Finalize track edits before touching notes[cite: 7]

    // 2. Add notes to destination tracks (Notes stage): one mutator per destination track
    struct Pasted {
        std::uint16_t track;
        history::NoteMutator mutator;
        std::vector<history::AddedNote> tickets;
    };
    std::vector<Pasted> pasted;
    for (auto& [src_track, notes_vec] : copied_notes) {
        const auto rel = static_cast<std::uint16_t>(src_track - first_track);
        const auto dest_track = static_cast<std::uint16_t>(base_track + rel);

        for (Note& note : notes_vec) {
            const auto shifted = static_cast<SignedMIDITick>(note.get_start()) +
                                 static_cast<SignedMIDITick>(_playhead_tick) -
                                 static_cast<SignedMIDITick>(clip_start);
            note.set_start(static_cast<MIDITick>(std::max<SignedMIDITick>(shifted, 0)));
        }

        Pasted p{dest_track, tx.notes(dest_track, {}), {}};
        p.tickets.reserve(notes_vec.size());
        for (const auto& note : notes_vec) {
            p.tickets.push_back(p.mutator.add_note(note));
        }
        pasted.push_back(std::move(p));
    }

    // 3. Selection stage: tx.selection() flushes the mutators, which is what makes final_id()
    // valid, so it must come after every notes() call and before any final_id().
    auto sel = tx.selection();
    sel.clear_all();
    for (auto& p : pasted) {
        std::vector<std::size_t> new_ids;
        new_ids.reserve(p.tickets.size());
        for (const auto& ticket : p.tickets) {
            new_ids.push_back(p.mutator.final_id(ticket));
        }
        std::sort(new_ids.begin(), new_ids.end());
        sel.set(p.track, new_ids);
    }

    if (!tx.commit()) {
        util::Debugger::log_error("Failed to commit paste notes transaction");
    }
}

void TrackEditing::apply_action(/*EditorAction& action*/) {
    util::Debugger::log_error("DEPRECATED: use new transaction api!");
}

std::pair<MIDITick, std::uint16_t> TrackEditing::get_mouse_midi_pos_snapped() const {
    const editor::timeline::Timeline* timeline = _controller->get_project_manager()->get_timeline();
    const auto snapped_tick = timeline->snap_nearest(mouse_info_.mouse_midi_track_pos.first);
    return {snapped_tick, mouse_info_.mouse_midi_track_pos.second};
}

#pragma region overlays

Vector2<float> TrackEditing::midi_track_pos_to_screen_pos(std::pair<MIDITick, std::uint16_t> p) const {
    return midi_track_pos_to_screen_pos_f(static_cast<float>(p.first), static_cast<float>(p.second));
}

Vector2<float> TrackEditing::midi_track_pos_to_screen_pos_f(float tick, float track) const {
    const TrackViewNavigation& nav = track_navigation()->value;

    const TickView tick_view = central_tick_view();

    util::math::Vector2<float> ui_pos{};
    ui_pos.x = tick_view.zoom == 0.0f ? 0.0f : (tick - tick_view.pos) / tick_view.zoom;
    ui_pos.y = (track - nav.track_pos_smoothed) / nav.zoom_tracks_smoothed;

    const editor::ViewRect& edit_region = context().rect;
    ui_pos.x = ui_pos.x * edit_region.width + edit_region.left;
    ui_pos.y = ui_pos.y * edit_region.height + edit_region.top;

    return ui_pos;
}

overlays::TransformFn TrackEditing::make_overlay_transform() const {
    return [this](const overlays::OverlayRect& r) -> overlays::ScreenRect {
        const auto a = midi_track_pos_to_screen_pos_f(static_cast<float>(r.first.x), static_cast<float>(r.first.y));
        const auto b = midi_track_pos_to_screen_pos_f(static_cast<float>(r.second.x), static_cast<float>(r.second.y));

        // keep overlays inside the note area
        const auto& rc = context().rect;
        const float min_x = rc.left;
        const float max_x = rc.left + rc.width;
        const float min_y = rc.top;
        const float max_y = rc.top + rc.height;

        return { Vector2<float>(std::clamp(a.x, min_x, max_x), std::clamp(a.y, min_y, max_y)),
                 Vector2<float>(std::clamp(b.x, min_x, max_x), std::clamp(b.y, min_y, max_y)) };
    };
}

#pragma endregion

#pragma region snapping

MIDITick TrackEditing::get_mouse_tick_pos_snapped() const {
    editor::Timeline* timeline = _controller->get_project_manager()->get_timeline();
    return timeline->snap_nearest(get_mouse_tick_pos());
}

}