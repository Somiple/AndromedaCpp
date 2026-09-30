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
#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "midi/midi_track.h"
#include "util/debugger.h"
#include "util/numeric.h"

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

void EditFunctions::apply_function(std::vector<Note>& notes,
                                   std::vector<std::size_t>& sel_note_ids, EditFunction func,
                                   std::uint16_t curr_track, EditorActions& editor_actions) {
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

                auto [notes_to_stretch, remaining_notes] = extract(std::move(notes), f.note_ids);
                // fixed rust bug: indexed [0] of an empty selection
                if (notes_to_stretch.empty()) {
                    notes = std::move(remaining_notes);
                    Debugger::log_warning("[EditFunctions] Stretch with an empty selection");
                    return;
                }

                const MIDITick first_tick = notes_to_stretch[0].get_start();

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
                            std::round(static_cast<float>(old_tick - first_tick) * f.factor)) +
                        first_tick;

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

                std::vector<EditorAction> bulk;
                bulk.push_back(
                    LengthChange{std::move(f.note_ids), std::move(length_change), curr_track});
                bulk.push_back(NotesMove{std::move(new_ids), std::move(pos_change), curr_track,
                                         true});
                editor_actions.register_action(Bulk{std::move(bulk)});
            } else if constexpr (std::is_same_v<T, edit_fn::Chop>) {
                std::vector<SignedMIDITick> changed_lengths;
                changed_lengths.reserve(f.note_ids.size());
                std::vector<std::size_t> changed_lengths_ids;

                std::vector<Note> new_notes;

                for (const std::size_t id : f.note_ids) {
                    if (id >= notes.size()) {
                        continue;
                    }
                    Note& note = notes[id];
                    MIDITick remaining_length = note.get_length();

                    if (note.get_length() > f.max_tick_len) {
                        const MIDITick old_length = note.get_length();
                        const MIDITick new_length = f.max_tick_len;
                        note.set_length(new_length);
                        remaining_length -= new_length;

                        changed_lengths.push_back(static_cast<SignedMIDITick>(new_length) -
                                                  static_cast<SignedMIDITick>(old_length));
                        changed_lengths_ids.push_back(id);

                        MIDITick start_time = note.get_start() + new_length;

                        while (remaining_length > 0) {
                            const MIDITick next_len = std::min(remaining_length, f.max_tick_len);
                            new_notes.push_back(Note{start_time, next_len, note.get_key(),
                                                     note.get_velocity(), note.get_channel()});

                            remaining_length -= next_len;
                            start_time += next_len;
                        }
                    }
                }

                std::stable_sort(
                    new_notes.begin(), new_notes.end(),
                    [](const Note& a, const Note& b) { return a.get_start() < b.get_start(); });

                auto [merged, chopped_ids] =
                    merge_notes_and_return_ids(std::move(notes), std::move(new_notes));
                notes = std::move(merged);

                if (!chopped_ids.empty() && !f.note_ids.empty() && !changed_lengths.empty()) {
                    std::vector<EditorAction> bulk;
                    bulk.push_back(LengthChange{std::move(changed_lengths_ids),
                                                std::move(changed_lengths), curr_track});
                    bulk.push_back(PlaceNotes{std::move(chopped_ids), std::nullopt, curr_track});
                    editor_actions.register_action(Bulk{std::move(bulk)});
                }
            } else if constexpr (std::is_same_v<T, edit_fn::Glue>) {
                if (sel_note_ids.empty()) {
                    return;
                }

                auto [notes_to_glue, remaining_notes] = extract(std::move(notes), sel_note_ids);

                const std::size_t table_size = f.separate_channels ? 16 * 128 : 128;
                std::vector<std::optional<std::size_t>> glue_table(table_size, std::nullopt);

                std::vector<Note> kept_notes;
                std::vector<std::size_t> kept_original_ids;
                std::vector<MIDITick> kept_original_lengths;

                std::vector<Note> removed_notes;
                std::vector<std::size_t> removed_ids;

                const std::size_t pair_count = std::min(notes_to_glue.size(), f.note_ids.size());
                for (std::size_t i = 0; i < pair_count; ++i) {
                    const Note note = notes_to_glue[i];
                    const std::size_t orig_id = f.note_ids[i];

                    const std::size_t key = note.get_key();
                    const std::size_t ch_index =
                        f.separate_channels ? static_cast<std::size_t>(note.get_channel()) * 128 : 0;
                    const std::size_t table_idx = ch_index + key;

                    const MIDITick note_start = note.get_start();
                    const MIDITick note_end = note.end();

                    if (const auto last_idx = glue_table[table_idx]) {
                        Note& last_note = kept_notes[*last_idx];
                        const MIDITick last_start = last_note.get_start();
                        const MIDITick last_end = last_note.end();

                        if (note_start <= last_end + f.glue_threshold) {
                            last_note.set_length(std::max(last_end, note_end) - last_start);

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

                auto [merged, new_ids] =
                    merge_notes_and_return_ids(std::move(remaining_notes), std::move(kept_notes));
                notes = std::move(merged);

                std::vector<SignedMIDITick> length_deltas;
                length_deltas.reserve(kept_original_ids.size());
                for (std::size_t i = 0; i < kept_original_lengths.size() && i < new_ids.size();
                     ++i) {
                    const MIDITick final_len = notes[new_ids[i]].get_length();
                    length_deltas.push_back(static_cast<SignedMIDITick>(final_len) -
                                            static_cast<SignedMIDITick>(kept_original_lengths[i]));
                }

                sel_note_ids = exclude(std::move(sel_note_ids), removed_ids);

                std::vector<EditorAction> bulk;

                if (!removed_ids.empty()) {
                    bulk.push_back(Deselect{removed_ids, curr_track});
                }

                if (std::any_of(length_deltas.begin(), length_deltas.end(),
                                [](SignedMIDITick d) { return d != 0; })) {
                    bulk.push_back(
                        LengthChange{std::move(new_ids), std::move(length_deltas), curr_track});
                }

                if (!removed_ids.empty()) {
                    bulk.push_back(
                        DeleteNotes{std::move(removed_ids), std::move(removed_notes), curr_track});
                }

                if (!bulk.empty()) {
                    editor_actions.register_action(Bulk{std::move(bulk)});
                }
            } else if constexpr (std::is_same_v<T, edit_fn::SliceAtTick>) {
                std::vector<SignedMIDITick> changed_lengths;
                changed_lengths.reserve(sel_note_ids.size());
                std::vector<Note> new_notes;
                new_notes.reserve(sel_note_ids.size());

                std::vector<std::size_t> affected_ids;
                affected_ids.reserve(f.note_ids.size());

                for (const std::size_t id : f.note_ids) {
                    if (id >= notes.size()) {
                        continue;
                    }
                    Note& note = notes[id];
                    const MIDITick start = note.get_start();
                    const MIDITick end = note.end();

                    if (start < f.slice_tick && end > f.slice_tick) {
                        const MIDITick left_len = f.slice_tick - start;
                        const MIDITick right_len = end - f.slice_tick;
                        if (left_len == 0 || right_len == 0) {
                            continue;
                        }

                        const MIDITick old_length = note.get_length();
                        const MIDITick new_length = left_len;
                        note.set_length(new_length);
                        changed_lengths.push_back(static_cast<SignedMIDITick>(new_length) -
                                                  static_cast<SignedMIDITick>(old_length));

                        new_notes.push_back(Note{f.slice_tick, right_len, note.get_key(),
                                                 note.get_velocity(), note.get_channel()});
                        affected_ids.push_back(id);
                    }
                }

                auto [merged, new_ids] =
                    merge_notes_and_return_ids(std::move(notes), std::move(new_notes));
                notes = std::move(merged);

                if (!changed_lengths.empty()) {
                    std::vector<EditorAction> bulk;
                    bulk.push_back(PlaceNotes{std::move(new_ids), std::nullopt, curr_track});
                    bulk.push_back(LengthChange{std::move(affected_ids), std::move(changed_lengths),
                                                curr_track});
                    editor_actions.register_action(Bulk{std::move(bulk)});
                }
            } else if constexpr (std::is_same_v<T, edit_fn::FadeNotes>) {
                const auto range = get_min_max_ticks_in_selection(notes, sel_note_ids);
                // fixed rust bug: unwrapped the range of an empty selection
                if (!range) {
                    Debugger::log_warning("[EditFunctions] Fade with an empty selection");
                    return;
                }
                const auto [min_tick, max_tick] = *range;

                std::vector<std::int8_t> vel_changes;
                vel_changes.reserve(sel_note_ids.size());

                for (const std::size_t id : sel_note_ids) {
                    if (id >= notes.size()) {
                        continue;
                    }
                    Note& note = notes[id];

                    float vel_fac = static_cast<float>(note.get_start() - min_tick) /
                                    (static_cast<float>(max_tick) - static_cast<float>(min_tick));
                    if (f.fade_out) {
                        vel_fac = 1.0f - vel_fac;
                    }

                    const std::uint8_t old_velocity = note.get_velocity();
                    const std::uint8_t new_velocity = std::clamp<std::uint8_t>(
                        util::saturating_cast<std::uint8_t>(static_cast<float>(old_velocity) *
                                                            vel_fac),
                        1, 127);
                    vel_changes.push_back(static_cast<std::int8_t>(
                        static_cast<int>(new_velocity) - static_cast<int>(old_velocity)));

                    note.set_velocity(new_velocity);
                }

                editor_actions.register_action(
                    VelocityChange{sel_note_ids, std::move(vel_changes), curr_track});
            } else if constexpr (std::is_same_v<T, edit_fn::Transpose>) {
                std::vector<SignedMIDIKey> key_changes;
                key_changes.reserve(sel_note_ids.size());
                std::vector<std::size_t> affected_note_ids;

                for (const std::size_t id : sel_note_ids) {
                    if (id >= notes.size()) {
                        continue;
                    }
                    Note& note = notes[id];
                    const std::uint8_t old_key = note.get_key();

                    // fixed rust bug: the i8 addition wrapped, so keys pushed past 127 landed on 0
                    const int new_key =
                        std::clamp(static_cast<int>(old_key) + static_cast<int>(f.amount), 0, 127);

                    const auto key_change =
                        static_cast<SignedMIDIKey>(new_key - static_cast<int>(old_key));
                    if (key_change == 0) {
                        continue;
                    }

                    affected_note_ids.push_back(id);
                    key_changes.push_back(key_change);
                    note.set_key(static_cast<MIDIKey>(new_key));
                }

                if (!key_changes.empty()) {
                    editor_actions.register_action(
                        KeyChange{std::move(affected_note_ids), std::move(key_changes), curr_track});
                }
            } else if constexpr (std::is_same_v<T, edit_fn::RemoveOverlaps>) {
                if (sel_note_ids.empty()) {
                    return;
                }

                auto [extracted_notes, new_notes] = extract(std::move(notes), sel_note_ids);
                std::unordered_set<std::pair<MIDITick, std::uint8_t>, TickKeyHash> lookup;
                lookup.reserve(extracted_notes.size());

                std::vector<Note> kept_notes;
                kept_notes.reserve(extracted_notes.size());
                std::vector<std::size_t> removed_indices;
                std::vector<Note> removed_notes;

                for (std::size_t idx = 0; idx < extracted_notes.size(); ++idx) {
                    const Note& note = extracted_notes[idx];
                    const std::pair<MIDITick, std::uint8_t> entry{note.get_start(), note.get_key()};

                    if (lookup.contains(entry)) {
                        removed_notes.push_back(note);
                        removed_indices.push_back(idx);
                    } else {
                        lookup.insert(entry);
                        kept_notes.push_back(note);
                    }
                }

                notes = merge_notes(std::move(new_notes), std::move(kept_notes));

                if (removed_notes.empty()) {
                    Debugger::log("No overlapped notes were removed.");
                    return;
                }

                Debugger::log(std::format("Removed {} notes.", removed_indices.size()));

                editor_actions.register_action(
                    DeleteNotes{std::move(removed_indices), std::move(removed_notes), curr_track});
            } else {
                static_assert(std::is_same_v<T, edit_fn::SetChannel>);
                if (sel_note_ids.empty()) {
                    return;
                }

                std::vector<std::int8_t> channel_changes;
                channel_changes.reserve(sel_note_ids.size());

                for (const std::size_t id : sel_note_ids) {
                    if (id >= notes.size()) {
                        continue;
                    }
                    Note& note = notes[id];
                    const std::uint8_t old_channel = note.get_channel();
                    channel_changes.push_back(static_cast<std::int8_t>(
                        static_cast<int>(f.channel) - static_cast<int>(old_channel)));
                    note.channel = f.channel;
                }

                editor_actions.register_action(
                    ChannelChange{sel_note_ids, std::move(channel_changes), curr_track});

                Debugger::log(std::format("Changed channels in selection to channel {}", f.channel));
            }
        },
        std::move(func));
}

