#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/rendering.h"
#include "app/rendering/buffers.h"
#include "app/rendering/shaders.h"
#include "app/shared.h"
#include "app/view_settings.h"
#include "audio/audio_engine.h"
#include "editor/editing.h"
#include "editor/midi_bar_cacher.h"
#include "editor/midi_types.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::app::rendering {

inline constexpr std::size_t TV_NOTE_BUFFER_SIZE = 32768;
inline constexpr std::size_t BAR_BUFFER_SIZE = 64;

using TVBarStart = float;
using TVBarLength = float;
using BarMeta = std::uint32_t;

struct RenderTrackViewBar {
    TVBarStart bar_start;
    TVBarLength bar_length;
    BarMeta bar_meta;
};

struct RenderTrackViewNote {
    std::array<float, 4> note_rect;
    std::uint32_t note_meta;
};

// differs from the piano roll's index order on purpose; shaders branch on vertex id
inline constexpr std::array<std::uint32_t, 6> TV_QUAD_INDICES = {0, 1, 3, 1, 2, 3};

class TrackViewRenderer : public Renderer {
public:
    TrackViewRenderer(const util::SharedPtr<editor::ProjectManager>& project_manager,
                      util::SharedMutPtr<ViewSettings> view_settings,
                      util::SharedPtr<editor::TrackViewNavigation> nav,
                      util::SharedPtr<editor::PianoRollNavigation> pr_nav,
                      std::shared_ptr<audio::AudioEngine> playback_manager,
                      std::shared_ptr<editor::BarCacher> bar_cacher,
                      std::shared_ptr<NoteColors> colors,
                      std::shared_ptr<editor::SharedSelectedNotes> shared_selected_notes);

    void draw() override;

    void window_size(ImVec2 size) override { window_size_ = size; }
    void set_active(bool is_active) override { render_active_ = is_active; }
    void update_ppq(std::uint16_t new_ppq) override { ppq = new_ppq; }
    void set_selected(std::shared_ptr<editor::SharedSelectedNotes> selected_ids) override {
        selected_ = std::move(selected_ids);
    }

    void set_ghost_notes(util::SharedMutPtr<std::vector<std::pair<std::uint16_t,
                                                                 std::vector<midi::Note>>>> notes) {
        ghost_notes_ = std::move(notes);
    }
    void set_ghost_note_offset(util::SharedPtr<editor::SignedMIDITrkVec> offset) {
        ghost_notes_render_offset_ = std::move(offset);
    }

    util::SharedPtr<editor::TrackViewNavigation> navigation;
    std::shared_ptr<editor::BarCacher> bar_cacher;
    std::uint16_t ppq = 960;

private:
    [[nodiscard]] float get_time() const;

    util::SharedPtr<editor::PianoRollNavigation> pr_nav_;
    util::SharedMutPtr<ViewSettings> view_settings_;
    std::shared_ptr<audio::AudioEngine> playback_manager_;
    ImVec2 window_size_{0.0f, 0.0f};

    ShaderProgram tv_program_;
    Buffer tv_vertex_buffer_;
    VertexArray tv_vertex_array_;
    Buffer tv_instance_buffer_;
    Buffer tv_index_buffer_;

    ShaderProgram tv_notes_program_;
    Buffer tv_notes_vbo_;
    VertexArray tv_notes_vao_;
    Buffer tv_notes_ibo_;
    Buffer tv_notes_ebo_;

    std::vector<RenderTrackViewBar> bars_render_;
    std::vector<RenderTrackViewNote> notes_render_;
    util::SharedPtr<std::vector<midi::MIDITrack>> all_tracks_;
    std::shared_ptr<NoteColors> note_colors_;

    util::SharedMutPtr<std::vector<std::pair<std::uint16_t, std::vector<midi::Note>>>> ghost_notes_;
    util::SharedPtr<editor::SignedMIDITrkVec> ghost_notes_render_offset_;

    std::vector<std::size_t> last_note_start_;
    std::vector<std::size_t> first_render_note_;
    float last_time_ = 0.0f;

    std::shared_ptr<editor::SharedSelectedNotes> selected_;
    bool render_active_ = false;
};

}
