#include "editor/editing/edit_function_catalog.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

#include "editor/edit_function_registry.h"
#include "editor/editor_controller.h"
#include "midi/midi_track.h"
#include "core/debugger.h"
#include "core/numeric.h"

namespace andromeda::editor {

namespace {

using midi::Note;
using util::Debugger;

/// Hash for the (start tick, key) pair used by the overlap detector.
struct TickKeyHash {
    std::size_t operator()(const std::pair<MIDITick, std::uint8_t>& v) const noexcept {
        return (static_cast<std::size_t>(v.first) << 8) ^ v.second;
    }
};

/// Shared guard: every note-transforming command is only meaningful with a selection.
[[nodiscard]] bool has_note_selection(EditorController& controller) {
    return controller.get_selection()->is_any_note_selected();
}

} // namespace

void register_builtin_edit_functions(EditFunctionRegistry& registry) {
    // refactor: `selection.filter_channels` is registered first on purpose - it is the
    // simplest command, so the registry API is exercised end to end before the rest.
    {
        auto& function_builder = registry.add_function(
            { "selection.filter_channels", "Filter Selection Channels", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.set_action([](EditorController& controller) {
            controller.filter_selection_channels();
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (Stretch branch),
        // behaviour unchanged - same anchor search, same transaction label.
        auto& function_builder = registry.add_function({ "note.stretch", "Stretch Selection", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "factor", .label = "Stretch factor (x)",
                                     .type = EditParamType::Decimal, .default_decimal = 1.0 });
        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const double factor = args.get_decimal("factor", 1.0);
            if (factor == 1.0) {
                return;
            }

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();

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
            auto tx = controller.begin_action("Stretch Selection");

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
                        std::round(static_cast<float>(old_length) * static_cast<float>(factor)));
                    const auto new_tick =
                        util::saturating_cast<MIDITick>(
                            std::round(static_cast<float>(old_tick - stretch_anchor) *
                                       static_cast<float>(factor))) +
                        stretch_anchor;

                    notes_mutator.set_length(id, new_length < 1 ? 1 : new_length);
                    notes_mutator.set_position(id, new_tick, note.get_key());
                }
            }

            if (!tx.commit()) {
                Debugger::log_error("[EditFunctions] Failed to commit Stretch transaction");
            }
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (Chop branch),
        // behaviour unchanged. the selection is remapped after the mutators are created.
        auto& function_builder = registry.add_function({ "note.chop", "Chop Selection", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "tick_length", .label = "Chop tick length",
                                     .type = EditParamType::Integer, .default_int = 240,
                                     .min_int = 1 });
        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const auto max_tick_len = static_cast<MIDITick>(args.get_int("tick_length", 240));
            if (max_tick_len == 0) {
                Debugger::log_error("[EditFunctions] Chop with target length 0");
                return;
            }

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();
            auto tx = controller.begin_action("Chop Selection");

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

                    if (note.get_length() <= max_tick_len) {
                        surviving_ids.push_back(id);
                        continue;
                    }

                    const MIDITick old_length = note.get_length();
                    const MIDITick new_length = max_tick_len;

                    notes_mutator.set_length(id, new_length);
                    surviving_ids.push_back(id);

                    MIDITick remaining_length = old_length - new_length;
                    MIDITick start_time = note.get_start() + new_length;

                    while (remaining_length > 0) {
                        const MIDITick next_len = std::min(remaining_length, max_tick_len);
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
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (Glue branch),
        // behaviour unchanged. `separate_channels` used to be hardcoded false at the call
        // site and is now a declared parameter defaulting to true, matching the old dialog.
        auto& function_builder = registry.add_function({ "note.glue", "Glue Notes", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "threshold", .label = "Glue threshold (in ticks)",
                                     .type = EditParamType::Integer, .default_int = 0,
                                     .min_int = 0 });
        function_builder.add_param({ .id = "separate_channels", .label = "Keep channels separate",
                                     .type = EditParamType::Boolean, .default_bool = true });
        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const auto glue_threshold = static_cast<MIDITick>(args.get_int("threshold", 0));
            const bool separate_channels = args.get_bool("separate_channels", true);

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();
            auto tx = controller.begin_action("Glue Notes");

            for (std::uint16_t track_id = 0; track_id < tracks.size(); ++track_id) {
                auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                if (!sel_note_ids || sel_note_ids->empty()) continue;

                const auto& notes = tracks[track_id].get_notes();
                auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                const std::size_t table_size = separate_channels ? 16 * 128 : 128;
                std::vector<std::optional<std::size_t>> glue_table(table_size, std::nullopt);

                std::vector<std::size_t> to_delete;

                for (const std::size_t orig_id : *sel_note_ids) {
                    if (orig_id >= notes.size()) continue;
                    const Note& note = notes[orig_id];

                    const std::size_t key = note.get_key();
                    const std::size_t ch_index = separate_channels ? static_cast<std::size_t>(note.get_channel()) * 128 : 0;
                    const std::size_t table_idx = ch_index + key;

                    const MIDITick note_start = note.get_start();
                    const MIDITick note_end = note.end();

                    if (const auto last_orig_id = glue_table[table_idx]) {
                        // We compare against the original note's baseline start/end
                        const auto& last_note = notes[*last_orig_id];
                        const MIDITick last_start = last_note.get_start();
                        const MIDITick last_end = last_note.end();

                        if (note_start <= last_end + glue_threshold) {
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
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (SliceAtTick branch),
        // behaviour unchanged.
        auto& function_builder = registry.add_function(
            { "note.slice_at_tick", "Slice Notes at Playhead", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "tick", .label = "Slice tick",
                                     .type = EditParamType::Integer, .default_int = 0 });
        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const auto slice_tick = static_cast<MIDITick>(args.get_int("tick", 0));

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();
            auto tx = controller.begin_action("Slice at Tick");

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

                    if (start >= slice_tick || end <= slice_tick) continue;

                    const MIDITick left_len = slice_tick - start;
                    const MIDITick right_len = end - slice_tick;

                    if (left_len == 0 || right_len == 0) continue;

                    notes_mutator.set_length(id, left_len);
                    notes_mutator.add_note(Note{ slice_tick, right_len, note.get_key(), note.get_velocity(), note.get_channel() });
                }
            }

            if (!tx.commit()) {
                Debugger::log_error("[EditFunctions] Failed to commit SliceAtTick transaction");
            }
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (FadeNotes branch),
        // behaviour unchanged. fade_in and fade_out used to be two menu entries; they are
        // one command with a declared boolean now.
        auto& function_builder = registry.add_function({ "note.fade", "Fade Notes", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "fade_out", .label = "Fade out",
                                     .type = EditParamType::Boolean, .default_bool = false });
        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const bool fade_out = args.get_bool("fade_out", false);

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();

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
            auto tx = controller.begin_action("Fade Notes");

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
                    if (fade_out) vel_fac = 1.0f - vel_fac;

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
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (Transpose branch),
        // behaviour unchanged.
        auto& function_builder = registry.add_function({ "note.transpose", "Transpose Notes", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "amount", .label = "Semitones",
                                     .type = EditParamType::Integer, .default_int = 0,
                                     .min_int = -127, .max_int = 127 });
        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const int amount = args.get_int("amount", 0);

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();
            auto tx = controller.begin_action("Transpose Notes");

            for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                if (!sel_note_ids || sel_note_ids->empty()) continue;

                const auto& notes = tracks[track_id].get_notes();
                auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                for (const std::size_t id : *sel_note_ids) {
                    if (id >= notes.size()) continue;
                    const auto& note = notes[id];

                    const int new_key = std::clamp(static_cast<int>(note.get_key()) + amount, 0, 127);
                    if (new_key != note.get_key()) {
                        notes_mutator.set_key(id, static_cast<std::uint8_t>(new_key));
                    }
                }
            }

            if (!tx.commit()) {
                Debugger::log_error("[EditFunctions] Failed to commit Transpose transaction");
            }
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (RemoveOverlaps branch),
        // behaviour unchanged.
        auto& function_builder = registry.add_function(
            { "note.remove_overlaps", "Remove Overlaps", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.set_action([](EditorController& controller) {
            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();
            auto tx = controller.begin_action("Remove Overlaps");

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
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (SetChannel branch),
        // behaviour unchanged.
        auto& function_builder = registry.add_function(
            { "note.set_channel", "Set Channel of Notes", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "channel", .label = "New channel",
                                     .type = EditParamType::Integer, .default_int = 0,
                                     .min_int = 0, .max_int = 15 });
        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const auto channel = static_cast<std::uint8_t>(args.get_int("channel", 0));

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();
            auto tx = controller.begin_action("Set Channel");

            for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                if (!sel_note_ids || sel_note_ids->empty()) continue;

                const auto& notes = tracks[track_id].get_notes();
                auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                for (const std::size_t id : *sel_note_ids) {
                    if (id >= notes.size()) continue;
                    if (notes[id].get_channel() != channel) {
                        notes_mutator.set_channel(id, channel);
                    }
                }
            }

            if (!tx.commit()) {
                Debugger::log_error("[EditFunctions] Failed to commit SetChannel transaction");
            }
        });
    }
}

} // namespace andromeda::editor
