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
#include "editor/editor_controller.h"

namespace andromeda::app {
class MainWindow;
}

namespace andromeda::app::rendering {

class NoteCullHelper;

class Renderer {
public:
    Renderer(andromeda::app::MainWindow* app) : _app(app) { }

    virtual ~Renderer() = default;

    virtual void draw() = 0;

    virtual void set_ghost_notes(std::vector<midi::Note>* notes) {
        (void)notes;
    }
    virtual void clear_ghost_notes() {}
    // selected should only be called from the editor controller!
    virtual void window_size(ImVec2 size) { (void)size; }
    virtual void app_scale(float scale) { (void)scale; }
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
protected:
    andromeda::app::MainWindow* _app;
};

enum class RenderType { PianoRoll, TrackView };

// no locking: renderers must only be touched on the gl context thread
class RenderManager : public AppEventListener {
public:
    void init_renderers(andromeda::app::MainWindow* app);

    void switch_renderer(RenderType render_type);

    // TODO: remove this, only get ppq from editor controller's project manager
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
