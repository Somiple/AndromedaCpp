#include "app/rendering/note_gpu_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>

#include "util/debugger.h"

namespace andromeda::app::rendering {

NoteGpuCache::~NoteGpuCache() {
    for (auto& [track, entry] : entries_) {
        destroy(entry);
    }
    for (Slab& slab : slabs_) {
        if (slab.tex != 0) {
            glDeleteTextures(1, &slab.tex);
            slab.tex = 0;
        }
    }
}

void NoteGpuCache::destroy(Entry& entry) {
    if (entry.selection_tex != 0) {
        glDeleteTextures(1, &entry.selection_tex);
        entry.selection_tex = 0;
    }
    release(entry);
}

namespace {

constexpr GLenum GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX = 0x9049;
constexpr GLenum TEXTURE_FREE_MEMORY_ATI = 0x87FC;

std::size_t free_vram_kb() {
    while (glGetError() != GL_NO_ERROR) {
    }

    GLint nvidia = 0;
    glGetIntegerv(GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &nvidia);
    if (glGetError() == GL_NO_ERROR && nvidia > 0) {
        return static_cast<std::size_t>(nvidia);
    }

    std::array<GLint, 4> amd{};
    glGetIntegerv(TEXTURE_FREE_MEMORY_ATI, amd.data());
    if (glGetError() == GL_NO_ERROR && amd[0] > 0) {
        return static_cast<std::size_t>(amd[0]);
    }

    while (glGetError() != GL_NO_ERROR) {
    }
    return 0;
}

}

std::size_t NoteGpuCache::budget_bytes() {
    static const std::size_t bytes = [] {
        const std::size_t kb = free_vram_kb();
        if (kb == 0) {
            util::Debugger::log(std::format("Note cache budget: {} MB (no VRAM query available)",
                                            BUDGET_FALLBACK / (1024 * 1024)));
            return BUDGET_FALLBACK;
        }

        const std::size_t half = (kb * 1024ull) / 2ull;
        const std::size_t chosen = std::clamp(half, BUDGET_MIN, BUDGET_MAX);
        util::Debugger::log(std::format("Note cache budget: {} MB of {} MB free on the GPU",
                                        chosen / (1024 * 1024), (kb * 1024ull) / (1024 * 1024)));
        return chosen;
    }();
    return bytes;
}

namespace {

std::size_t max_note_texels() {
    static const std::size_t cap = [] {
        GLint texels = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texels);
        return texels > 0 ? static_cast<std::size_t>(texels) : 0;
    }();
    return cap;
}

}

std::size_t NoteGpuCache::upload_budget_per_frame() {
    static const std::size_t bytes = [] {
        if (const char* env = std::getenv("ANDROMEDA_UPLOAD_BUDGET_MB");
            env != nullptr && env[0] != 0) {
            const std::size_t mb = std::strtoull(env, nullptr, 10);
            if (mb > 0) {
                return mb * 1024ull * 1024ull;
            }
        }

        return 8ull * 1024ull * 1024ull;
    }();
    return bytes;
}

std::size_t NoteGpuCache::upload_entries_per_frame() {
    static const std::size_t entries = [] {
        if (const char* env = std::getenv("ANDROMEDA_UPLOAD_ENTRIES");
            env != nullptr && env[0] != 0) {
            const std::size_t n = std::strtoull(env, nullptr, 10);
            if (n > 0) {
                return n;
            }
        }

        return std::size_t{64};
    }();
    return entries;
}

std::size_t NoteGpuCache::add_slab(std::size_t capacity) {
    Slab slab;
    slab.capacity = capacity;
    slab.buffer.bind();
    glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(capacity * sizeof(midi::Note)),
                 nullptr, GL_STATIC_DRAW);

    glGenTextures(1, &slab.tex);
    glBindTexture(GL_TEXTURE_BUFFER, slab.tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGB32UI, slab.buffer.id());
    glBindTexture(GL_TEXTURE_BUFFER, 0);

    slab.free.push_back(Range{0, capacity});
    slabs_.push_back(std::move(slab));
    slab_notes_ += capacity;

    return slabs_.size() - 1;
}

bool NoteGpuCache::reusable(const Range& range) const {
    return range.free_frame == 0 || frame_ >= range.free_frame + REUSE_DELAY_FRAMES;
}

bool NoteGpuCache::has_room_for(std::size_t count) const {
    for (const Slab& slab : slabs_) {
        for (const Range& range : slab.free) {
            if (range.count >= count && reusable(range)) {
                return true;
            }
        }
    }
    return false;
}

