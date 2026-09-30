#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <glad/glad.h>

#include "app/rendering/buffers.h"
#include "midi/midi_track.h"

namespace andromeda::app::rendering {

class NoteGpuCache {
public:
    static_assert(sizeof(midi::Note) == 12, "one note is one RGB32UI texel");

    struct Entry {
        std::size_t slab = 0;
        std::size_t base_note = 0;
        std::size_t alloc_notes = 0;
        std::size_t note_count = 0;
        std::size_t uploaded_notes = 0;
        std::uint64_t revision = 0;
        std::uint64_t last_used_frame = 0;

        Buffer selection{GL_TEXTURE_BUFFER};
        GLuint selection_tex = 0;
        std::uint64_t selection_version = 0;
        bool selection_any = false;

        std::list<std::size_t>::iterator lru_it{};
    };

    NoteGpuCache() = default;
    ~NoteGpuCache();

    static void set_enabled(bool enabled) { enabled_ = enabled; }
    [[nodiscard]] static bool enabled() { return enabled_; }

    NoteGpuCache(const NoteGpuCache&) = delete;
    NoteGpuCache& operator=(const NoteGpuCache&) = delete;

    void begin_frame() {
        std::lock_guard cache_lock(mutex_);
        ++frame_;
        uploaded_this_frame_ = 0;
        uploaded_entries_this_frame_ = 0;
        budget_exhausted_ = false;
    }

    Entry* ensure(std::size_t key, const std::vector<midi::Note>& notes,
                  std::uint64_t revision);

    enum class Prewarm {
        Written,
        AlreadyUp,
        Skipped,
        Full,
    };

    // uploader thread only, inside a background pass; never evict, frames may draw it
    Prewarm prewarm(std::size_t key, const std::vector<midi::Note>& notes, std::uint64_t revision);

    struct Written {
        std::size_t key = 0;
        std::uint64_t revision = 0;
    };

    void publish(std::vector<Written>& batch);

    void begin_background_pass();
    void end_background_pass();

    std::vector<std::size_t> take_wanted_tracks();

    void clear();

    [[nodiscard]] bool full() const;

    enum class Variant : std::size_t { Exact = 0, Culled = 1, FirstLodLevel = 2 };

    static std::size_t key_for(std::size_t track_index, Variant variant) {
        return track_index * 8 + static_cast<std::size_t>(variant);
    }

    static std::size_t key_for_lod(std::size_t track_index, std::size_t level) {
        return track_index * 8 + static_cast<std::size_t>(Variant::FirstLodLevel) + level - 1;
    }

    static std::size_t track_of(std::size_t key) { return key / 8; }
    static bool is_exact(std::size_t key) {
        return key % 8 == static_cast<std::size_t>(Variant::Exact);
    }

    void ensure_selection(Entry& entry, const std::vector<std::size_t>* ids,
                          std::uint64_t selection_version);

    [[nodiscard]] GLuint texture_of(const Entry& entry) const {
        return entry.slab < slabs_.size() ? slabs_[entry.slab].tex : 0;
    }

    [[nodiscard]] std::size_t slab_count() const { return slabs_.size(); }

    [[nodiscard]] std::size_t resident_bytes() const {
        return slab_notes_ * sizeof(midi::Note);
    }
    [[nodiscard]] std::size_t used_bytes() const { return resident_notes_ * sizeof(midi::Note); }
    [[nodiscard]] std::size_t resident_tracks() const { return entries_.size(); }

    [[nodiscard]] std::size_t refused_full() const { return refused_full_; }
    [[nodiscard]] std::size_t refused_budget() const { return refused_budget_; }
    [[nodiscard]] std::size_t resident_entries() const { return entries_.size(); }

    std::size_t take_uploaded_notes() {
        const std::size_t n = uploaded_notes_;
        uploaded_notes_ = 0;
        return n;
    }

private:
    struct Range {
        std::size_t begin = 0;
        std::size_t count = 0;
        std::uint64_t free_frame = 0;
    };

    struct Slab {
        Buffer buffer{GL_TEXTURE_BUFFER};
        GLuint tex = 0;
        std::size_t capacity = 0;
        std::vector<Range> free;
    };

    void evict_until_fits(std::size_t wanted_notes);
    void touch(Entry& entry, std::size_t key);
    void destroy(Entry& entry);

    bool allocate(Entry& entry, std::size_t count);
    void release(Entry& entry);
    [[nodiscard]] bool has_room_for(std::size_t count) const;
    [[nodiscard]] bool reusable(const Range& range) const;
    std::size_t add_slab(std::size_t capacity);

    bool upload_chunk(Entry& entry, const std::vector<midi::Note>& notes);

    static std::size_t budget_bytes();
    static std::size_t max_track_bytes() {
        return std::min<std::size_t>(budget_bytes() / 4, 192ull * 1024ull * 1024ull);
    }

    static constexpr std::size_t BUDGET_FALLBACK = 1024ull * 1024ull * 1024ull;
    static constexpr std::size_t BUDGET_MIN = 256ull * 1024ull * 1024ull;
    static constexpr std::size_t BUDGET_MAX = 6144ull * 1024ull * 1024ull;

    static constexpr std::size_t UPLOAD_CHUNK_NOTES = 512ull * 1024ull;

    static constexpr std::size_t SLAB_NOTES = (64ull * 1024ull * 1024ull) / sizeof(midi::Note);

    static constexpr std::size_t ALLOC_GRAIN = 1024;

    static constexpr std::uint64_t REUSE_DELAY_FRAMES = 2;

    static std::size_t upload_entries_per_frame();

    static std::size_t upload_budget_per_frame();

    mutable std::mutex mutex_;

    std::vector<Slab> slabs_;
    std::unordered_map<std::size_t, Entry> entries_;

    std::list<std::size_t> lru_;

    inline static bool enabled_ = true;

    bool budget_exhausted_ = false;

    bool background_pass_ = false;
    std::vector<std::size_t> wanted_tracks_;

    std::size_t slab_notes_ = 0;
    std::size_t resident_notes_ = 0;
    std::size_t uploaded_notes_ = 0;
    std::size_t refused_full_ = 0;
    std::size_t refused_budget_ = 0;
    std::size_t uploaded_this_frame_ = 0;
    std::size_t uploaded_entries_this_frame_ = 0;
    std::uint64_t frame_ = 0;
};

}
