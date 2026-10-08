#include "editor/edit_functions.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <imgui.h>

#include "editor/editing.h"
#include "editor/editing/note_editing.h"
#include "editor/editor_controller.h"
#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "midi/midi_track.h"
#include "util/debugger.h"
#include "util/numeric.h"

#define SORT_CHECKS

namespace andromeda::editor {

using midi::Note;
using note_seq::exclude;
using note_seq::extract;
using note_seq::merge_notes;
using note_seq::merge_notes_and_return_ids;
using util::Debugger;

namespace {

[[noreturn]] void deprecated(const char* msg) {
    throw std::runtime_error(std::format("Use of deprecated code: {}", msg));
}

struct TickKeyHash {
    std::size_t operator()(const std::pair<MIDITick, std::uint8_t>& v) const noexcept {
        return (static_cast<std::size_t>(v.first) << 8) ^ v.second;
    }
};

}

void EditFunctions::apply_function(EditFunction& func, editor::EditorController* controller) {
    std::visit(
        [&](auto&& f) {
            using T = std::decay_t<decltype(f)>;

            if constexpr (std::is_same_v<T, edit_fn::FlipX>) {
                deprecated("use flip x from plugins instead");
            } else if constexpr (std::is_same_v<T, edit_fn::FlipY>) {
                deprecated("use flip y from plugins instead");
            } else if constexpr (std::is_same_v<T, edit_fn::Stretch>) {
                // having it stretch on only one track is annoying, so it'll stretch for multiple tracks now
                // (if the selection spans multiple tracks)
                if (f.factor == 1.0f) {
                    return;
                }

                std::vector<EditorAction> bulk_actions{};
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                {
                    // pass 1: get the first note for the stretch anchor (soon we should have it anchor around playhead position)
                    MIDITick stretch_anchor = std::numeric_limits<MIDITick>::max();
                    for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                        midi::MIDITrack& track = tracks[track_id];
                        auto* note_ids = selection->get_selected_ids_in_track(track_id);
                        if (!note_ids) continue;

                        if (note_ids->empty()) continue;
                        MIDITick first_note_tick = track.get_notes()[note_ids->front()].get_start();
                        if (first_note_tick < stretch_anchor) stretch_anchor = first_note_tick;
                    }

                    // pass 2: apply stretch
                    for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                        midi::MIDITrack& track = tracks[track_id];

                        auto* note_ids = selection->get_selected_ids_in_track(track_id);
                        if (!note_ids) continue;

                        if (note_ids->empty()) continue;

                        auto& notes = track.get_notes_mut();

                        auto [notes_to_stretch, remaining_notes] = extract(std::move(notes), *note_ids);

                        // fixed rust bug: indexed [0] of an empty selection
                        if (notes_to_stretch.empty()) {
                            notes = std::move(remaining_notes);
                            Debugger::log_warning("[EditFunctions] Stretch with an empty selection");
                            continue;
                        }

                        std::vector<std::pair<SignedMIDITick, std::int16_t>> pos_change;
                        std::vector<SignedMIDITick> length_change;
                        pos_change.reserve(notes_to_stretch.size());
                        length_change.reserve(notes_to_stretch.size());

                        for (Note& note : notes_to_stretch) {
                            const MIDITick old_length = note.get_length();
                            const MIDITick old_tick = note.get_start();

                            const auto new_length = util::saturating_cast<MIDITick>(
                                std::round(static_cast<float>(old_length) * f.factor));
                            const auto new_tick =
                                util::saturating_cast<MIDITick>(
                                    std::round(static_cast<float>(old_tick - stretch_anchor) * f.factor)) +
                                stretch_anchor;

                            note.set_start(new_tick);
                            note.set_length(new_length);

                            pos_change.emplace_back(static_cast<SignedMIDITick>(new_tick) -
                                static_cast<SignedMIDITick>(old_tick),
                                static_cast<std::int16_t>(0));
                            length_change.push_back(static_cast<SignedMIDITick>(new_length) -
                                static_cast<SignedMIDITick>(old_length));
                        }

                        auto [merged, new_ids] = merge_notes_and_return_ids(std::move(remaining_notes),
                            std::move(notes_to_stretch));
                        notes = std::move(merged);

                        std::vector<EditorAction> track_bulk{};

                        // make copy of selection so notes don't select
                        std::vector<std::size_t> affected_ids = *note_ids;
                        track_bulk.push_back(
                            LengthChange{ std::move(affected_ids), std::move(length_change), static_cast<uint16_t>(track_id) });
                        track_bulk.push_back(
                            NotesMove{ std::move(new_ids), std::move(pos_change), static_cast<uint16_t>(track_id), true });

                        bulk_actions.push_back(Bulk{ std::move(track_bulk) });
                    }
                }

                if (!bulk_actions.empty())
                    controller->get_actions()->register_action(Bulk{ std::move(bulk_actions) });
            } else if constexpr (std::is_same_v<T, edit_fn::Chop>) {
                if (f.max_tick_len == 0) {
                    Debugger::log_error("[EditFunctions] Chop with target length 0");
                    return;
                }

                std::vector<EditorAction> bulk_actions;

                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                for (std::uint16_t track_id = 0; track_id < tracks.size(); ++track_id) {
                    auto* note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!note_ids || note_ids->empty())
                        continue;

                    auto& track = tracks[track_id];
                    auto& notes = track.get_notes_mut();

                    std::vector<SignedMIDITick> changed_lengths;
                    changed_lengths.reserve(note_ids->size());

                    std::vector<std::size_t> changed_lengths_ids;
                    changed_lengths_ids.reserve(note_ids->size());

                    std::vector<Note> new_notes;

                    for (const std::size_t id : *note_ids) {
                        if (id >= notes.size()) continue;

                        Note& note = notes[id];

                        if (note.get_length() <= f.max_tick_len) continue;

                        const MIDITick old_length = note.get_length();
                        const MIDITick new_length = f.max_tick_len;

                        note.set_length(new_length);

                        MIDITick remaining_length = old_length - new_length;
                        MIDITick start_time = note.get_start() + new_length;

                        changed_lengths.push_back(
                            static_cast<SignedMIDITick>(new_length) - static_cast<SignedMIDITick>(old_length));

                        changed_lengths_ids.push_back(id);

                        while (remaining_length > 0) {
                            const MIDITick next_len = std::min(remaining_length, f.max_tick_len);

                            new_notes.emplace_back(start_time, next_len, note.get_key(), note.get_velocity(), note.get_channel());

                            remaining_length -= next_len;
                            start_time += next_len;
                        }
                    }

                    if (new_notes.empty() || changed_lengths.empty()) continue;

                    std::stable_sort(new_notes.begin(), new_notes.end(),
                        [](const Note& a, const Note& b) {
                            return a.get_start() < b.get_start();
                        });

                    auto [merged, chopped_ids] = merge_notes_and_return_ids(std::move(notes), std::move(new_notes));

                    notes = std::move(merged);

                    if (chopped_ids.empty()) continue;

                    std::vector<EditorAction> track_bulk;

                    track_bulk.push_back(
                        LengthChange{std::move(changed_lengths_ids), std::move(changed_lengths), track_id});

                    track_bulk.push_back(
                        PlaceNotes{std::move(chopped_ids), std::nullopt, track_id});

                    bulk_actions.push_back(
                        Bulk{std::move(track_bulk)});
                }

                if (!bulk_actions.empty()) {
                    controller->get_actions()->register_action(
                        Bulk{ std::move(bulk_actions) });
                }
            } else if constexpr (std::is_same_v<T, edit_fn::Glue>) {
                std::vector<EditorAction> bulk_actions;

                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                for (std::uint16_t track_id = 0; track_id < tracks.size(); ++track_id) {
                    std::vector<size_t>* sel_note_ids = &selection->get_selected_ids_mut(track_id);
                    if (!sel_note_ids || sel_note_ids->empty())
                        continue;

                    auto& track = tracks[track_id];
                    auto& notes = track.get_notes_mut();

                    auto [notes_to_glue, remaining_notes] = extract(std::move(notes), *sel_note_ids);

                    const std::size_t table_size = f.separate_channels ? 16 * 128 : 128;

                    std::vector<std::optional<std::size_t>> glue_table(
                        table_size, std::nullopt);

                    std::vector<Note> kept_notes;
                    std::vector<std::size_t> kept_original_ids;
                    std::vector<MIDITick> kept_original_lengths;

                    std::vector<Note> removed_notes;
                    std::vector<std::size_t> removed_ids;

                    const std::size_t pair_count = std::min(notes_to_glue.size(), sel_note_ids->size());

                    for (std::size_t i = 0; i < pair_count; ++i) {
                        const Note note = notes_to_glue[i];
                        const std::size_t orig_id = (*sel_note_ids)[i];

                        const std::size_t key = note.get_key();
                        const std::size_t ch_index = f.separate_channels ? static_cast<std::size_t>(note.get_channel()) * 128 : 0;

                        const std::size_t table_idx = ch_index + key;

                        const MIDITick note_start = note.get_start();
                        const MIDITick note_end = note.end();

                        if (const auto last_idx = glue_table[table_idx]) {
                            Note& last_note = kept_notes[*last_idx];

                            const MIDITick last_start = last_note.get_start();
                            const MIDITick last_end = last_note.end();

                            if (note_start <= last_end + f.glue_threshold) {
                                last_note.set_length(
                                    std::max(last_end, note_end) - last_start);

                                removed_notes.push_back(note);
                                removed_ids.push_back(orig_id);
                                continue;
                            }
                        }

                        const std::size_t kept_idx = kept_notes.size();
                        const MIDITick original_len = note.get_length();

                        kept_notes.push_back(note);
                        kept_original_ids.push_back(orig_id);
                        kept_original_lengths.push_back(original_len);
                        glue_table[table_idx] = kept_idx;
                    }

                    auto [merged, new_ids] = merge_notes_and_return_ids( std::move(remaining_notes), std::move(kept_notes));

                    notes = std::move(merged);

                    std::vector<SignedMIDITick> length_deltas;
                    length_deltas.reserve(kept_original_ids.size());

                    for (std::size_t i = 0; i < kept_original_lengths.size() && i < new_ids.size(); ++i) {
                        const MIDITick final_len = notes[new_ids[i]].get_length();

                        length_deltas.push_back(
                            static_cast<SignedMIDITick>(final_len) - static_cast<SignedMIDITick>(kept_original_lengths[i]));
                    }

                    // Keep the surviving notes selected; removed notes are no longer selected.
                    *sel_note_ids = exclude(std::move(*sel_note_ids), removed_ids);

                    std::vector<EditorAction> track_bulk;

                    if (!removed_ids.empty()) {
                        track_bulk.push_back(
                            Deselect{removed_ids, track_id});
                    }

                    if (std::any_of(length_deltas.begin(), length_deltas.end(),
                        [](SignedMIDITick d) { return d != 0; })) {
                        track_bulk.push_back(
                            LengthChange{std::move(new_ids), std::move(length_deltas), track_id});
                    }

                    if (!removed_ids.empty()) {
                        track_bulk.push_back(
                            DeleteNotes{ std::move(removed_ids), std::move(removed_notes), track_id});
                    }

                    if (!track_bulk.empty()) {
                        bulk_actions.push_back(
                            Bulk{ std::move(track_bulk) });
                    }
                }

                if (!bulk_actions.empty()) {
                    controller->get_actions()->register_action(
                        Bulk{ std::move(bulk_actions) });
                }
            } else if constexpr (std::is_same_v<T, edit_fn::SliceAtTick>) {
                std::vector<EditorAction> bulk_actions;

                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty())
                        continue;

