#include "editor/editing/track_editing.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "util/debugger.h"
#include "util/numeric.h"
#include "editor/editor_controller.h"
#include "app/main_window.h"

namespace andromeda::editor {

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
    const Vector2<float>& mouse_pos_norm = *_app->get_mouse_pos();
    mouse_info_.mouse_pos = { mouse_pos_norm.x, mouse_pos_norm.y };
    mouse_info_.mouse_midi_track_pos = screen_pos_to_midi_track_pos(mouse_pos_norm);
}

std::pair<MIDITick, std::uint16_t> TrackEditing::screen_pos_to_midi_track_pos(Vector2<float> screen_pos) const {
    const auto& work_rect = context().rect;
    const auto& nav = _app->track_nav->value;

    const float screen_x_norm = (screen_pos.x - work_rect.left) / work_rect.width;
    const float screen_y_norm = (screen_pos.y - work_rect.top) / work_rect.height;

    const auto screen_x_tick = saturating_cast<MIDITick>(
        screen_x_norm * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed);
    const auto screen_y_trck = saturating_cast<std::uint16_t>(
        screen_y_norm * nav.zoom_tracks_smoothed + nav.track_pos_smoothed);

    return {screen_x_tick, screen_y_trck};
}

Vector2<float> TrackEditing::midi_track_pos_to_screen_pos(std::pair<MIDITick, std::uint16_t> midi_track_pos) const {
    const auto& work_rect = context().rect;
    const auto& nav = _app->track_nav->value;

    const float screen_x_norm =
        (static_cast<float>(midi_track_pos.first) - nav.tick_pos_smoothed) /
        nav.zoom_ticks_smoothed;
    const float screen_y_norm =
        (static_cast<float>(midi_track_pos.second) - nav.track_pos_smoothed) /
        nav.zoom_tracks_smoothed;

    return { work_rect.left + screen_x_norm * work_rect.width, work_rect.top + screen_y_norm * work_rect.height};
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
    const bool shift_down = get_flag(TRACK_EDIT_SHIFT_DOWN);
    const bool mouse_over_selection = is_mouse_over_select_area();

    if (!mouse_over_selection) {
        if (!shift_down) {
            deselect_all();
        }
        init_selection_box(mouse_info_.mouse_midi_track_pos);
    } else {
        selected_notes_to_ghost_notes();

        mouse_info_.last_mouse_click_pos = get_mouse_midi_pos_snapped();
    }

    set_flag(TRACK_EDIT_SELECTION_MOVE, mouse_over_selection);
}

void TrackEditing::select_mouse_move() {
    if (get_flag(TRACK_EDIT_SELECTION_MOVE)) {
        const auto [last_tick, last_track] = mouse_info_.last_mouse_click_pos;
        const auto [raw_tick, curr_track] = mouse_info_.mouse_midi_track_pos;

        const auto curr_tick =
            static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(raw_tick)));

        const SignedMIDITick tick_change =
            static_cast<SignedMIDITick>(curr_tick) - static_cast<SignedMIDITick>(last_tick);
        const auto track_change = static_cast<std::int16_t>(
            static_cast<SignedMIDITrk>(curr_track) - static_cast<SignedMIDITrk>(last_track));

        offset_render_ghost_notes({tick_change, track_change});
    } else {
        update_selection_box(mouse_info_.mouse_midi_track_pos);
    }
}

