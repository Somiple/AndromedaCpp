#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "editor/midi_types.h"
#include "midi/events/note.h"

namespace andromeda::editor::note_seq {

using midi::Note;

Note remove_note(std::vector<Note>& src, std::size_t id);

std::vector<Note> merge_notes(std::vector<Note> notes_1, std::vector<Note> notes_2);

std::pair<std::vector<Note>, std::vector<std::size_t>> merge_notes_and_return_ids(
    std::vector<Note> notes_1, std::vector<Note> notes_2);

template <typename T>
std::pair<std::vector<T>, std::vector<T>> extract(std::vector<T> src,
                                                  const std::vector<std::size_t>& ids) {
    std::vector<T> extracted;
    extracted.reserve(ids.size());
    std::vector<T> new_arr;
    new_arr.reserve(src.size() > ids.size() ? src.size() - ids.size() : 0);

    std::size_t ids_idx = 0;
    for (std::size_t id = 0; id < src.size(); ++id) {
        if (ids_idx < ids.size() && id == ids[ids_idx]) {
            extracted.push_back(std::move(src[id]));
            ids_idx += 1;
        } else {
            new_arr.push_back(std::move(src[id]));
        }
    }

    return {std::move(extracted), std::move(new_arr)};
}

template <typename T>
std::vector<T> exclude(std::vector<T> a, const std::vector<T>& b) {
    std::vector<T> result;

    std::size_t ai = 0;
    std::size_t bi = 0;

    while (ai < a.size()) {
        if (bi < b.size() && a[ai] > b[bi]) {
            bi += 1;
        } else if (bi < b.size() && a[ai] == b[bi]) {
            ai += 1;
            bi += 1;
        } else {
            result.push_back(std::move(a[ai]));
            ai += 1;
        }
    }

    return result;
}

template <typename T>
std::vector<T> merge_unique(std::vector<T> a, std::vector<T> b) {
    std::vector<T> result;
    result.reserve(a.size() + b.size());

    std::size_t ai = 0;
    std::size_t bi = 0;

    const auto push_unique = [&result](T value) {
        if (result.empty() || !(result.back() == value)) {
            result.push_back(std::move(value));
        }
    };

    while (ai < a.size() && bi < b.size()) {
        if (a[ai] < b[bi]) {
            push_unique(std::move(a[ai++]));
        } else if (a[ai] > b[bi]) {
            push_unique(std::move(b[bi++]));
        } else {
            bi += 1;
            push_unique(std::move(a[ai++]));
        }
    }

    while (ai < a.size()) {
        push_unique(std::move(a[ai++]));
    }
    while (bi < b.size()) {
        push_unique(std::move(b[bi++]));
    }

    return result;
}

template <typename T, typename U>
std::pair<std::vector<std::pair<T, U>>, std::vector<T>> extract_with(
    std::vector<T> src, const std::vector<std::size_t>& ids, std::vector<U> arr) {
    std::vector<std::pair<T, U>> extracted;
    extracted.reserve(ids.size());
    std::vector<T> new_arr;
    new_arr.reserve(src.size() > ids.size() ? src.size() - ids.size() : 0);

    std::size_t ids_idx = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (ids_idx < ids.size() && i == ids[ids_idx] && ids_idx < arr.size()) {
            extracted.emplace_back(std::move(src[i]), std::move(arr[ids_idx]));
            ids_idx += 1;
        } else {
            new_arr.push_back(std::move(src[i]));
        }
    }

    return {std::move(extracted), std::move(new_arr)};
}

template <typename T>
std::tuple<std::vector<T>, std::vector<T>, std::vector<std::size_t>> extract_and_remap_ids(
    std::vector<T> src, const std::vector<std::size_t>& ids,
    const std::vector<std::size_t>& ids_to_remap) {
    std::vector<T> extracted;
    extracted.reserve(ids.size());
    std::vector<T> new_arr;
    new_arr.reserve(src.size() > ids.size() ? src.size() - ids.size() : 0);
    std::vector<std::size_t> new_ids;
    new_ids.reserve(ids_to_remap.size());

    std::size_t extract_idx = 0;
    for (std::size_t id = 0; id < src.size(); ++id) {
        if (extract_idx < ids.size() && id == ids[extract_idx]) {
            extracted.push_back(std::move(src[id]));
            extract_idx += 1;
            continue;
        }
        new_arr.push_back(std::move(src[id]));
    }

    extract_idx = 0;
    for (const std::size_t id : ids_to_remap) {
        while (extract_idx < ids.size() && ids[extract_idx] < id) {
            extract_idx += 1;
        }
        if (extract_idx < ids.size() && id == ids[extract_idx]) {
            extract_idx += 1;
            continue;
        }
        new_ids.push_back(id - extract_idx);
    }

    return {std::move(extracted), std::move(new_arr), std::move(new_ids)};
}

std::vector<std::pair<Note, std::pair<SignedMIDITick, std::int16_t>>> move_each_note_by(
    std::vector<Note> notes_with_ids,
    const std::vector<std::pair<SignedMIDITick, std::int16_t>>& dt_pos);

std::vector<Note> move_all_notes_by(std::vector<Note> notes,
                                    std::pair<SignedMIDITick, std::int16_t> dt_pos);

}