                    auto& track = tracks[track_id];
                    auto& notes = track.get_notes_mut();

                    std::vector<SignedMIDITick> changed_lengths;
                    changed_lengths.reserve(sel_note_ids->size());

                    std::vector<Note> new_notes;
                    new_notes.reserve(sel_note_ids->size());

                    std::vector<std::size_t> affected_ids;
                    affected_ids.reserve(sel_note_ids->size());

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;

                        Note& note = notes[id];

                        const MIDITick start = note.get_start();
                        const MIDITick end = note.end();

                        if (start >= f.slice_tick || end <= f.slice_tick) continue;

                        const MIDITick left_len = f.slice_tick - start;
                        const MIDITick right_len = end - f.slice_tick;

                        if (left_len == 0 || right_len == 0) continue;

                        const MIDITick old_length = note.get_length();
                        const MIDITick new_length = left_len;

                        note.set_length(new_length);

                        changed_lengths.push_back(
                            static_cast<SignedMIDITick>(new_length) - static_cast<SignedMIDITick>(old_length));

                        new_notes.emplace_back(
                            f.slice_tick, right_len, note.get_key(),
                            note.get_velocity(), note.get_channel());

                        affected_ids.push_back(id);
                    }

                    if (changed_lengths.empty()) continue;

                    auto [merged, new_ids] =
                        merge_notes_and_return_ids(std::move(notes), std::move(new_notes));

                    notes = std::move(merged);

                    std::vector<EditorAction> track_bulk;

                    track_bulk.push_back(
                        PlaceNotes{std::move(new_ids), std::nullopt, track_id});

                    track_bulk.push_back(
                        LengthChange{std::move(affected_ids), std::move(changed_lengths), track_id});

                    bulk_actions.push_back(Bulk{ std::move(track_bulk) });
                }

                if (!bulk_actions.empty()) {
                    controller->get_actions()->register_action(
                        Bulk{ std::move(bulk_actions) });
                }
            } else if constexpr (std::is_same_v<T, edit_fn::FadeNotes>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                // pass 1: find the leftmost / rightmost selected notes across all tracks
                std::optional<MIDITick> min_tick;
                std::optional<MIDITick> max_tick;

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;

                        const Note& note = notes[id];

                        if (!min_tick || note.get_start() < *min_tick)
                            min_tick = note.get_start();

                        if (!max_tick || note.get_start() > *max_tick)
                            max_tick = note.get_start();
                    }
                }

                if (!min_tick || !max_tick) {
                    Debugger::log_warning("[EditFunctions] Fade with an empty selection");
                    return;
                }

                const MIDITick fade_min_tick = *min_tick;
                const MIDITick fade_max_tick = *max_tick;

                std::vector<EditorAction> bulk_actions;

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);

                    if (!sel_note_ids || sel_note_ids->empty())
                        continue;

                    auto& notes = tracks[track_id].get_notes_mut();

                    std::vector<std::size_t> affected_ids;
                    std::vector<std::int8_t> vel_changes;

                    affected_ids.reserve(sel_note_ids->size());
                    vel_changes.reserve(sel_note_ids->size());

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;

                        Note& note = notes[id];

                        float vel_fac = 1.0f;

                        if (fade_max_tick != fade_min_tick) {
                            vel_fac =
                                static_cast<float>(note.get_start() - fade_min_tick) /
                                static_cast<float>(fade_max_tick - fade_min_tick);
                        }

                        if (f.fade_out) vel_fac = 1.0f - vel_fac;

                        const std::uint8_t old_velocity = note.get_velocity();
                        const std::uint8_t new_velocity =
                            std::clamp<std::uint8_t>(
                                util::saturating_cast<std::uint8_t>(
                                    static_cast<float>(old_velocity) * vel_fac),
                                1, 127);

                        if (new_velocity == old_velocity) continue;

                        vel_changes.push_back(
                            static_cast<std::int8_t>(
                                static_cast<int>(new_velocity) - static_cast<int>(old_velocity)));

                        affected_ids.push_back(id);
                        note.set_velocity(new_velocity);
                    }

                    if (affected_ids.empty())
                        continue;

                    std::vector<EditorAction> track_bulk;

                    track_bulk.push_back(
                        VelocityChange{std::move(affected_ids), std::move(vel_changes), track_id});

                    bulk_actions.push_back(
                        Bulk{std::move(track_bulk)});
                }

                if (!bulk_actions.empty()) {
                    controller->get_actions()->register_action(
                        Bulk{ std::move(bulk_actions) });
                }
            } else if constexpr (std::is_same_v<T, edit_fn::Transpose>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                std::vector<EditorAction> bulk_actions{};

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);

                    if (!sel_note_ids || sel_note_ids->empty())
                        continue;

                    auto& notes = tracks[track_id].get_notes_mut();

                    std::vector<SignedMIDIKey> key_changes;
                    key_changes.reserve(sel_note_ids->size());
                    std::vector<std::size_t> affected_note_ids;

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;
                        Note& note = notes[id];
                        const std::uint8_t old_key = note.get_key();

                        // fixed rust bug: the i8 addition wrapped, so keys pushed past 127 landed on 0
                        const int new_key =
                            std::clamp(static_cast<int>(old_key) + static_cast<int>(f.amount), 0, 127);

                        const auto key_change =
                            static_cast<SignedMIDIKey>(new_key - static_cast<int>(old_key));

                        if (key_change == 0) continue;

                        affected_note_ids.push_back(id);
                        key_changes.push_back(key_change);
                        note.set_key(static_cast<MIDIKey>(new_key));
                    }

                    if (!key_changes.empty())
                        bulk_actions.push_back(KeyChange{ std::move(affected_note_ids), std::move(key_changes), track_id });
                }

                if (!bulk_actions.empty()) {
                    controller->get_actions()->register_action(Bulk{ std::move(bulk_actions) });
                }
            } else if constexpr (std::is_same_v<T, edit_fn::RemoveOverlaps>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                std::vector<EditorAction> bulk_actions;

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);

                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    auto& track = tracks[track_id];
                    auto& notes = track.get_notes_mut();

                    auto [extracted_notes, remaining_notes] = extract(std::move(notes), *sel_note_ids);

                    std::unordered_set<std::pair<MIDITick, std::uint8_t>,TickKeyHash> lookup;

                    lookup.reserve(extracted_notes.size());

                    std::vector<Note> kept_notes;
                    kept_notes.reserve(extracted_notes.size());

                    std::vector<std::size_t> removed_indices;
                    removed_indices.reserve(extracted_notes.size());

                    std::vector<Note> removed_notes;

                    for (std::size_t idx = 0; idx < extracted_notes.size(); ++idx) {
                        const Note& note = extracted_notes[idx];

                        const std::pair<MIDITick, std::uint8_t>
                            entry{note.get_start(), note.get_key()};

                        if (lookup.contains(entry)) {
                            removed_notes.push_back(note);
                            removed_indices.push_back(idx);
                        } else {
                            lookup.insert(entry);
                            kept_notes.push_back(note);
                        }
                    }

                    notes = merge_notes(std::move(remaining_notes), std::move(kept_notes));

                    if (removed_notes.empty()) {
                        Debugger::log(std::format("[EditFunctions] No overlapped notes were removed from track {}.", track_id));
                        continue;
                    }

                    Debugger::log(
                        std::format("[EditFunctions] Removed {} notes from track {}.", removed_indices.size(), track_id));

                    std::vector<EditorAction> track_bulk;

                    track_bulk.push_back(DeleteNotes{ std::move(removed_indices), std::move(removed_notes), track_id});

                    bulk_actions.push_back( Bulk{ std::move(track_bulk) });
                }

                if (!bulk_actions.empty()) {
                    controller->get_actions()->register_action(Bulk{std::move(bulk_actions)});
                }
            } else {
                static_assert(std::is_same_v<T, edit_fn::SetChannel>);

                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                std::vector<EditorAction> bulk_actions;

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {

                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);

                    if (!sel_note_ids || sel_note_ids->empty())
                        continue;

                    auto& notes = tracks[track_id].get_notes_mut();

                    std::vector<std::size_t> affected_ids;
                    std::vector<std::int8_t> channel_changes;

                    affected_ids.reserve(sel_note_ids->size());
                    channel_changes.reserve(sel_note_ids->size());

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;

                        Note& note = notes[id];

                        const std::uint8_t old_channel = note.get_channel();

                        if (old_channel == f.channel) continue;

                        channel_changes.push_back(
                            static_cast<std::int8_t>(
                                static_cast<int>(f.channel) - static_cast<int>(old_channel)));

                        affected_ids.push_back(id);

                        note.channel = f.channel;
                    }

                    if (affected_ids.empty())
                        continue;

                    bulk_actions.push_back(
                        ChannelChange{std::move(affected_ids), std::move(channel_changes), track_id});

                    Debugger::log(
                        std::format("Changed channels in selection on track {} to channel {}", track_id, f.channel));
                }

                if (!bulk_actions.empty()) {
                    controller->get_actions()->register_action(
                        Bulk{std::move(bulk_actions)});
                }
            }
        },
        std::move(func));
}

