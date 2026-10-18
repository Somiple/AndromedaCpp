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
                if (f.factor == 1.0f) {
                    return;
                }

                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                // Pass 1: find stretch anchor
                MIDITick stretch_anchor = std::numeric_limits<MIDITick>::max();
                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!note_ids || note_ids->empty()) continue;
                    MIDITick first_note_tick = tracks[track_id].get_notes()[note_ids->front()].get_start();
                    if (first_note_tick < stretch_anchor) stretch_anchor = first_note_tick;
                }

                if (stretch_anchor == std::numeric_limits<MIDITick>::max()) {
                    return;
                }

                // Begin transaction
                auto tx = controller->begin_action("Stretch Selection");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!note_ids || note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *note_ids);

                    for (const std::size_t id : *note_ids) {
                        if (id >= notes.size()) continue;
                        const auto& note = notes[id];

                        const MIDITick old_length = note.get_length();
                        const MIDITick old_tick = note.get_start();

                        const auto new_length = util::saturating_cast<MIDITick>(
                            std::round(static_cast<float>(old_length) * f.factor));
                        const auto new_tick =
                            util::saturating_cast<MIDITick>(
                                std::round(static_cast<float>(old_tick - stretch_anchor) * f.factor)) +
                            stretch_anchor;

                        notes_mutator.set_length(id, new_length < 1 ? 1 : new_length);
                        notes_mutator.set_position(id, new_tick, note.get_key());
                    }
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit Stretch transaction");
                }

            }
            else if constexpr (std::is_same_v<T, edit_fn::Chop>) {
                if (f.max_tick_len == 0) {
                    Debugger::log_error("[EditFunctions] Chop with target length 0");
                    return;
                }

                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();
                auto tx = controller->begin_action("Chop Selection");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); ++track_id) {
                    auto* note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!note_ids || note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *note_ids);

                    std::vector<std::size_t> surviving_ids;
                    surviving_ids.reserve(note_ids->size());

                    for (const std::size_t id : *note_ids) {
                        if (id >= notes.size()) continue;
                        const auto& note = notes[id];

                        if (note.get_length() <= f.max_tick_len) {
                            surviving_ids.push_back(id);
                            continue;
                        }

                        const MIDITick old_length = note.get_length();
                        const MIDITick new_length = f.max_tick_len;

                        notes_mutator.set_length(id, new_length);
                        surviving_ids.push_back(id);

                        MIDITick remaining_length = old_length - new_length;
                        MIDITick start_time = note.get_start() + new_length;

                        while (remaining_length > 0) {
                            const MIDITick next_len = std::min(remaining_length, f.max_tick_len);
                            notes_mutator.add_note(Note{ start_time, next_len, note.get_key(), note.get_velocity(), note.get_channel() });
                            remaining_length -= next_len;
                            start_time += next_len;
                        }
                    }

                    // Update selection to include newly chopped segments
                    tx.selection().set(track_id, notes_mutator.remap_ids(surviving_ids));
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit Chop transaction");
                }

            }
            else if constexpr (std::is_same_v<T, edit_fn::Glue>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();
                auto tx = controller->begin_action("Glue Notes");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); ++track_id) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                    const std::size_t table_size = f.separate_channels ? 16 * 128 : 128;
                    std::vector<std::optional<std::size_t>> glue_table(table_size, std::nullopt);

                    std::vector<std::size_t> to_delete;

                    for (const std::size_t orig_id : *sel_note_ids) {
                        if (orig_id >= notes.size()) continue;
                        const Note& note = notes[orig_id];

                        const std::size_t key = note.get_key();
                        const std::size_t ch_index = f.separate_channels ? static_cast<std::size_t>(note.get_channel()) * 128 : 0;
                        const std::size_t table_idx = ch_index + key;

                        const MIDITick note_start = note.get_start();
                        const MIDITick note_end = note.end();

                        if (const auto last_orig_id = glue_table[table_idx]) {
                            // We compare against the original note's baseline start/end 
                            const auto& last_note = notes[*last_orig_id];
                            const MIDITick last_start = last_note.get_start();
                            const MIDITick last_end = last_note.end();

                            if (note_start <= last_end + f.glue_threshold) {
                                // Extend the previous note's length
                                const MIDITick new_len = std::max(last_end, note_end) - last_start;
                                notes_mutator.set_length(*last_orig_id, new_len);

                                // Mark current note for deletion since it's glued into the previous one
                                to_delete.push_back(orig_id);
                                continue;
                            }
                        }
                        glue_table[table_idx] = orig_id;
                    }

                    if (!to_delete.empty()) {
                        notes_mutator.delete_notes(to_delete);
                        tx.selection().remap(notes_mutator);
                    }
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit Glue transaction");
                }
            }
            else if constexpr (std::is_same_v<T, edit_fn::SliceAtTick>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();
                auto tx = controller->begin_action("Slice at Tick");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;
                        const auto& note = notes[id];

                        const MIDITick start = note.get_start();
                        const MIDITick end = note.end();

                        if (start >= f.slice_tick || end <= f.slice_tick) continue;

                        const MIDITick left_len = f.slice_tick - start;
                        const MIDITick right_len = end - f.slice_tick;

                        if (left_len == 0 || right_len == 0) continue;

                        notes_mutator.set_length(id, left_len);
                        notes_mutator.add_note(Note{ f.slice_tick, right_len, note.get_key(), note.get_velocity(), note.get_channel() });
                    }
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit SliceAtTick transaction");
                }

            }
            else if constexpr (std::is_same_v<T, edit_fn::FadeNotes>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();

                std::optional<MIDITick> min_tick;
                std::optional<MIDITick> max_tick;

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;
                    const auto& notes = tracks[track_id].get_notes();
                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;
                        if (!min_tick || notes[id].get_start() < *min_tick) min_tick = notes[id].get_start();
                        if (!max_tick || notes[id].get_start() > *max_tick) max_tick = notes[id].get_start();
                    }
                }

                if (!min_tick || !max_tick) return;

                const MIDITick fade_min_tick = *min_tick;
                const MIDITick fade_max_tick = *max_tick;
                auto tx = controller->begin_action("Fade Notes");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;
                        const auto& note = notes[id];

                        float vel_fac = 1.0f;
                        if (fade_max_tick != fade_min_tick) {
                            vel_fac = static_cast<float>(note.get_start() - fade_min_tick) /
                                static_cast<float>(fade_max_tick - fade_min_tick);
                        }
                        if (f.fade_out) vel_fac = 1.0f - vel_fac;

                        const std::uint8_t old_velocity = note.get_velocity();
                        const std::uint8_t new_velocity = std::clamp<std::uint8_t>(
                            util::saturating_cast<std::uint8_t>(static_cast<float>(old_velocity) * vel_fac), 1, 127);

                        if (new_velocity != old_velocity) {
                            notes_mutator.set_velocity(id, new_velocity);
                        }
                    }
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit FadeNotes transaction");
                }

            }
            else if constexpr (std::is_same_v<T, edit_fn::Transpose>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();
                auto tx = controller->begin_action("Transpose Notes");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;
                        const auto& note = notes[id];

                        const int new_key = std::clamp(static_cast<int>(note.get_key()) + static_cast<int>(f.amount), 0, 127);
                        if (new_key != note.get_key()) {
                            notes_mutator.set_key(id, static_cast<std::uint8_t>(new_key));
                        }
                    }
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit Transpose transaction");
                }

            }
            else if constexpr (std::is_same_v<T, edit_fn::RemoveOverlaps>) {
                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();
                auto tx = controller->begin_action("Remove Overlaps");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                    std::unordered_set<std::pair<MIDITick, std::uint8_t>, TickKeyHash> lookup;
                    std::vector<std::size_t> to_delete;

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;
                        const auto& note = notes[id];

                        std::pair<MIDITick, std::uint8_t> entry{ note.get_start(), note.get_key() };
                        if (lookup.contains(entry)) {
                            to_delete.push_back(id);
                        }
                        else {
                            lookup.insert(entry);
                        }
                    }

                    if (!to_delete.empty()) {
                        notes_mutator.delete_notes(to_delete);
                        tx.selection().remap(notes_mutator);
                    }
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit RemoveOverlaps transaction");
                }

            }
            else {
                static_assert(std::is_same_v<T, edit_fn::SetChannel>);

                auto& tracks = *controller->get_project_manager()->get_tracks();
                auto* selection = controller->get_selection();
                auto tx = controller->begin_action("Set Channel");

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!sel_note_ids || sel_note_ids->empty()) continue;

                    const auto& notes = tracks[track_id].get_notes();
                    auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                    for (const std::size_t id : *sel_note_ids) {
                        if (id >= notes.size()) continue;
                        if (notes[id].get_channel() != f.channel) {
                            notes_mutator.set_channel(id, f.channel);
                        }
                    }
                }

                if (!tx.commit()) {
                    Debugger::log_error("[EditFunctions] Failed to commit SetChannel transaction");
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
