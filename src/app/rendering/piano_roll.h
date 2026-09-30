#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>

#include "app/rendering.h"
#include "app/rendering/buffers.h"
#include "app/rendering/note_cull_helper.h"
#include "app/rendering/note_gpu_cache.h"
#include "app/rendering/note_occlusion.h"
#include "app/rendering/screen_coverage.h"
#include "app/rendering/shaders.h"
#include "app/shared.h"
#include "app/view_settings.h"
#include "audio/audio_engine.h"
#include "editor/editing.h"
#include "editor/midi_bar_cacher.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::app::rendering {

inline constexpr std::size_t NOTE_BUFFER_SIZE = 8192;

using BarStart = float;
using BarLength = float;
using BarNumber = std::uint32_t;

struct RenderPianoRollBar {
    BarStart bar_start;
    BarLength bar_length;
    BarNumber bar_number;
};

using NoteRect = std::array<float, 4>;
// bits 0-3 colour, 4-11 velocity, 12 playing, 13 selected, 14-15 onion grey
using NoteMeta = std::uint32_t;

struct RenderPianoRollNote {
    NoteRect note_rect;
    NoteMeta note_meta;
};

using KBMeta0 = std::uint32_t;
using KBMeta1 = std::uint32_t;

struct RenderPianoRollKeyboard {
    KBMeta0 meta0;
    KBMeta1 meta1;
};

struct KeyboardMeta {
    bool pressed = false;
    bool is_black = false;
    std::uint8_t key = 0;
    std::uint8_t color_idx = 0;

    [[nodiscard]] std::uint32_t get_meta() const {
        std::uint32_t meta = static_cast<std::uint32_t>(key) & 0x7F;
        if (is_black) {
            meta |= 1u << 31;
        }
        if (pressed) {
            meta |= 1u << 30;
        }
        meta |= static_cast<std::uint32_t>(color_idx) << 7;
        return meta;
    }
};

using Position = std::array<float, 2>;

struct Vertex {
    Position position;
};

inline constexpr std::array<Vertex, 4> QUAD_VERTICES = {{
    {{0.0f, 0.0f}},
    {{1.0f, 0.0f}},
    {{1.0f, 1.0f}},
    {{0.0f, 1.0f}},
}};

inline constexpr std::array<std::uint32_t, 6> QUAD_INDICES = {0, 1, 2, 0, 2, 3};

class PianoRollRenderer : public Renderer {
public:
    PianoRollRenderer(const util::SharedPtr<editor::ProjectManager>& project_manager,
                      util::SharedMutPtr<ViewSettings> view_settings,
                      util::SharedPtr<editor::PianoRollNavigation> nav,
                      std::shared_ptr<audio::AudioEngine> playback_manager,
                      std::shared_ptr<editor::BarCacher> bar_cacher,
                      std::shared_ptr<NoteColors> colors,
                      std::shared_ptr<NoteCullHelper> note_cull_helper,
                      std::shared_ptr<editor::SharedSelectedNotes> shared_selected_notes);

    [[nodiscard]] std::shared_ptr<NoteGpuCache> note_cache() const { return note_gpu_cache_; }

    // call only with the note uploader stopped
    void drop_note_caches() {
        note_gpu_cache_->clear();
        note_occlusion_.clear();
    }

    ~PianoRollRenderer() override;

    void draw() override;

    void set_ghost_notes(util::SharedMutPtr<std::vector<midi::Note>> notes) override {
        ghost_notes = std::move(notes);
    }
    void clear_ghost_notes() override { ghost_notes.reset(); }
    void window_size(ImVec2 size) override { window_size_ = size; }
    void update_ppq(std::uint16_t new_ppq) override { ppq = new_ppq; }
    void set_selected(std::shared_ptr<editor::SharedSelectedNotes> selected_ids) override {
        selected_ = std::move(selected_ids);
    }
    void set_active(bool is_active) override { render_active_ = is_active; }
    void app_scale(float scale) override { keyboard_scale_ = scale; }
    [[nodiscard]] std::size_t instances_drawn() const override { return instances_drawn_; }