bool NoteGpuCache::allocate(Entry& entry, std::size_t count) {
    const std::size_t want = ((count + ALLOC_GRAIN - 1) / ALLOC_GRAIN) * ALLOC_GRAIN;

    const auto take_from = [&](std::size_t index) {
        Slab& slab = slabs_[index];
        for (std::size_t i = 0; i < slab.free.size(); ++i) {
            if (slab.free[i].count < want || !reusable(slab.free[i])) {
                continue;
            }
            entry.slab = index;
            entry.base_note = slab.free[i].begin;
            if (slab.free[i].count == want) {
                slab.free.erase(slab.free.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                slab.free[i].begin += want;
                slab.free[i].count -= want;
            }
            entry.alloc_notes = want;
            return true;
        }
        return false;
    };

    for (std::size_t i = 0; i < slabs_.size(); ++i) {
        if (take_from(i)) {
            return true;
        }
    }

    const std::size_t capacity = std::max(SLAB_NOTES, want);
    if ((slab_notes_ + capacity) * sizeof(midi::Note) > budget_bytes()) {
        return false;
    }
    if (capacity > max_note_texels()) {
        return false;
    }

    return take_from(add_slab(capacity));
}

void NoteGpuCache::release(Entry& entry) {
    if (entry.alloc_notes == 0 || entry.slab >= slabs_.size()) {
        return;
    }

    Slab& slab = slabs_[entry.slab];
    const Range freed{entry.base_note, entry.alloc_notes, frame_};
    entry.alloc_notes = 0;

    const auto at = std::lower_bound(
        slab.free.begin(), slab.free.end(), freed.begin,
        [](const Range& range, std::size_t begin) { return range.begin < begin; });
    const auto inserted = slab.free.insert(at, freed);

    auto merged = inserted;
    if (merged + 1 != slab.free.end() && merged->begin + merged->count == (merged + 1)->begin) {
        merged->count += (merged + 1)->count;
        merged->free_frame = std::max(merged->free_frame, (merged + 1)->free_frame);
        slab.free.erase(merged + 1);
    }
    if (merged != slab.free.begin()) {
        auto prev = merged - 1;
        if (prev->begin + prev->count == merged->begin) {
            prev->count += merged->count;
            prev->free_frame = std::max(prev->free_frame, merged->free_frame);
            slab.free.erase(merged);
        }
    }
}

void NoteGpuCache::evict_until_fits(std::size_t wanted_notes) {
    const std::size_t want = ((wanted_notes + ALLOC_GRAIN - 1) / ALLOC_GRAIN) * ALLOC_GRAIN;
    const auto room = [&] {
        return has_room_for(want) ||
               (slab_notes_ + std::max(SLAB_NOTES, want)) * sizeof(midi::Note) <= budget_bytes();
    };

    while (!room() && !lru_.empty()) {
        const std::size_t victim_key = lru_.front();
        auto it = entries_.find(victim_key);
        if (it == entries_.end()) {
            lru_.pop_front();
            continue;
        }

        if (it->second.last_used_frame == frame_) {
            budget_exhausted_ = true;
            return;
        }

        resident_notes_ -= it->second.note_count;
        destroy(it->second);
        lru_.pop_front();
        entries_.erase(it);
    }
}

void NoteGpuCache::touch(Entry& entry, std::size_t key) {
    entry.last_used_frame = frame_;
    lru_.splice(lru_.end(), lru_, entry.lru_it);
    (void)key;
}

NoteGpuCache::Entry* NoteGpuCache::ensure(std::size_t key, const std::vector<midi::Note>& notes,
                                          std::uint64_t revision) {
    std::lock_guard cache_lock(mutex_);
    const std::size_t bytes = notes.size() * sizeof(midi::Note);

    if (!enabled_ || notes.empty() || bytes > max_track_bytes() ||
        notes.size() > max_note_texels()) {
        return nullptr;
    }

    if (background_pass_) {
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            Entry& entry = it->second;
            if (entry.revision == revision && entry.note_count == notes.size() &&
                entry.uploaded_notes >= entry.note_count) {
                touch(entry, key);
                return &entry;
            }
        }
        if (is_exact(key)) {
            wanted_tracks_.push_back(track_of(key));
        }
        refused_budget_ += 1;
        return nullptr;
    }

    const bool budget_spent = uploaded_this_frame_ >= upload_budget_per_frame() ||
                              uploaded_entries_this_frame_ >= upload_entries_per_frame();

    auto it = entries_.find(key);
    if (it != entries_.end()) {
        Entry& entry = it->second;
        touch(entry, key);

        const bool current = entry.revision == revision && entry.note_count == notes.size();
        if (current && entry.uploaded_notes >= entry.note_count) {
            return &entry;
        }

        if (budget_spent) {
            return nullptr;
        }

        if (!current) {
            resident_notes_ -= entry.note_count;
            release(entry);
            evict_until_fits(notes.size());
            if (!allocate(entry, notes.size())) {
                entry.note_count = 0;
                entry.uploaded_notes = 0;
                refused_full_ += 1;
                return nullptr;
            }
            entry.note_count = notes.size();
            entry.uploaded_notes = 0;
            entry.revision = revision;
            entry.selection_version = 0;
            resident_notes_ += notes.size();
        }

        return upload_chunk(entry, notes) ? &entry : nullptr;
    }

    if (budget_spent) {
        refused_budget_ += 1;
        return nullptr;
    }

    if (budget_exhausted_) {
        refused_full_ += 1;
        return nullptr;
    }

    evict_until_fits(notes.size());

    Entry& entry = entries_[key];
    if (!allocate(entry, notes.size())) {
        entries_.erase(key);
        refused_full_ += 1;
        return nullptr;
    }

    entry.lru_it = lru_.insert(lru_.end(), key);
    entry.note_count = notes.size();
    entry.uploaded_notes = 0;
    entry.revision = revision;
    entry.last_used_frame = frame_;
    entry.selection_version = 0;
    entry.selection_any = false;
    resident_notes_ += notes.size();

    return upload_chunk(entry, notes) ? &entry : nullptr;
}