void EFDialogBase::apply(const std::function<EditFunction(editor::EditorController*)>& make_func) {
    if (!_controller) return;

    EditFunction func = make_func(_controller);
    _controller->get_edit_functions()
        ->apply_function(func, _controller);
}

app::MaybeDlgAction EFStretchDialog::draw(const app::ImageResources&) {
    stretch_factor.show("Stretch factor (x):");
    return std::nullopt;
}

std::optional<app::DialogActionButtons> EFStretchDialog::get_action_buttons() const {
    return app::DlgOk{[](app::Dialog& dlg) -> app::MaybeDlgAction {
        auto& self = static_cast<EFStretchDialog&>(dlg);
        self.apply([&self](editor::EditorController* controller) -> EditFunction {
            return edit_fn::Stretch{controller, self.stretch_factor.value()};
        });
        return app::DialogClose{self.get_dialog_name()};
    }};
}

app::MaybeDlgAction EFChopDialog::draw(const app::ImageResources&) {
    target_tick_len.show("Chop tick length");
    return std::nullopt;
}

std::optional<app::DialogActionButtons> EFChopDialog::get_action_buttons() const {
    return app::DlgOkCancel{[](app::Dialog& dlg) -> app::MaybeDlgAction {
        auto& self = static_cast<EFChopDialog&>(dlg);
        self.apply([&self](editor::EditorController* controller) -> EditFunction {
            return edit_fn::Chop{controller, self.target_tick_len.value()};
        });
        return app::DialogClose{self.get_dialog_name()};
    },
    app::dialog_default_close_action()};
}

