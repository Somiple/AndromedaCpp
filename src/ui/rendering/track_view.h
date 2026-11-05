#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <imgui.h>

#include "ui/rendering/rendering.h"
#include "ui/rendering/buffers.h"
#include "ui/rendering/shaders.h"
#include "ui/style/note_colors.h"
#include "ui/app/view_settings.h"
#include "audio/audio_timer.h"
#include "editor/editing.h"
#include "editor/midi_types.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "core/shared.h"

#include "primitive_batch/primitive_batch.h"
#include "primitive_batch/rect_batch.h"

namespace andromeda::app {
class MainWindow;
}

namespace andromeda::app::rendering {

inline constexpr std::size_t TV_NOTE_BUFFER_SIZE = 32768;

struct RenderTrackViewNote {
    std::array<float, 4> note_rect;
    std::uint32_t note_meta;
};

// differs from the piano roll's index order on purpose; shaders branch on vertex id
inline constexpr std::array<std::uint32_t, 6> TV_QUAD_INDICES = {0, 1, 3, 1, 2, 3};

class TrackViewRenderer : public Renderer {
public:
    // TODO: fix this function lol
    TrackViewRenderer(app::MainWindow* app);

    void draw() override;

    void window_size(ImVec2 size) override { window_size_ = size; }
    void set_active(bool is_active) override { render_active_ = is_active; }

    void set_ghost_notes(util::SharedMutPtr<std::vector<std::pair<std::uint16_t,
                                                                 std::vector<midi::Note>>>> notes) {
        ghost_notes_ = std::move(notes);
    }
    void set_ghost_note_offset(util::SharedPtr<editor::SignedMIDITrkVec> offset) {
        ghost_notes_render_offset_ = std::move(offset);
    }

private:
    [[nodiscard]] float get_time() const;

    ImVec2 window_size_{0.0f, 0.0f};

    ShaderProgram tv_notes_program_;
    Buffer tv_notes_vbo_;
    VertexArray tv_notes_vao_;
    Buffer tv_notes_ibo_;
    Buffer tv_notes_ebo_;

    std::vector<RenderTrackViewNote> notes_render_;

    RectBatch batch_track_view_bars_;

    util::SharedMutPtr<std::vector<std::pair<std::uint16_t, std::vector<midi::Note>>>> ghost_notes_;
    util::SharedPtr<editor::SignedMIDITrkVec> ghost_notes_render_offset_;

    std::vector<std::size_t> last_note_start_;
    std::vector<std::size_t> first_render_note_;
    float last_time_ = 0.0f;

    bool render_active_ = false;
};

}