bool NoteGpuCache::upload_chunk(Entry& entry, const std::vector<midi::Note>& notes) {
    const std::size_t budget_left =
        uploaded_this_frame_ >= upload_budget_per_frame()
            ? 0
            : (upload_budget_per_frame() - uploaded_this_frame_) / sizeof(midi::Note);
    const std::size_t remaining = entry.note_count - entry.uploaded_notes;
    const std::size_t run =
        std::min({remaining, UPLOAD_CHUNK_NOTES, std::max<std::size_t>(budget_left, 1)});

    const Slab& slab = slabs_[entry.slab];
    const std::size_t offset = (entry.base_note + entry.uploaded_notes) * sizeof(midi::Note);
    const std::size_t size = run * sizeof(midi::Note);

    slab.buffer.bind();
    void* mapped = glMapBufferRange(
        GL_TEXTURE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size),
        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);

    if (mapped != nullptr) {
        std::memcpy(mapped, notes.data() + entry.uploaded_notes, size);
        glUnmapBuffer(GL_TEXTURE_BUFFER);
    } else {
        slab.buffer.set_sub_data(offset,
                                 std::span<const midi::Note>(notes).subspan(entry.uploaded_notes,
                                                                            run));
    }

    entry.uploaded_notes += run;
    uploaded_notes_ += run;
    uploaded_this_frame_ += run * sizeof(midi::Note);
    uploaded_entries_this_frame_ += 1;

    return entry.uploaded_notes >= entry.note_count;
}

bool NoteGpuCache::full() const {
    std::lock_guard cache_lock(mutex_);
    return (slab_notes_ * sizeof(midi::Note)) >= budget_bytes() && !has_room_for(ALLOC_GRAIN);
}

