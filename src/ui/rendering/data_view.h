#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "ui/rendering/rendering.h"
#include "ui/rendering/buffers.h"
#include "ui/rendering/note_cull_helper.h"
#include "ui/rendering/note_gpu_cache.h"
#include "ui/rendering/shaders.h"
#include "ui/style/note_colors.h"
#include "ui/app/view_settings.h"
#include "editor/editing.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "midi/events/channel_event.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "core/shared.h"

#include "primitive_batch/primitive_batch.h"
#include "primitive_batch/rect_batch.h"
#include "editor/editing/data_editing/event_lane.h"

namespace andromeda::editor {
class EventLane;
struct EventLanePoint;
}

namespace andromeda::app::rendering {

class DataViewRenderer : public Renderer, public AppEventListener {
public:
    // TODO: fix this function cuz wtf
    DataViewRenderer(app::MainWindow* app);

    ~DataViewRenderer() override;

    void draw() override;

    void use_note_cache(std::shared_ptr<NoteGpuCache> cache) {
        note_gpu_cache_ = std::move(cache);
    }

    /// @param lane lane the editor projects for this strip; not owned, may be nullptr.
    ///        Once attached the lane is the only truth for this panel - its points already carry
    ///        their own length, channel and track, so the legacy note walk must not also run.
    void set_lane(const editor::EventLane* lane) noexcept { lane_ = lane; }

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

    /// @return true while velocity notes are still streaming to the GPU; the host should keep
    ///         requesting frames until it turns false.
    [[nodiscard]] bool uploading() const noexcept { return uploading_; }

private:
    [[nodiscard]] float get_time() const;

    struct LaneRenderContext {
        float tick_pos_offs = 0.0f;
        float view_end = 0.0f;
        float zoom_ticks = 0.0f;
        float baseline = 0.0f;
        bool is_global_lane = false;
    };

    /// Draws the editable lane and its ghost lanes from the editor-owned projection.
    /// @param tick_pos first visible tick.
    /// @param zoom_ticks visible tick span.
    /// @return true when the lane owns the panel (drawn, or legitimately empty); false when no
    ///         lane is attached, so the legacy note walk should run instead.
    bool draw_lane_handles(float tick_pos, float zoom_ticks);

    // ---- Velocity lane: drawn straight from the track notes, GPU-resident -------------------
    // There is no EventLanePoint projection for velocity. Each visible track's notes are copied
    // once per track revision into a texture buffer (16 bytes per note: start, end, velocity,
    // channel), a few notes per frame, and drawn by vertex pulling (12 vertices per note). The
    // visible [first, end) range comes from the shared note culler, so nothing is built per frame.
    // Falls back to a strided CPU draw when the shader is missing, a track is too large for the
    // driver's texture buffer, or ANDROMEDA_DV_NO_RESIDENT is set.
    struct NoteResident {
        GLuint buf = 0;
        GLuint tex = 0;
        const midi::Note* data = nullptr;
        std::size_t total = 0;     ///< notes in the track
        std::size_t uploaded = 0;  ///< notes copied to the GPU in the current pass
        std::size_t drawable = 0;  ///< prefix of the buffer that is safe to draw
        std::uint64_t revision = ~0ull;
        bool overwrite = false;    ///< same-size edit: old contents stay drawable while refreshed
        bool too_large = false;
    };
    std::unordered_map<std::uint16_t, NoteResident> note_res_;

    struct ResidentPalette {
        GLuint buf = 0;
        GLuint tex = 0;
        std::size_t entries = 0;
        std::uint64_t version = ~0ull;
        std::uint64_t index_type = ~0ull;
    };
    ResidentPalette palette_;

    struct VelocityDraw {
        std::size_t track = 0;
        std::size_t first = 0;
        std::size_t end = 0;
        std::size_t stride = 1;
        int ghost_mode = 0; ///< 0 = editable track, 1/2 = onion partial/grey colouring
    };
    std::vector<VelocityDraw> velocity_draws_;
    LaneRenderContext velocity_ctx_;
    bool velocity_pending_ = false;
    bool uploading_ = false;

    ShaderProgram dv_resident_program_;
    VertexArray dv_resident_vao_;
    std::vector<std::uint32_t> resident_staging_; ///< reused upload scratch

    /// Culls, uploads and queues the velocity lane. Runs inside the batch scope (uploads don't
    /// touch draw state); the queued GPU draw is issued later by draw_velocity_resident().
    void draw_velocity_lane(const LaneRenderContext& ctx);
    /// Copies up to @p budget notes of @p notes into @p r. @return false if @p r can't be resident.
    bool sync_note_resident(NoteResident& r, const std::vector<midi::Note>& notes,
        std::uint64_t revision, std::size_t& budget);
    void sync_palette(std::uint32_t max_key);
    void draw_velocity_cpu(std::size_t track, std::size_t first, std::size_t end,
        std::size_t stride, const std::vector<midi::Note>& notes, int ghost_mode,
        const LaneRenderContext& ctx);
    /// Issues the queued resident draw. Must run AFTER the primitive batch scope closes,
    /// otherwise the batch flush would paint the bars over the handles.
    void draw_velocity_resident();

    void draw_continuous_lane(const std::vector<editor::EventLanePoint>& points,
        const LaneRenderContext& ctx, bool is_ghost);

    void draw_lane_point(const editor::EventLanePoint& point, editor::MIDITick length,
        float start_value, const LaneRenderContext& ctx,
        bool is_ghost);

    editor::MIDITick get_lane_point_length(
        const editor::EventLanePoint& point, const LaneRenderContext& ctx) const;

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

    const editor::EventLane* lane_ = nullptr; ///< non-owning; set by the data-view widget

    // renders bars AND the handles
    RectBatch data_view_render_batch_;
    
    std::shared_ptr<NoteGpuCache> note_gpu_cache_;

    float view_offset_ = 0.0f;
};

}