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
#include "app/rendering/shaders.h"
#include "app/shared.h"
#include "app/view_settings.h"
#include "audio/audio_engine.h"
#include "editor/editing.h"
#include "editor/midi_bar_cacher.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "midi/events/channel_event.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::app::rendering {

inline constexpr std::size_t HANDLE_BUFFER_SIZE = 2048;
inline constexpr std::size_t DV_BAR_BUFFER_SIZE = 32;

struct RenderDataViewBar {
    float bar_start;
    float bar_length;
    std::uint32_t bar_number;
};

struct RenderDataViewHandle {
    std::array<float, 4> handle_rect;
    std::uint32_t handle_meta;
};

inline constexpr std::array<std::uint32_t, 6> DV_QUAD_INDICES = {0, 1, 3, 1, 2, 3};

class DataViewRenderer : public Renderer, public AppEventListener {
public:
    // TODO: fix this function cuz wtf
    DataViewRenderer(app::MainWindow* app);

    ~DataViewRenderer() override;

    void draw() override;

    void use_note_cache(std::shared_ptr<NoteGpuCache> cache) {
        note_gpu_cache_ = std::move(cache);
    }

    // don't need to manually change ppq here now, controller's project manager owns the ppq
    void on_event(const AndromedaEvent& event) override {
        (void)event;
    }

    void window_size(ImVec2 size) override { window_size_ = size; }
    void set_active(bool) override {}
    void set_ghost_notes(std::vector<midi::Note>* notes) override {
        ghost_notes = notes;
    }
    void clear_ghost_notes() override { ghost_notes = nullptr; }

    std::vector<midi::Note>* ghost_notes = nullptr;

private:
    [[nodiscard]] float get_time() const;

    void draw_note_velocities(float tick_pos, float zoom_ticks);
    void draw_channel_event_data(float tick_pos, float zoom_ticks,
                                 const midi::ChannelEventType& channel_event_type);

    struct VisibleTrack {
        std::size_t track;
        std::size_t first;
        std::size_t end;
        std::size_t stride = 1;
    };

    bool draw_track_direct(const VisibleTrack& visible, const std::vector<midi::Note>& notes,
                           std::uint64_t revision, std::uint32_t onion_meta,
                           bool is_current_track);

    void apply_density_cap(std::vector<VisibleTrack>& visible) const;

    static std::size_t max_handles_per_pixel();
    static constexpr std::size_t DEFAULT_MAX_HANDLES_PER_PIXEL = 4096;
    void set_direct_frame_uniforms(float tick_pos, float zoom_ticks);

    struct DirectState {
        int color_mode = 0;
        int track = -1;
        std::uint32_t onion_meta = 0xFFFFFFFFu;
        int selection_on = -1;
        std::size_t stride = 0;
        GLuint notes_tex = 0;
    };
    DirectState direct_state_;

    struct OnionRange {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::uint32_t color_meta = 0;
    };
    [[nodiscard]] OnionRange onion_range(std::size_t track_count,
                                         std::uint16_t nav_curr_track) const;

    struct HandleCache {
        GLuint fbo = 0;
        GLuint tex = 0;
        int width = 0;
        int height = 0;
        std::uint64_t key = 0;
        bool valid = false;
        bool unavailable = false;
    };
    HandleCache handle_cache_;
    std::uint64_t last_handles_key_ = 0;
    ShaderProgram dv_cache_program_;

    [[nodiscard]] std::uint64_t handles_key(std::vector<midi::MIDITrack>& tracks,
                                            const OnionRange& onion, std::uint16_t nav_curr_track,
                                            float tick_pos_offs, float zoom_ticks,
                                            const GLint viewport[4]) const;
    bool prepare_handle_cache(int width, int height);
    void composite_handle_cache();

    ImVec2 window_size_{0.0f, 0.0f};

    ShaderProgram dv_program_;
    Buffer dv_vertex_buffer_;
    VertexArray dv_vertex_array_;
    Buffer dv_instance_buffer_;
    Buffer dv_index_buffer_;

    ShaderProgram dv_handles_program_;
    Buffer dv_handles_vbo_;
    VertexArray dv_handles_vao_;
    Buffer dv_handles_ibo_;
    Buffer dv_handles_ebo_;

    std::vector<RenderDataViewBar> bars_render_;
    std::vector<RenderDataViewHandle> dv_handles_render_;

    ShaderProgram dv_handles_direct_program_;
    VertexArray dv_direct_vao_;
    std::shared_ptr<NoteGpuCache> note_gpu_cache_;

    float view_offset_ = 0.0f;
};

}
