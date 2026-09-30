#include "editor/editing.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace andromeda::editor {

void SharedClipboard::move_notes_to_clipboard(std::vector<Note> notes, std::uint16_t track,
                                              bool clear_clipboard) {
    if (clear_clipboard) {
        notes_clipboard_map_.clear();
    }
    notes_clipboard_map_[track] = std::move(notes);
    is_empty = false;
}

void SharedClipboard::move_multi_notes_to_clipboard(std::vector<std::vector<Note>> notes,
                                                    const std::vector<std::uint16_t>& tracks) {
    notes_clipboard_map_.clear();

    const std::size_t count = std::min(notes.size(), tracks.size());
    for (std::size_t i = 0; i < count; ++i) {
        notes_clipboard_map_[tracks[i]] = std::move(notes[i]);
    }
    is_empty = false;
}

std::vector<std::pair<std::uint16_t, std::vector<Note>>>
SharedClipboard::get_notes_from_clipboard() const {
    std::vector<std::pair<std::uint16_t, std::vector<Note>>> retrieved;
    retrieved.reserve(notes_clipboard_map_.size());

    for (const auto& [track, notes] : notes_clipboard_map_) {
        retrieved.emplace_back(track, notes);
    }

    std::sort(retrieved.begin(), retrieved.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return retrieved;
}

void SharedClipboard::clear_clipboard() {
    notes_clipboard_map_.clear();
    is_empty = true;
}

MIDITick SharedClipboard::get_clipboard_start_tick() const {
    MIDITick start_tick = std::numeric_limits<MIDITick>::max();

    for (const auto& [track, notes] : notes_clipboard_map_) {
        // fixed rust bug: an empty track here was read with front() (rust panicked)
        if (notes.empty()) {
            continue;
        }
        const MIDITick first_note_tick = notes.front().get_start();
        if (first_note_tick <= start_tick) {
            start_tick = first_note_tick;
        }
    }

    return start_tick;
}

std::vector<std::uint16_t> SharedSelectedNotes::get_active_selected_tracks() const {
    std::vector<std::uint16_t> tracks;
    tracks.reserve(selected_notes_hash_.size());
    for (const auto& [track, ids] : selected_notes_hash_) {
        tracks.push_back(track);
    }
    std::sort(tracks.begin(), tracks.end());
    return tracks;
}

const std::vector<std::size_t>* SharedSelectedNotes::get_selected_ids_in_track(
    std::uint16_t track) const {
    const auto it = selected_notes_hash_.find(track);
    return it == selected_notes_hash_.end() ? nullptr : &it->second;
}

std::vector<std::size_t>& SharedSelectedNotes::get_selected_ids_mut(std::uint16_t track) {
    bump();
    return selected_notes_hash_[track];
}

std::vector<std::pair<std::uint16_t, const std::vector<std::size_t>*>>
SharedSelectedNotes::get_selected() const {
    std::vector<std::pair<std::uint16_t, const std::vector<std::size_t>*>> retrieved;
    retrieved.reserve(selected_notes_hash_.size());

    for (const auto& [track, note_ids] : selected_notes_hash_) {
        retrieved.emplace_back(track, &note_ids);
    }

    return retrieved;
}

void SharedSelectedNotes::set_selected_in_track(std::vector<std::size_t> ids, std::uint16_t track) {
    bump();
    selected_notes_hash_[track] = std::move(ids);
}

void SharedSelectedNotes::add_selected_to_track(const std::vector<std::size_t>& ids,
                                                std::uint16_t track) {
    bump();
    std::vector<std::size_t> selected = take_selected_from_track(track);
    selected.insert(selected.end(), ids.begin(), ids.end());
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    selected_notes_hash_[track] = std::move(selected);
}

std::vector<std::size_t> SharedSelectedNotes::take_selected_from_track(std::uint16_t track) {
    bump();
    const auto it = selected_notes_hash_.find(track);
    if (it == selected_notes_hash_.end()) {
        return {};
    }
    std::vector<std::size_t> out = std::move(it->second);
    selected_notes_hash_.erase(it);
    return out;
}

std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>>
SharedSelectedNotes::take_selected_from_all() {
    bump();
    std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> result;
    result.reserve(selected_notes_hash_.size());

    for (auto& [track, ids] : selected_notes_hash_) {
        result.emplace_back(track, std::move(ids));
    }
    selected_notes_hash_.clear();

    return result;
}

void SharedSelectedNotes::clear_selected() {
    bump();
    selected_notes_hash_.clear();
}

bool SharedSelectedNotes::is_any_note_selected() const {
    if (selected_notes_hash_.empty()) {
        return false;
    }

    for (const auto& [track, ids] : selected_notes_hash_) {
        if (!ids.empty()) {
            return true;
        }
    }
    return false;
}

bool SharedSelectedNotes::selected_ids_in_track_contains(std::uint16_t track,
                                                         std::size_t id) const {
    const std::vector<std::size_t>* ids = get_selected_ids_in_track(track);
    if (ids == nullptr) {
        return false;
    }
    return std::find(ids->begin(), ids->end(), id) != ids->end();
}

std::size_t SharedSelectedNotes::get_num_selected_in_track(std::uint16_t track) const {
    const std::vector<std::size_t>* ids = get_selected_ids_in_track(track);
    return ids == nullptr ? 0 : ids->size();
}

}
