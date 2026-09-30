#include "editor/editing/note_editing/note_sequence_funcs.h"

namespace andromeda::editor::note_seq {

Note remove_note(std::vector<Note>& src, std::size_t id) {
    Note note = src[id];
    src.erase(src.begin() + static_cast<std::ptrdiff_t>(id));
    return note;
}

std::vector<Note> merge_notes(std::vector<Note> notes_1, std::vector<Note> notes_2) {
    std::vector<Note> merged;
    merged.reserve(notes_1.size() + notes_2.size());

    std::size_t i1 = 0;
    std::size_t i2 = 0;

    while (i1 < notes_1.size() && i2 < notes_2.size()) {
        if (notes_1[i1].get_start() <= notes_2[i2].get_start()) {
            merged.push_back(notes_1[i1++]);
        } else {
            merged.push_back(notes_2[i2++]);
        }
    }

    while (i1 < notes_1.size()) {
        merged.push_back(notes_1[i1++]);
    }
    while (i2 < notes_2.size()) {
        merged.push_back(notes_2[i2++]);
    }

    return merged;
}

std::pair<std::vector<Note>, std::vector<std::size_t>> merge_notes_and_return_ids(
    std::vector<Note> notes_1, std::vector<Note> notes_2) {
    std::vector<Note> merged;
    merged.reserve(notes_1.size() + notes_2.size());

    std::vector<std::size_t> ids;
    ids.reserve(notes_2.size());

    std::size_t i1 = 0;
    std::size_t i2 = 0;
    std::size_t write_idx = 0;

    while (i1 < notes_1.size() && i2 < notes_2.size()) {
        // strict < on purpose, unlike merge_notes: on a tie notes_2 wins and gets an id
        if (notes_1[i1].get_start() < notes_2[i2].get_start()) {
            merged.push_back(notes_1[i1++]);
        } else {
            ids.push_back(write_idx);
            merged.push_back(notes_2[i2++]);
        }
        write_idx += 1;
    }

    while (i1 < notes_1.size()) {
        merged.push_back(notes_1[i1++]);
    }

    while (i2 < notes_2.size()) {
        ids.push_back(write_idx);
        merged.push_back(notes_2[i2++]);
        write_idx += 1;
    }

    return {std::move(merged), std::move(ids)};
}

std::vector<std::pair<Note, std::pair<SignedMIDITick, std::int16_t>>> move_each_note_by(
    std::vector<Note> notes_with_ids,
    const std::vector<std::pair<SignedMIDITick, std::int16_t>>& dt_pos) {
    std::vector<std::pair<Note, std::pair<SignedMIDITick, std::int16_t>>> tmp;
    tmp.reserve(notes_with_ids.size());

    const std::size_t count = std::min(notes_with_ids.size(), dt_pos.size());

    for (std::size_t i = 0; i < count; ++i) {
        Note note = notes_with_ids[i];
        const auto [dt_tick, dt_key] = dt_pos[i];

        const auto orig_start = static_cast<SignedMIDITick>(note.get_start());
        const auto orig_key = static_cast<std::int16_t>(note.get_key());

        SignedMIDITick new_start = orig_start + dt_tick;
        if (new_start < 0) {
            new_start = 0;
        }

        auto new_key = static_cast<std::int16_t>(orig_key + dt_key);
        if (new_key < 0) {
            new_key = 0;
        } else if (new_key > 127) {
            new_key = 127;
        }

        note.start = static_cast<MIDITick>(new_start);
        note.key = static_cast<std::uint8_t>(new_key);

        tmp.emplace_back(note, std::pair<SignedMIDITick, std::int16_t>{
                                   new_start - orig_start,
                                   static_cast<std::int16_t>(new_key - orig_key)});
    }

    std::stable_sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) {
        return a.first.get_start() < b.first.get_start();
    });

    return tmp;
}

std::vector<Note> move_all_notes_by(std::vector<Note> notes,
                                    std::pair<SignedMIDITick, std::int16_t> dt_pos) {
    const auto [dt_tick, dt_key] = dt_pos;

    std::vector<Note> tmp;
    tmp.reserve(notes.size());

    for (Note note : notes) {
        const auto orig_start = static_cast<SignedMIDITick>(note.get_start());
        const auto orig_key = static_cast<std::int16_t>(note.get_key());

        SignedMIDITick new_start = orig_start + dt_tick;
        if (new_start < 0) {
            new_start = 0;
        }

        auto new_key = static_cast<std::int16_t>(orig_key + dt_key);
        if (new_key < 0) {
            new_key = 0;
        } else if (new_key > 127) {
            new_key = 127;
        }

        note.start = static_cast<MIDITick>(new_start);
        note.key = static_cast<std::uint8_t>(new_key);

        tmp.push_back(note);
    }

    std::stable_sort(tmp.begin(), tmp.end(),
                     [](const Note& a, const Note& b) { return a.get_start() < b.get_start(); });

    return tmp;
}

}
