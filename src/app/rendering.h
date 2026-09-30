#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>

#include "app/app_event_listener.h"
#include "app/shared.h"
#include "app/view_settings.h"
#include "audio/audio_engine.h"
#include "editor/editing.h"
#include "editor/midi_bar_cacher.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "midi/events/note.h"
#include "util/shared.h"

namespace andromeda::app::rendering {

class NoteCullHelper;

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void draw() = 0;

    virtual void set_ghost_notes(util::SharedMutPtr<std::vector<midi::Note>> notes) {
        (void)notes;
    }
    virtual void clear_ghost_notes() {}
    virtual void set_selected(std::shared_ptr<editor::SharedSelectedNotes> selected_ids) {
        (void)selected_ids;
    }
    virtual void window_size(ImVec2 size) { (void)size; }
    virtual void app_scale(float scale) { (void)scale; }
    virtual void update_ppq(std::uint16_t ppq) { (void)ppq; }
    virtual void time_changed(std::uint64_t time) { (void)time; }
    virtual void set_active(bool is_active) { (void)is_active; }

    [[nodiscard]] virtual std::size_t instances_drawn() const { return 0; }
    [[nodiscard]] virtual std::size_t resident_instances() const { return 0; }
    [[nodiscard]] virtual std::size_t cpu_instances() const { return 0; }
    [[nodiscard]] virtual std::size_t residency_refusals() const { return 0; }
    [[nodiscard]] virtual std::size_t bail_range_past_end() const { return 0; }
    [[nodiscard]] virtual std::size_t bail_empty_range() const { return 0; }
    [[nodiscard]] virtual std::size_t resident_tracks() const { return 0; }
    [[nodiscard]] virtual std::size_t cpu_tracks() const { return 0; }
    [[nodiscard]] virtual std::size_t resident_megabytes() const { return 0; }

    [[nodiscard]] virtual std::size_t coverage_tested() const { return 0; }
    [[nodiscard]] virtual std::size_t coverage_skipped() const { return 0; }

    virtual std::size_t take_uploaded_notes() { return 0; }
};

enum class RenderType { PianoRoll, TrackView };

// no locking: renderers must only be touched on the gl context thread
class RenderManager : public AppEventListener {
public:
    void init_renderers(const util::SharedPtr<editor::ProjectManager>& project_manager,
                        util::SharedPtr<editor::PianoRollNavigation> nav,
                        util::SharedPtr<editor::TrackViewNavigation> track_view_nav,
                        util::SharedMutPtr<ViewSettings> view_settings,
                        std::shared_ptr<audio::AudioEngine> playback_manager,
                        std::shared_ptr<editor::BarCacher> bar_cacher,
                        std::shared_ptr<NoteColors> colors,
                        std::shared_ptr<NoteCullHelper> note_cull_helper,
                        std::shared_ptr<editor::SharedSelectedNotes> shared_selected_notes);

    void switch_renderer(RenderType render_type);

    void set_ppq(std::uint16_t ppq);

    Renderer* get_active_renderer();

    [[nodiscard]] RenderType get_render_type() const { return render_type_; }

    std::shared_ptr<Renderer> get_renderer(RenderType render_type);

    void on_event(const AndromedaEvent& event) override;

    RenderType render_type_ = RenderType::PianoRoll;

private:
    void set_active(RenderType render_type);

    std::vector<std::shared_ptr<Renderer>> renderers_;
};

}