app::MaybeDlgAction EFGlueDialog::draw(const app::ImageResources&) {
    glue_threshold.show("Glue threshold (in ticks)");
    ImGui::Checkbox("Keep channels separate", &separate_channels);
    return std::nullopt;
}

std::optional<app::DialogActionButtons> EFGlueDialog::get_action_buttons() const {
    return app::DlgOkCancel{
        [](app::Dialog& dlg) -> app::MaybeDlgAction {
            auto& self = static_cast<EFGlueDialog&>(dlg);
            self.apply([&self](editor::EditorController* controller) -> EditFunction {
                return edit_fn::Glue{controller, self.glue_threshold.value(), self.separate_channels};
            });
            return app::DialogClose{self.get_dialog_name()};
        },
        app::dialog_default_close_action()};
}

app::MaybeDlgAction EFSetChannelDialog::draw(const app::ImageResources&) {
    new_channel.show("New channel");
    return std::nullopt;
}

std::optional<app::DialogActionButtons> EFSetChannelDialog::get_action_buttons() const {
    return app::DlgOkCancel{[](app::Dialog& dlg) -> app::MaybeDlgAction {
            auto& self = static_cast<EFSetChannelDialog&>(dlg);
            self.apply([&self](editor::EditorController*) -> EditFunction {
                return edit_fn::SetChannel{self.new_channel.value()};
            });
            return app::DialogClose{self.get_dialog_name()};
        },
        app::dialog_default_close_action()};
}

}