void TrackEditing::select_mouse_up() {
    SharedSelectedNotes* selection = _controller->get_selection();
    EditorActions* actions = _controller->get_actions();

    if (get_flag(TRACK_EDIT_SELECTION_MOVE)) {
        
        const auto [last_tick, last_track] = mouse_info_.last_mouse_click_pos;
        const auto [raw_tick, curr_track] = mouse_info_.mouse_midi_track_pos;

        const auto curr_tick =
            static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(raw_tick)));

        const SignedMIDITick tick_change =
            static_cast<SignedMIDITick>(curr_tick) - static_cast<SignedMIDITick>(last_tick);
        const auto track_change = static_cast<std::int16_t>(
            static_cast<SignedMIDITrk>(curr_track) - static_cast<SignedMIDITrk>(last_track));

        move_ghost_notes(tick_change, track_change);

        auto new_ids = apply_ghost_notes();
        reset_ghost_note_offset();

        {
            selection->clear_selected();
            for (const auto& [track, ids] : new_ids) {
                selection->set_selected_in_track(ids, track);
            }
        }

        selection_range = {
            static_cast<MIDITick>(static_cast<SignedMIDITick>(std::get<0>(selection_range)) +
                                  tick_change),
            static_cast<MIDITick>(static_cast<SignedMIDITick>(std::get<1>(selection_range)) +
                                  tick_change),
            static_cast<MIDITrk>(static_cast<SignedMIDITrk>(std::get<2>(selection_range)) +
                                 track_change),
            static_cast<MIDITrk>(static_cast<SignedMIDITrk>(std::get<3>(selection_range)) +
                                 track_change)};

        if (actions) {
            actions->register_action(
                NotesMoveMultiTrack{std::move(new_ids), {tick_change, track_change}});
        }
    } else {
        const bool shift_down = get_flag(TRACK_EDIT_SHIFT_DOWN);
        if (!shift_down) {
            deselect_all();
        }

        draw_select_box_ = false;

        const auto region = get_selection_range();
        auto selected_ids_with_track = get_note_ids_in_region(region);

        if (!selected_ids_with_track.empty()) {
            has_selection = true;

            std::size_t num_selected = 0;
            for (auto& [track, ids] : selected_ids_with_track) {
                num_selected += ids.size();
                if (shift_down) {
                    selection->add_selected_to_track(ids, track);
                } else {
                    selection->set_selected_in_track(std::move(ids), track);
                }
            }

            Debugger::log(std::format("Selected {} notes in track view.", num_selected));
        } else {
            Debugger::log("Selected no notes in track view.");
        }
    }
}

void TrackEditing::eraser_mouse_down() {
    enable_flag(TRACK_EDIT_ERASING);
    init_selection_box(mouse_info_.mouse_midi_track_pos);
    deselect_all();
}

void TrackEditing::eraser_mouse_move() { update_selection_box(mouse_info_.mouse_midi_track_pos); }

void TrackEditing::eraser_mouse_up() {
    disable_flag(TRACK_EDIT_ERASING);
    draw_select_box_ = false;

    const auto region = get_selection_range();
    const auto selected_ids_with_track = get_note_ids_in_region(region);
    if (selected_ids_with_track.empty()) {
        return;
    }

    std::vector<std::vector<std::size_t>> deleted_ids;
    std::vector<std::vector<Note>> deleted_notes;
    std::vector<std::uint16_t> affected_tracks;

    for (const auto& [track, ids] : selected_ids_with_track) {
        auto taken = take_some_notes_in_track(track, ids);
        if (!taken) {
            continue;
        }
        auto& [removed_notes, retained_notes] = *taken;
        set_notes_in_track(track, std::move(retained_notes));

        deleted_ids.push_back(ids);
        deleted_notes.push_back(std::move(removed_notes));
        affected_tracks.push_back(track);
    }

    EditorActions* actions = _controller->get_actions();
    if (actions) {
        actions->register_action(DeleteNotesMultiTrack{
            std::move(deleted_ids), std::move(deleted_notes), std::move(affected_tracks)});
    }
}

void TrackEditing::init_selection_box(std::pair<MIDITick, std::uint16_t> start_pos) {
    const auto snapped_tick =
        static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(start_pos.first)));
    selection_range = {snapped_tick, snapped_tick, start_pos.second, start_pos.second};
    draw_select_box_ = true;
}

