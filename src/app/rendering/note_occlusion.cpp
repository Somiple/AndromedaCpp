#include "app/rendering/note_occlusion.h"

#include <algorithm>
#include <limits>
#include <shared_mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace andromeda::app::rendering {

namespace {

using editor::MIDITick;

constexpr MIDITick UNENDED = std::numeric_limits<MIDITick>::max();
constexpr std::size_t SLOT_COUNT = 256 * 16;

MIDITick note_end(const midi::Note& note) {
    if (note.length == UNENDED || note.length > UNENDED - note.start) {
        return UNENDED;
    }
    return note.start + note.length;
}

}

NoteOcclusionStore::Culled NoteOcclusionStore::cull(const std::vector<midi::Note>& src) {
    Culled out;
    out.notes.reserve(src.size());

    std::vector<MIDITick> reach(SLOT_COUNT, 0);
    std::vector<bool> seen(SLOT_COUNT, false);

    for (const midi::Note& note : src) {
        const std::size_t slot = (static_cast<std::size_t>(note.key) << 4) |
                                 static_cast<std::size_t>(note.channel & 0x0F);
        const MIDITick end = note_end(note);

        if (seen[slot] && end <= reach[slot]) {
            continue;
        }

        out.notes.push_back(note);
        reach[slot] = seen[slot] ? std::max(reach[slot], end) : end;
        seen[slot] = true;
    }

    for (const midi::Note& note : out.notes) {
        if (note.length != UNENDED) {
            out.max_length = std::max(out.max_length, note.length);
        }
    }

    out.notes.shrink_to_fit();
    return out;
}

void NoteOcclusionStore::attach(std::vector<midi::MIDITrack>* tracks) {
    if (worker_.joinable() || !tracks) {
        return;
    }
    tracks_ = std::move(tracks);
    worker_ = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

const NoteOcclusionStore::Culled* NoteOcclusionStore::get(std::size_t track,
                                                          std::uint64_t revision,
                                                          const std::vector<midi::Note>& notes) {
    if (!enabled_ || notes.empty() || !tracks_) {
        return nullptr;
    }

    std::lock_guard lock(mutex_);

    Entry& entry = entries_[track];
    if (entry.revision != revision) {
        cached_notes_ -= entry.culled.notes.size();
        entry.culled = Culled{};
        entry.state = State::Idle;
        entry.revision = revision;
    }

    switch (entry.state) {
    case State::Built:
        return entry.culled.notes.empty() ? nullptr : &entry.culled;
    case State::Queued:
    case State::NotWorthIt:
        return nullptr;
    case State::Idle:
        break;
    }

    if (cached_notes_ >= MAX_CACHED_NOTES) {
        return nullptr;
    }

    entry.state = State::Queued;
    queue_.push_back(Job{track, revision});
    wake_.notify_one();
    return nullptr;
}

void NoteOcclusionStore::run(std::stop_token stop) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    while (!stop.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
                return;
            }
            job = queue_.front();
            queue_.pop_front();
        }

        Culled built;
        std::size_t source_notes = 0;
        {
            std::vector<midi::MIDITrack>& tracks = *tracks_;
            if (job.track < tracks.size() && tracks[job.track].revision == job.revision) {
                const std::vector<midi::Note>& notes = tracks[job.track].get_notes();
                source_notes = notes.size();
                if (source_notes != 0) {
                    built = cull(notes);
                }
            }
        }

        std::lock_guard lock(mutex_);
        const auto it = entries_.find(job.track);
        if (it == entries_.end() || it->second.revision != job.revision ||
            it->second.state != State::Queued) {
            continue;
        }
        Entry& entry = it->second;

        if (source_notes == 0) {
            entry.state = State::Idle;
            continue;
        }

        const double kept =
            static_cast<double>(built.notes.size()) / static_cast<double>(source_notes);
        if (kept > KEEP_BELOW) {
            entry.state = State::NotWorthIt;
            continue;
        }

        cached_notes_ += built.notes.size();
        entry.culled = std::move(built);
        entry.state = State::Built;
    }
}

void NoteOcclusionStore::clear() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    entries_.clear();
    cached_notes_ = 0;
}

std::size_t NoteOcclusionStore::cached_notes() const {
    std::lock_guard lock(mutex_);
    return cached_notes_;
}

}