void EFDialogBase::apply(
    const std::function<EditFunction(const std::vector<std::size_t>&)>& make_func) {
    if (!note_editing_ || !edit_functions_ || !edit_actions_) {
        return;
    }

    const auto tracks = note_editing_->get_tracks();
    const auto shared_selected = note_editing_->get_shared_selected_ids();
    if (!tracks || !shared_selected) {
        return;
    }

    const std::uint16_t curr_track = note_editing_->get_current_track();

    std::unique_lock lock(tracks->mutex);
    if (curr_track >= tracks->value.size()) {
        return;
    }

    std::vector<Note>& notes = tracks->value[curr_track].get_notes_mut();
    std::vector<std::size_t>& sel_notes = shared_selected->get_selected_ids_mut(curr_track);

    edit_functions_->apply_function(notes, sel_notes, make_func(sel_notes), curr_track,
                                    *edit_actions_);
}

app::MaybeDlgAction EFStretchDialog::draw(const app::ImageResources&) {
    stretch_factor.show("Stretch factor (x):");
    return std::nullopt;
}

std::optional<app::DialogActionButtons> EFStretchDialog::get_action_buttons() const {
    return app::DlgOk{[](app::Dialog& dlg) -> app::MaybeDlgAction {
        auto& self = static_cast<EFStretchDialog&>(dlg);
        self.apply([&self](const std::vector<std::size_t>& sel) -> EditFunction {
            return edit_fn::Stretch{sel, self.stretch_factor.value()};
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
                                self.apply([&self](const std::vector<std::size_t>& sel)
                                               -> EditFunction {
                                    return edit_fn::Chop{sel, self.target_tick_len.value()};
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
            self.apply([&self](const std::vector<std::size_t>& sel) -> EditFunction {
                return edit_fn::Glue{sel, self.glue_threshold.value(), self.separate_channels};
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
                                self.apply([&self](const std::vector<std::size_t>&) -> EditFunction {
                                    return edit_fn::SetChannel{self.new_channel.value()};
                                });
                                return app::DialogClose{self.get_dialog_name()};
                            },
                            app::dialog_default_close_action()};
}

}