NoteGpuCache::Prewarm NoteGpuCache::prewarm(std::size_t key, const std::vector<midi::Note>& notes,
                                             std::uint64_t revision) {
    const std::size_t bytes = notes.size() * sizeof(midi::Note);
    if (!enabled_ || notes.empty() || bytes > max_track_bytes() ||
        notes.size() > max_note_texels()) {
        return Prewarm::Skipped;
    }

    std::size_t base = 0;
    GLuint buffer_id = 0;
    {
        std::lock_guard cache_lock(mutex_);

        auto it = entries_.find(key);
        if (it != entries_.end()) {
            Entry& existing = it->second;
            if (existing.revision == revision && existing.note_count == notes.size()) {
                return Prewarm::AlreadyUp;
            }

            resident_notes_ -= existing.note_count;
            release(existing);
            if (!allocate(existing, notes.size())) {
                existing.note_count = 0;
                existing.uploaded_notes = 0;
                return Prewarm::Full;
            }

            existing.note_count = notes.size();
            existing.uploaded_notes = 0;
            existing.revision = revision;
            existing.selection_version = 0;
            resident_notes_ += notes.size();
            base = existing.base_note;
            buffer_id = slabs_[existing.slab].buffer.id();
        } else {
            Entry& fresh = entries_[key];
            if (!allocate(fresh, notes.size())) {
                entries_.erase(key);
                return Prewarm::Full;
            }

            fresh.lru_it = lru_.insert(lru_.begin(), key);
            fresh.note_count = notes.size();
            fresh.uploaded_notes = 0;
            fresh.revision = revision;
            fresh.last_used_frame = 0;
            fresh.selection_version = 0;
            fresh.selection_any = false;
            resident_notes_ += notes.size();
            base = fresh.base_note;
            buffer_id = slabs_[fresh.slab].buffer.id();
        }
    }

    glBindBuffer(GL_TEXTURE_BUFFER, buffer_id);
    for (std::size_t at = 0; at < notes.size(); at += UPLOAD_CHUNK_NOTES) {
        const std::size_t run = std::min(UPLOAD_CHUNK_NOTES, notes.size() - at);
        const std::size_t offset = (base + at) * sizeof(midi::Note);
        const std::size_t size = run * sizeof(midi::Note);

        void* mapped = glMapBufferRange(
            GL_TEXTURE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size),
            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (mapped != nullptr) {
            std::memcpy(mapped, notes.data() + at, size);
            glUnmapBuffer(GL_TEXTURE_BUFFER);
        } else {
            glBufferSubData(GL_TEXTURE_BUFFER, static_cast<GLintptr>(offset),
                            static_cast<GLsizeiptr>(size), notes.data() + at);
        }
    }

    return Prewarm::Written;
}

void NoteGpuCache::publish(std::vector<Written>& batch) {
    if (batch.empty()) {
        return;
    }

    // required: writes must finish before the frame context is told to draw them
    glFinish();

    std::lock_guard cache_lock(mutex_);
    for (const Written& written : batch) {
        auto it = entries_.find(written.key);
        if (it == entries_.end() || it->second.revision != written.revision) {
            continue;
        }
        it->second.uploaded_notes = it->second.note_count;
        uploaded_notes_ += it->second.note_count;
    }
    batch.clear();
}

void NoteGpuCache::begin_background_pass() {
    std::lock_guard cache_lock(mutex_);
    background_pass_ = true;
    wanted_tracks_.clear();
}

void NoteGpuCache::end_background_pass() {
    std::lock_guard cache_lock(mutex_);
    background_pass_ = false;
    wanted_tracks_.clear();
}

void NoteGpuCache::clear() {
    std::lock_guard cache_lock(mutex_);

    for (auto& [key, entry] : entries_) {
        if (entry.selection_tex != 0) {
            glDeleteTextures(1, &entry.selection_tex);
            entry.selection_tex = 0;
        }
    }
    entries_.clear();
    lru_.clear();

    for (Slab& slab : slabs_) {
        if (slab.tex != 0) {
            glDeleteTextures(1, &slab.tex);
            slab.tex = 0;
        }
    }
    slabs_.clear();

    slab_notes_ = 0;
    resident_notes_ = 0;
    budget_exhausted_ = false;
    wanted_tracks_.clear();
}

std::vector<std::size_t> NoteGpuCache::take_wanted_tracks() {
    std::vector<std::size_t> wanted;
    {
        std::lock_guard cache_lock(mutex_);
        wanted.swap(wanted_tracks_);
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    return wanted;
}

void NoteGpuCache::ensure_selection(Entry& entry, const std::vector<std::size_t>* ids,
                                    std::uint64_t selection_version) {
    if (entry.selection_version == selection_version) {
        return;
    }
    entry.selection_version = selection_version;

    const bool any = ids != nullptr && !ids->empty();
    entry.selection_any = any;
    if (!any) {
        return;
    }

    const std::size_t words = (entry.note_count + 31) / 32;
    std::vector<std::uint32_t> bits(words, 0u);

    for (const std::size_t id : *ids) {
        if (id < entry.note_count) {
            bits[id >> 5] |= 1u << (id & 31u);
        }
    }

    entry.selection.set_data(std::span<const std::uint32_t>(bits), GL_DYNAMIC_DRAW);

    if (entry.selection_tex == 0) {
        glGenTextures(1, &entry.selection_tex);
    }
    glBindTexture(GL_TEXTURE_BUFFER, entry.selection_tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, entry.selection.id());
    glBindTexture(GL_TEXTURE_BUFFER, 0);
}

}