void TrackEditing::update_selection_box(std::pair<MIDITick, std::uint16_t> new_pos) {
    std::get<1>(selection_range) =
        static_cast<MIDITick>(snap_tick(static_cast<SignedMIDITick>(new_pos.first)));
    std::get<3>(selection_range) = new_pos.second;
}

std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t> TrackEditing::get_selection_range()
    const {
    const auto [t0, t1, k0, k1] = selection_range;

    const auto [min_tick, max_tick] = t0 > t1 ? std::pair{t1, t0} : std::pair{t0, t1};
    const auto [min_track, max_track] =
        k0 > k1 ? std::pair<std::uint16_t, std::uint16_t>{k1, static_cast<std::uint16_t>(k0 + 1)}
                : std::pair<std::uint16_t, std::uint16_t>{k0, static_cast<std::uint16_t>(k1 + 1)};

    return {min_tick, max_tick, min_track, max_track};
}

std::pair<Vector2<float>, Vector2<float>> TrackEditing::get_selection_range_ui() const {

    const auto [min_tick, max_tick, min_track, max_track] = get_selection_range();

    const auto tl = midi_track_pos_to_screen_pos({min_tick, min_track});
    const auto br = midi_track_pos_to_screen_pos({max_tick, max_track});

    return {tl, br};
}

bool TrackEditing::is_mouse_over_select_area() const {
    if (!has_selection) {
        return false;
    }

    const auto [min_tick, max_tick, min_track, max_track] = get_selection_range();
    const auto [mouse_x, mouse_y] = mouse_info_.mouse_midi_track_pos;

    return mouse_x > min_tick && mouse_x < max_tick && mouse_y >= min_track && mouse_y < max_track;
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
    has_selection = false;
    selection->clear_selected();
}

