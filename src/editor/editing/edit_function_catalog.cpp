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
#include "editor/timeline/snap_types_utils.h"

// refactor: catalog audit - what each registered command reads, and from where. the rule this
// records: a command may never ask the user for positional state the application already owns.
// ambient state (read from EditorController, never from a MainWindow pointer):
//   note.slice_at_tick  - transport position via EditorController::playhead_tick()
// user intent (declarative params rendered by the generic params dialog):
//   note.stretch        - factor
//   note.chop           - max_duration (grid), tick_length (0 = use grid)
//   note.glue           - threshold, separate_channels
//   note.fade           - fade_out
//   note.transpose      - amount
//   note.set_channel    - channel
//   note.quantize       - grid, strength
//   note.strum          - direction, duration, strength, length_preserve
// no input at all:
//   selection.filter_channels, note.remove_overlaps, note.slice_at_tick
// slice_at_tick appears under both headings on purpose: it takes no parameter and derives its
// cut position from the transport, which is the pattern every future position-dependent
// command should follow.

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

void modify_selection(EditorController& controller, std::string action_name,
    std::function<bool(history::NoteMutator&, std::uint16_t, const std::vector<std::size_t>&)>&& func) {

    auto& tracks = *controller.get_project_manager()->get_tracks();
    auto* selection = controller.get_selection();
    
    auto tx = controller.begin_action(action_name);

    std::vector<std::pair<std::uint16_t, history::NoteMutator>> note_mutators{};
    for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
        auto* note_ids = selection->get_selected_ids_in_track(track_id);
        if (!note_ids || note_ids->empty()) continue;

        auto notes_mutator = tx.notes(track_id, *note_ids);
        bool success = func(notes_mutator, track_id, *note_ids);
        if (success) note_mutators.push_back({ track_id, std::move(notes_mutator) });
    }

    auto selection_mutator = tx.selection();
    for (const auto& [m_track, mutator] : note_mutators) {
        const auto* sel_note_ids = selection->get_selected_ids_in_track(m_track);
        selection_mutator.set(m_track, mutator.remap_ids(*sel_note_ids));
    }

    if (!tx.commit()) {
        Debugger::log_error(std::format("Failed to commit action '{}'", action_name));
    }
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

            MIDITick stretch_anchor = std::numeric_limits<MIDITick>::max();
            
            // Pass 1: find stretch anchor
            {
                auto* selection = controller.get_selection();

                for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                    auto* note_ids = selection->get_selected_ids_in_track(track_id);
                    if (!note_ids || note_ids->empty()) continue;
                    MIDITick first_note_tick = tracks[track_id].get_notes()[note_ids->front()].get_start();
                    if (first_note_tick < stretch_anchor) stretch_anchor = first_note_tick;
                }

                if (stretch_anchor == std::numeric_limits<MIDITick>::max()) {
                    return;
                }
            }

            modify_selection(controller, "Stretch notes",
                [&](history::NoteMutator& mutator, std::uint16_t track, const std::vector<std::size_t>& note_ids) {
                    const auto& notes = tracks[track].get_notes();

                    for (const std::size_t id : note_ids) {
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

                        mutator.set_length(id, new_length < 1 ? 1 : new_length);
                        mutator.set_position(id, new_tick, note.get_key());
                    }

                    return true;
                });
        });
    }

    {
        auto& function_builder = registry.add_function({
            "note.chop", "Chop Selection", ""
            });

        function_builder.set_enabled(has_note_selection);

        const auto fallback_snap = timeline::TimelineGridSnap::QuarterNote;

        function_builder.add_param({
            .id = "max_duration", .label = "Grid",
            .type = EditParamType::Choice,
            .choices = timeline::get_grid_snap_strings() });

        function_builder.add_param({
            .id = "tick_length", .label = "Fixed tick length (0 = use grid)",
            .type = EditParamType::Integer, .default_int = 0,
            .min_int = 0 });

        function_builder.set_action([&](EditorController& controller, const EditFunctionArgs& args) {
            const std::string snap_str = args.get_choice("max_duration", timeline::grid_snap_to_string(fallback_snap));

            const auto snap = timeline::string_to_grid_snap(snap_str);

            const auto raw_tick_length = args.get_int("tick_length", 0);
            const bool use_fixed_length = raw_tick_length > 0;

            const auto fixed_tick_length = use_fixed_length ? static_cast<MIDITick>(raw_tick_length) : MIDITick{ 0 };

            auto* timeline_ptr = controller.get_project_manager()->get_timeline();

            if (!timeline_ptr) {
                Debugger::log_error("[EditFunctions] Chop requires a timeline");
                return;
            }

            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();

            auto tx = controller.begin_action("Chop Selection");

            using SelectionRemapEntry = std::tuple<std::uint16_t, history::NoteMutator, std::vector<history::AddedNote>>;
            std::vector<SelectionRemapEntry> selection_remap_table;

            for (std::size_t track_index = 0; track_index < tracks.size(); ++track_index) {
                const auto track_id = static_cast<std::uint16_t>(track_index);

                auto* note_ids = selection->get_selected_ids_in_track(track_id);
                if (!note_ids || note_ids->empty()) continue;

                const auto& notes = tracks[track_index].get_notes();
                auto notes_mutator = tx.notes(track_id, *note_ids);

                std::vector<history::AddedNote> added_notes;

                for (const std::size_t id : *note_ids) {
                    if (id >= notes.size()) continue;
                    const auto& note = notes[id];

                    // Capture properties before mutating note storage.
                    const MIDITick old_start = note.get_start();
                    const MIDITick old_length = note.get_length();
                    const auto key = note.get_key();
                    const auto velocity = note.get_velocity();
                    const auto channel = note.get_channel();

                    MIDITick remaining_length = old_length;
                    MIDITick start_time = old_start;

                    while (remaining_length > 0) {
                        // The grid step is evaluated at each segment's start,
                        // allowing the grid to adapt to time-signature changes.
                        const MIDITick grid_length = use_fixed_length ? fixed_tick_length : timeline_ptr->step_length_at(start_time, snap);

                        if (grid_length == 0) {
                            Debugger::log_error(
                                "[EditFunctions] Chop resolved to zero tick length"
                            );
                            return;
                        }

                        const MIDITick segment_length =
                            std::min(remaining_length, grid_length);

                        if (start_time == old_start) {
                            // Reuse the original note for the first segment.
                            if (segment_length != old_length) {
                                notes_mutator.set_length(id, segment_length);
                            }
                        } else {
                            added_notes.push_back(
                                notes_mutator.add_note(Note{
                                    start_time,
                                    segment_length,
                                    key,
                                    velocity,
                                    channel
                                    })
                            );
                        }

                        remaining_length -= segment_length;
                        start_time += segment_length;
                    }
                }

                selection_remap_table.push_back({ track_id, std::move(notes_mutator), std::move(added_notes) });
            }

            // Resolve added-note IDs before updating the selection.
            auto selection_mutator = tx.selection();

            for (const auto& [track_id, mutator, added_notes] : selection_remap_table) {
                auto* note_ids = selection->get_selected_ids_in_track(track_id);

                if (!note_ids) continue;

                std::vector<std::size_t> final_ids;
                final_ids.reserve(added_notes.size());

                for (const auto& added : added_notes)
                    final_ids.push_back(mutator.final_id(added));

                selection_mutator.set(track_id, mutator.remap_ids(*note_ids));
                selection_mutator.add(track_id, final_ids);
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

            modify_selection(controller, "Glue Notes",
                [&](history::NoteMutator& mutator, std::uint16_t track, const std::vector<std::size_t>& note_ids) {
                    const auto& notes = tracks[track].get_notes();

                    const std::size_t table_size = separate_channels ? 16 * 128 : 128;
                    std::vector<std::optional<std::size_t>> glue_table(table_size, std::nullopt);
                    std::vector<std::size_t> to_delete;

                    for (const std::size_t orig_id : note_ids) {
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
                                mutator.set_length(*last_orig_id, new_len);

                                // Mark current note for deletion since it's glued into the previous one
                                to_delete.push_back(orig_id);
                                continue;
                            }
                        }
                        glue_table[table_idx] = orig_id;
                    }

                    if (!to_delete.empty()) mutator.delete_notes(to_delete);

                    return true;
                });
        });
    }

    {
        // refactor: body moved from EditFunctions::apply_function (SliceAtTick branch). the
        // command used to slice at a hard-coded tick of 0, which made the straddle test below
        // reject every note, so the command was a no-op. the tick now comes from the transport
        // through EditorController, and the parameter the dialog used to ask for is gone.
        auto& function_builder = registry.add_function(
            { "note.slice_at_tick", "Split notes at Playhead", "" });
        function_builder.set_enabled(has_note_selection);

        function_builder.set_action([](EditorController& controller) {
            // sample once: every note in this command is cut at the same tick, so a running
            // transport cannot make the halves disagree with each other.
            const MIDITick slice_tick = controller.playhead_tick();
            if (slice_tick == 0) {
                // tick 0 is the start of the timeline and also what playhead_tick() returns when
                // no transport is installed: there is nothing to cut, so say so and do nothing
                // rather than writing zero-length halves.
                Debugger::log_warning(
                    "[EditFunctions] slice_at_tick: no playhead position, nothing sliced");
                return;
            }

            auto& tracks = *controller.get_project_manager()->get_tracks();

            modify_selection(controller, "Split notes at Playhead",
                [&](history::NoteMutator& mutator, std::uint16_t track, const std::vector<std::size_t>& note_ids) {
                    const auto& notes = tracks[track].get_notes();

                    for (const std::size_t id : note_ids) {
                        if (id >= notes.size()) continue;
                        const auto& note = notes[id];

                        const MIDITick start = note.get_start();
                        const MIDITick end = note.end();

                        // only notes the playhead passes through are cut: one that lies fully
                        // before or after it, or that merely touches it, would produce a
                        // zero-length half.
                        if (start >= slice_tick || end <= slice_tick) continue;

                        const MIDITick left_len = slice_tick - start;
                        const MIDITick right_len = end - slice_tick;

                        mutator.set_length(id, left_len);
                        mutator.add_note(Note{ slice_tick, right_len, note.get_key(), note.get_velocity(), note.get_channel() });
                    }
                    return true;
                });
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

            modify_selection(controller, "Remove overlaps",
                [&](history::NoteMutator& mutator, std::uint16_t track, const std::vector<std::size_t>& note_ids) {
                    const auto& notes = tracks[track].get_notes();

                    std::unordered_set<std::pair<MIDITick, std::uint8_t>, TickKeyHash> lookup;
                    std::vector<std::size_t> to_delete;

                    for (const std::size_t id : note_ids) {
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

                    if (!to_delete.empty())
                        mutator.delete_notes(to_delete);

                    return true;
                });
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

    {
        auto& function_builder = registry.add_function(
            { "note.quantize", "Quantize...", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "grid", .label = "Grid",
                                     .type = EditParamType::Choice,
                                     .choices = timeline::get_grid_snap_strings() });

        function_builder.add_param({ .id = "strength", .label = "Strength",
                                     .type = EditParamType::Decimal, .default_decimal = 1.0 });

        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            const timeline::TimelineGridSnap fallback_snap = timeline::TimelineGridSnap::QuarterNote;
            std::string snap_str = args.get_choice("grid", timeline::grid_snap_to_string(fallback_snap));
            timeline::TimelineGridSnap snap = timeline::string_to_grid_snap(snap_str);

            double strength = std::clamp(args.get_decimal("strength", 1.0), 0.0, 1.0);

            auto* timeline = controller.get_project_manager()->get_timeline();
            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();

            auto tx = controller.begin_action("Quantize notes");

            std::vector<std::pair<std::uint16_t, history::NoteMutator>> note_mutators{};

            for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                if (!sel_note_ids || sel_note_ids->empty()) continue;

                const auto& notes = tracks[track_id].get_notes();
                auto notes_mutator = tx.notes(track_id, *sel_note_ids);

                for (const std::size_t id : *sel_note_ids) {
                    if (id >= notes.size()) continue;
                    const auto& note = notes[id];
                    midi::MIDITick note_start = note.get_start();
                    midi::MIDITick note_end = note_start + note.get_length();

                    // probably Expensive:tm:, will optimize later.
                    midi::MIDITick quant_start = timeline->snap_nearest(note_start, snap);
                    midi::MIDITick quant_end = timeline->snap_nearest(note_end, snap);
                    
                    if (quant_end <= quant_start)
                        quant_end = timeline->snap_ceil(quant_start + 1, snap);

                    midi::MIDITick final_note_start = static_cast<midi::MIDITick>((1.0 - strength) * note_start
                        + strength * quant_start);
                    midi::MIDITick final_note_end = static_cast<midi::MIDITick>((1.0 - strength) * note_end
                        + strength * quant_end);

                    // fallback
                    if (final_note_end <= final_note_start) final_note_end = final_note_start + 1;

                    if (final_note_start != note_start)
                        notes_mutator.set_position(id, final_note_start, note.get_key());
                    if (final_note_end != note_end)
                        notes_mutator.set_length(id, final_note_end - final_note_start);
                }

                note_mutators.push_back({ track_id, std::move(notes_mutator) });
            }

            auto selection_mutator = tx.selection();

            for (const auto& [m_track, mutator] : note_mutators) {
                const auto* sel_note_ids = selection->get_selected_ids_in_track(m_track);
                selection_mutator.set(m_track, mutator.remap_ids(*sel_note_ids));
            }

            if (!tx.commit()) {
                Debugger::log_error("[EditFunctions] Failed to commit Quantize transaction");
            }
        });
    }

    {
        enum StrumDirection {
            Up,
            Down,
            Random
        };

        auto& function_builder = registry.add_function(
            { "note.strum", "Strum...", "" });
        function_builder.set_enabled(has_note_selection);
        function_builder.add_param({ .id = "direction", .label = "Direction",
                                     .type = EditParamType::Choice,
                                     .choices = { "Up", "Down", "Random" } });
        function_builder.add_param({ .id = "duration", .label = "Duration",
                                     .type = EditParamType::Choice,
                                     .choices = timeline::get_grid_snap_strings() });
        function_builder.add_param({ .id = "strength", .label = "Strength",
                                     .type = EditParamType::Decimal, .default_decimal = 1.0 });
        function_builder.add_param({ .id = "length_preserve", .label = "Preserve note lengths",
                                     .type = EditParamType::Boolean, .default_bool = false });

        function_builder.set_action([](EditorController& controller, const EditFunctionArgs& args) {
            std::string direction_str = args.get_choice("direction");
            timeline::TimelineGridSnap duration = timeline::string_to_grid_snap(args.get_choice("duration"));

            StrumDirection direction = Up;
            if (direction_str == "Up") direction = Up;
            else if (direction_str == "Down") direction = Down;
            else if (direction_str == "Random") direction = Random;

            double strength = std::clamp(args.get_decimal("strength", 1.0), 0.0, 1.0);
            bool preserve_length = args.get_bool("length_preserve");

            auto* timeline = controller.get_project_manager()->get_timeline();
            auto& tracks = *controller.get_project_manager()->get_tracks();
            auto* selection = controller.get_selection();

            auto tx = controller.begin_action("Strum notes");
            // TODO: have selection provide min/max keys and/or ticks functions

            std::vector<std::pair<std::uint16_t, history::NoteMutator>> note_mutators{};
            for (std::uint16_t track_id = 0; track_id < tracks.size(); track_id++) {
                auto* sel_note_ids = selection->get_selected_ids_in_track(track_id);
                if (!sel_note_ids || sel_note_ids->empty()) continue;

                const auto& notes = tracks[track_id].get_notes();

                std::uint8_t min_key = 0xFF;
                std::uint8_t max_key = 0;

                for (const std::size_t& id : *sel_note_ids) {
                    if (id >= notes.size()) continue;

                    const std::uint8_t key = notes[id].get_key();
                    if (key < min_key) min_key = key;
                    if (key > max_key) max_key = key;
                }

                if (max_key <= min_key) continue;

                auto notes_mutator = tx.notes(track_id, *sel_note_ids);
                for (const std::size_t& id : *sel_note_ids) {
                    if (id >= notes.size()) continue;

                    const auto& note = notes[id];
                    const midi::MIDITick note_start = note.get_start();
                    const midi::MIDITick note_length = note.get_length();
                    const std::uint8_t note_key = note.get_key();

                    const midi::MIDITick strum_length = timeline->step_length_at(note_start, duration);

                    double strum_factor = static_cast<double>(note_key - min_key) /
                            static_cast<double>(max_key - min_key);
                    if (direction == Down) strum_factor = 1.0 - strum_factor;
                    
                    // TODO: add random ordering

                    strum_factor *= strength;

                    const midi::MIDITick strum_move = static_cast<midi::MIDITick>(strum_length * strum_factor);
                    const midi::MIDITick new_start = note.get_start() + strum_move;
                    const midi::MIDITick new_length = preserve_length ? note_length
                        : std::max<midi::MIDITick>(1, note_length > strum_move ? note_length - strum_move : 1);

                    if (new_start != note_start) notes_mutator.set_position(id, new_start, note_key);
                    if (new_length != note_length) notes_mutator.set_length(id, new_length);
                }

                note_mutators.push_back({ track_id, std::move(notes_mutator) });
            }

            auto selection_mutator = tx.selection();

            for (const auto& [m_track, mutator] : note_mutators) {
                const auto* sel_note_ids = selection->get_selected_ids_in_track(m_track);
                selection_mutator.set(m_track, mutator.remap_ids(*sel_note_ids));
            }

            if (!tx.commit()) {
                Debugger::log_error("[EditFunctions] Failed to commit Strum transaction");
            }
        });
    }
}

} // namespace andromeda::editor