    [[nodiscard]] std::size_t resident_instances() const override { return resident_instances_; }
    [[nodiscard]] std::size_t cpu_instances() const override { return cpu_instances_; }
    [[nodiscard]] std::size_t residency_refusals() const override {
        return note_gpu_cache_->refused_full() + note_gpu_cache_->refused_budget();
    }
    [[nodiscard]] std::size_t resident_megabytes() const override {
        return note_gpu_cache_->resident_bytes() / (1024 * 1024);
    }
    std::size_t take_uploaded_notes() override { return note_gpu_cache_->take_uploaded_notes(); }

    [[nodiscard]] std::size_t bail_range_past_end() const override { return bail_range_past_end_; }
    [[nodiscard]] std::size_t bail_empty_range() const override { return bail_empty_range_; }
    [[nodiscard]] std::size_t resident_tracks() const override { return resident_tracks_; }
    [[nodiscard]] std::size_t cpu_tracks() const override { return cpu_tracks_; }

    [[nodiscard]] std::size_t coverage_tested() const override { return note_coverage_.tested(); }
    [[nodiscard]] std::size_t coverage_skipped() const override { return note_coverage_.skipped(); }

    util::SharedPtr<editor::PianoRollNavigation> navigation;
    std::shared_ptr<audio::AudioEngine> playback_manager;
    util::SharedMutPtr<ViewSettings> view_settings;
    std::shared_ptr<editor::BarCacher> bar_cacher;
    std::uint16_t ppq = 960;

    util::SharedMutPtr<std::vector<midi::Note>> ghost_notes;
    float keyboard_height;

private:
    [[nodiscard]] float get_time() const;

    std::size_t upload_note_indices(const std::vector<std::uint32_t>& ids);

    static bool is_black(std::size_t key) {
        const std::size_t k = key % 12;
        return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
    }

    ImVec2 window_size_{0.0f, 0.0f};

    ShaderProgram pr_program_;
    Buffer pr_vertex_buffer_;
    VertexArray pr_vertex_array_;
    Buffer pr_instance_buffer_;
    Buffer pr_index_buffer_;

    ShaderProgram pr_notes_program_;
    Buffer pr_notes_vbo_;
    VertexArray pr_notes_vao_;
    Buffer pr_notes_ibo_;
    Buffer pr_notes_ebo_;

    ShaderProgram pr_notes_direct_program_;
    VertexArray pr_notes_direct_vao_;
    std::shared_ptr<NoteGpuCache> note_gpu_cache_ = std::make_shared<NoteGpuCache>();
    NoteOcclusionStore note_occlusion_;

    ScreenCoverage note_coverage_;
    Buffer note_index_buffer_{GL_TEXTURE_BUFFER};
    GLuint note_index_tex_ = 0;
    std::size_t note_index_capacity_ = 0;
    std::size_t note_index_head_ = 0;

    ShaderProgram pr_keyboard_program_;
    Buffer pr_keyboard_vertex_buffer_;
    VertexArray pr_keyboard_vertex_array_;
    Buffer pr_keyboard_instance_buffer_;
    Buffer pr_keyboard_index_buffer_;

    std::vector<RenderPianoRollBar> bars_render_;
    std::vector<RenderPianoRollNote> notes_render_;
    std::array<RenderPianoRollKeyboard, 128> kb_render_{};

    util::SharedPtr<std::vector<midi::MIDITrack>> all_tracks_;
    std::shared_ptr<NoteColors> note_colors_;
    std::shared_ptr<NoteCullHelper> note_cull_helper_;

    float keyboard_scale_ = 1.0f;
    std::vector<std::size_t> key_ids_;
    std::array<KeyboardMeta, 128> key_metas_{};

    std::shared_ptr<editor::SharedSelectedNotes> selected_;
    bool render_active_ = false;

    std::size_t instances_drawn_ = 0;
    std::size_t resident_instances_ = 0;
    std::size_t cpu_instances_ = 0;
    std::size_t resident_tracks_ = 0;
    std::size_t cpu_tracks_ = 0;
    std::size_t bail_no_entry_ = 0;
    std::size_t bail_range_past_end_ = 0;
    std::size_t bail_empty_range_ = 0;

    GLuint overdraw_query_ = 0;
    GLuint64 overdraw_peak_ = 0;
};

}