void TrackEditing::delete_selection() {
    {
        SharedSelectedNotes* selected = _controller->get_selection();

        std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

        std::vector<std::uint16_t> affected_tracks;
        std::vector<std::vector<Note>> deleted_notes;
        std::vector<std::vector<std::size_t>> deleted_ids;

        for (auto& [trk, ids] : selected->take_selected_from_all()) {
            if (trk >= tracks->size()) {
                continue;
            }
            std::vector<Note>& notes_ref = tracks->at(trk).get_notes_mut();
            std::vector<Note> notes = std::exchange(notes_ref, {});

            auto [deleted, kept] = extract(std::move(notes), ids);
            notes_ref = std::move(kept);

            affected_tracks.push_back(trk);
            deleted_notes.push_back(std::move(deleted));
            deleted_ids.push_back(std::move(ids));
        }

        EditorActions* actions = _controller->get_actions();
        if (actions) {
            actions->register_action(DeleteNotesMultiTrack{
                std::move(deleted_ids), std::move(deleted_notes), std::move(affected_tracks)});
        }
    }

    deselect_all();
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
    const auto [_min_tick, _max_tick, min_track, max_track] = get_selection_range();
    SharedSelectedNotes* selection = _controller->get_selection();

    GhostTrackNotes ghost_notes;

    for (std::uint16_t trk = min_track; trk < max_track; ++trk) {
        if (!track_exists(trk)) {
            break;
        }

        const std::vector<std::size_t> selected_ids = selection->take_selected_from_track(trk);

        auto taken = take_some_notes_in_track(trk, selected_ids);
        if (!taken) {
            continue;
        }
        auto& [extracted, old_notes] = *taken;
        set_notes_in_track(trk, std::move(old_notes));

        ghost_notes.emplace_back(trk, std::move(extracted));
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

    insert_notes_and_ch_evs(track, {}, {});

    EditorActions* actions = _controller->get_actions();
    if (actions) {
        actions->register_action(AddTrack{track, std::nullopt, false});
    }
}

void TrackEditing::remove_track(std::uint16_t track) {
    std::uint16_t track_count = get_used_track_count();
    if (track >= track_count) {
        return;
    }

    {
        MIDITrack removed_track = remove_track_at(track);
        track_count -= 1;

        bool removed_first = false;
        if (track_count == 0) {
            track_count = 1;
            removed_first = true;
        }

        std::deque<MIDITrack> removed_track_queue;
        removed_track_queue.push_back(std::move(removed_track));

        EditorActions* actions = _controller->get_actions();
        if (actions) {
            actions->register_action(
                RemoveTrack{track, std::move(removed_track_queue), removed_first});
        }
    }

    {
        const MIDITrk active_track = _controller->get_active_track();
        if (active_track > track) {
            change_track(static_cast<std::uint16_t>(active_track - 1));
        }
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

void TrackEditing::decompose_track(std::uint16_t track, bool should_register) {
    std::vector<Note> notes;
    std::vector<ChannelEvent> ch_evs;
    {
        std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
        if (track >= tracks->size()) {
            return;
        }
        notes = std::exchange(tracks->at(track).get_notes_mut(), {});
        ch_evs = std::exchange(tracks->at(track).get_channel_evs_mut(), {});
    }

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

    std::erase_if(decomposed, [](const auto& t) { return t.first.empty() && t.second.empty(); });

    if (decomposed.empty()) {
        return;
    }

    const std::size_t decomposed_count = decomposed.size();

    {
        std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

        tracks->reserve(tracks->size() + decomposed_count - 1);

        for (std::size_t i = 0; i < decomposed_count; ++i) {
            auto& [notes_for_channel, evs_for_channel] = decomposed[i];

            if (i == 0) {
                MIDITrack& track_mut = tracks->at(track);
                track_mut.get_notes_mut() = std::move(notes_for_channel);
                track_mut.get_channel_evs_mut() = std::move(evs_for_channel);
            } else {
                MIDITrack new_track;
                new_track.get_notes_mut() = std::move(notes_for_channel);
                new_track.get_channel_evs_mut() = std::move(evs_for_channel);
                tracks->insert(tracks->begin() + static_cast<std::ptrdiff_t>(track + i),
                              std::move(new_track));
            }
        }
    }

    EditorActions* actions = _controller->get_actions();
    if (should_register && actions) {
        actions->register_action(
            DecomposeTrack{track, static_cast<std::uint16_t>(decomposed_count)});
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
    // why should track editing change the view settings?? main window should do that
    /*{
        std::lock_guard lock(view_settings_->mutex);
        view_settings_->value.pr_curr_track = new_track;
    }*/

    {
        ProjectManager* manager = _controller->get_project_manager();
        manager->get_project_data_mut().validate_tracks(new_track);
    }

    /*{
        std::unique_lock lock(pr_nav_->mutex);
        pr_nav_->value.curr_track = new_track;
    }*/
}

void TrackEditing::swap_tracks_and_register(std::uint16_t track_1, std::uint16_t track_2,
                                            bool allow_register) {
    const std::uint16_t track_count = get_used_track_count();

    {
        ProjectManager* manager = _controller->get_project_manager();
        ProjectData& project_data = manager->get_project_data_mut();

        if (track_1 >= track_count || track_2 >= track_count) {
            project_data.validate_tracks(std::max(track_1, track_2));
        }

        std::vector<MIDITrack>* tracks = manager->get_tracks();
        if (track_1 < tracks->size() && track_2 < tracks->size()) {
            std::swap(tracks->at(track_1), tracks->at(track_2));
        }
    }

    EditorActions* actions = _controller->get_actions();
    if (allow_register && actions) {
        actions->register_action(SwapTracks{track_1, track_2});
    }

    const std::uint16_t curr_track = _controller->get_active_track();
    if (curr_track == track_1) {
        change_track(track_2);
        return;
    }
    if (curr_track == track_2) {
        change_track(track_1);
        return;
    }
}

void TrackEditing::swap_tracks(std::uint16_t track_1, std::uint16_t track_2) {
    swap_tracks_and_register(track_1, track_2, true);
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

void TrackEditing::prepare_clipboard() {
    SharedClipboard* clipboard = _controller->get_clipboard();
    clipboard->clear_clipboard();

    const MIDITick clipboard_start = clipboard->get_clipboard_start_tick();
    clipboard->offset_from_playhead = static_cast<SignedMIDITick>(clipboard_start) -
                                      static_cast<SignedMIDITick>(_playhead_tick);
}

void TrackEditing::copy_notes() {
    SharedSelectedNotes* selection = _controller->get_selection();
    SharedClipboard* clipboard = _controller->get_clipboard();

    const std::vector<std::uint16_t> active_tracks =
        selection->get_active_selected_tracks();

    if (active_tracks.empty()) {
        return;
    }

    prepare_clipboard();

    for (const std::uint16_t track : active_tracks) {
        const std::vector<std::size_t>* selected =
            selection->get_selected_ids_in_track(track);
        if (!selected) {
            continue;
        }

        std::vector<Note> copied_notes = clone_notes(track, *selected);
        clipboard->move_notes_to_clipboard(std::move(copied_notes), track, false);
    }
}

void TrackEditing::cut_notes() {
    SharedSelectedNotes* selection = _controller->get_selection();
    SharedClipboard* clipboard = _controller->get_clipboard();

    const std::vector<std::uint16_t> active_tracks =
        selection->get_active_selected_tracks();

    if (active_tracks.empty()) {
        return;
    }
    prepare_clipboard();

    std::vector<EditorAction> actions_list;
    for (const std::uint16_t track : active_tracks) {
        std::vector<std::size_t> selected =
            selection->take_selected_from_track(track);
        if (selected.empty()) {
            continue;
        }

        auto old_notes = take_notes_in_track(track);
        if (!old_notes) {
            continue;
        }

        auto [cut_notes, retained_notes] = extract(std::move(*old_notes), selected);
        clipboard->move_notes_to_clipboard(cut_notes, track, false);

        set_notes_in_track(track, std::move(retained_notes));
        actions_list.push_back(DeleteNotes{std::move(selected), std::move(cut_notes), track});
    }

    EditorActions* actions = _controller->get_actions();
    if (actions) {
        actions->register_action(Bulk{std::move(actions_list)});
    }
}

void TrackEditing::paste_notes(std::uint16_t base_track) {
    SharedClipboard* clipboard = _controller->get_clipboard();
    SharedSelectedNotes* selection = _controller->get_selection();

    auto copied_notes = clipboard->get_notes_from_clipboard();
    const SignedMIDITick offset_from_playhead = clipboard->offset_from_playhead;

    if (copied_notes.empty()) {
        return;
    }
    std::stable_sort(copied_notes.begin(), copied_notes.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    const std::uint16_t first_track = copied_notes[0].first;
    std::uint16_t num_tracks = get_used_track_count();

    std::vector<EditorAction> track_actions;
    std::vector<EditorAction> actions_list;

    for (auto& [src_track, notes_vec] : copied_notes) {
        const auto rel = static_cast<std::uint16_t>(src_track - first_track);
        const auto dest_track = static_cast<std::uint16_t>(base_track + rel);

        while (dest_track >= num_tracks) {
            append_empty_track();
            track_actions.push_back(AddTrack{dest_track, std::nullopt, false});
            num_tracks += 1;
        }

        auto old_notes = take_notes_in_track(dest_track);
        if (!old_notes) {
            continue;
        }

        for (Note& note : notes_vec) {
            const auto shifted = static_cast<SignedMIDITick>(note.get_start()) +
                                 static_cast<SignedMIDITick>(_playhead_tick) + offset_from_playhead;
            note.set_start(static_cast<MIDITick>(std::max<SignedMIDITick>(shifted, 0)));
        }

        auto [new_notes, new_ids] =
            merge_notes_and_return_ids(std::move(*old_notes), std::move(notes_vec));
        set_notes_in_track(dest_track, std::move(new_notes));

        selection->set_selected_in_track(new_ids, dest_track);

        actions_list.push_back(PlaceNotes{std::move(new_ids), std::nullopt, dest_track});
    }

    track_actions.insert(track_actions.end(), std::make_move_iterator(actions_list.begin()),
                         std::make_move_iterator(actions_list.end()));

    EditorActions* actions = _controller->get_actions();
    if (actions) {
        actions->register_action(Bulk{std::move(track_actions)});
    }
}

void TrackEditing::apply_action(EditorAction& action) {
    SharedClipboard* clipboard = _controller->get_clipboard();
    SharedSelectedNotes* selection = _controller->get_selection();

    if (auto* add_track = std::get_if<AddTrack>(&action.node)) {
        Debugger::log("Undoing or redoing removing tracks");
        // fixed rust bug: asserted here; a broken undo entry now logs instead
        if (!add_track->deleted_tracks || add_track->deleted_tracks->empty()) {
            Debugger::log_error(
                "[ADD_TRACKS] Something has gone wrong while trying to add a track.");
            return;
        }

        std::deque<MIDITrack> recovered = std::move(*add_track->deleted_tracks);
        add_track->deleted_tracks.reset();
        MIDITrack recovered_track = std::move(recovered.front());
        recovered.pop_front();
        insert_track_at(add_track->track, std::move(recovered_track));

        const std::uint16_t curr_track = _controller->get_active_track();
        if (add_track->track <= curr_track) {
            change_track(static_cast<std::uint16_t>(curr_track + 1));
        }
    } else if (auto* rem_track = std::get_if<RemoveTrack>(&action.node)) {
        Debugger::log("Undoing or redoing adding tracks");
        std::deque<MIDITrack> rem_track_queue;
        rem_track_queue.push_front(remove_track_at(rem_track->track));
        rem_track->deleted_tracks = std::move(rem_track_queue);

        const std::uint16_t curr_track = _controller->get_active_track();
        if (rem_track->track < curr_track) {
            change_track(static_cast<std::uint16_t>(curr_track - 1));
        }
    } else if (auto* place_multi = std::get_if<PlaceNotesMultiTrack>(&action.node)) {
        Debugger::log("Undoing or redoing removing notes in multiple tracks");
        // fixed rust bug: asserted here; a broken undo entry now logs instead
        if (!place_multi->notes) {
            Debugger::log_error(
                "[PLACE NOTES] Something has gone wrong while trying to add back notes in "
                "multiple tracks.");
            return;
        }

        std::vector<std::vector<Note>> recovered_notes = std::move(*place_multi->notes);
        place_multi->notes.reset();

        const std::size_t count = std::min(recovered_notes.size(), place_multi->note_groups.size());
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint16_t track = place_multi->note_groups[i];
            auto old_notes = take_notes_in_track(track);
            if (!old_notes) {
                continue;
            }
            set_notes_in_track(track,
                               merge_notes(std::move(*old_notes), std::move(recovered_notes[i])));
        }
    } else if (auto* move_multi = std::get_if<NotesMoveMultiTrack>(&action.node)) {
        Debugger::log("Undoing or redoing moving notes in multiple tracks");
        std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> new_note_ids;

        for (const auto& [track, note_ids] : move_multi->note_track_ids) {
            auto taken = take_some_notes_in_track(track, note_ids);
            if (!taken) {
                continue;
            }
            auto& [to_move, old_notes] = *taken;
            set_notes_in_track(track, std::move(old_notes));

            const auto new_track = static_cast<MIDITrk>(static_cast<SignedMIDITrk>(track) +
                                                        move_multi->moved_by.second);

            for (Note& note : to_move) {
                note.set_start(static_cast<MIDITick>(
                    static_cast<SignedMIDITick>(note.get_start()) + move_multi->moved_by.first));
            }

            auto dest_notes = take_notes_in_track(new_track);
            if (!dest_notes) {
                continue;
            }
            auto [merged, new_ids] =
                merge_notes_and_return_ids(std::move(*dest_notes), std::move(to_move));

            set_notes_in_track(new_track, std::move(merged));
            new_note_ids.emplace_back(new_track, std::move(new_ids));
        }

        move_multi->note_track_ids = std::move(new_note_ids);
    } else if (auto* del_multi = std::get_if<DeleteNotesMultiTrack>(&action.node)) {
        Debugger::log("Undoing or redoing adding notes in multiple tracks");
        std::vector<std::vector<Note>> notes_deleted;

        const std::size_t count =
            std::min(del_multi->note_ids.size(), del_multi->note_groups.size());
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint16_t track = del_multi->note_groups[i];
            auto old_notes = take_notes_in_track(track);
            if (!old_notes) {
                continue;
            }

            std::vector<std::size_t> old_sel_ids =
                selection->take_selected_from_track(track);
            auto [deleted, new_notes, new_ids] =
                extract_and_remap_ids(std::move(*old_notes), del_multi->note_ids[i], old_sel_ids);
            selection->set_selected_in_track(std::move(new_ids), track);

            set_notes_in_track(track, std::move(new_notes));

            notes_deleted.push_back(std::move(deleted));
        }

        del_multi->notes = std::move(notes_deleted);
    } else if (auto* compose = std::get_if<ComposeTrack>(&action.node)) {
        Debugger::log("Undoing or redoing adding notes in multiple tracks");
        std::vector<std::vector<Note>> decomposed_tracks;
        {
            std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

            for (std::uint16_t i = 0; i < compose->channel_count; ++i) {
                const std::size_t idx = static_cast<std::size_t>(compose->track) + i;
                if (idx >= tracks->size()) {
                    break;
                }
                decomposed_tracks.push_back(
                    std::exchange(tracks->at(idx).get_notes_mut(), {}));
            }
        }

        std::vector<Note> composed;
        for (auto& part : decomposed_tracks) {
            composed = composed.empty() ? std::move(part)
                                        : merge_notes(std::move(composed), std::move(part));
        }

        {
            std::vector<MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();

            if (compose->track < tracks->size()) {
                tracks->at(compose->track).get_notes_mut() = std::move(composed);
            }

            for (std::uint16_t i = compose->channel_count; i-- > 1;) {
                const std::size_t idx = static_cast<std::size_t>(compose->track) + i;
                if (idx < tracks->size()) {
                    tracks->erase(tracks->begin() + static_cast<std::ptrdiff_t>(idx));
                }
            }
        }
    } else if (auto* decompose = std::get_if<DecomposeTrack>(&action.node)) {
        decompose_track(decompose->track, false);
    } else if (auto* swap = std::get_if<SwapTracks>(&action.node)) {
        swap_tracks_and_register(swap->track_1, swap->track_2, false);
    }
}

std::pair<MIDITick, std::uint16_t> TrackEditing::get_mouse_midi_pos_snapped() const {
    const auto snapped_tick = static_cast<MIDITick>(
        snap_tick(static_cast<SignedMIDITick>(mouse_info_.mouse_midi_track_pos.first)));
    return {snapped_tick, mouse_info_.mouse_midi_track_pos.second};
}

SignedMIDITick TrackEditing::snap_tick(SignedMIDITick tick) const {
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

MIDITick TrackEditing::get_min_snap_tick_length() const {
    app::EditorToolSettings* settings = _controller->get_editor_tool_settings();
    const uint16_t ppq = _controller->get_project_manager()->get_ppq();

    const auto snap_ratio = settings->snap_ratio;
    if (snap_ratio.first == 0) {
        return 1;
    }
    return (static_cast<MIDITick>(ppq) * 4 * static_cast<MIDITick>(snap_ratio.first)) /
           static_cast<MIDITick>(snap_ratio.second);
}

}
