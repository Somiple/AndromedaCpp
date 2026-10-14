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

// which of the two central widgets is on screen
enum class RenderType { PianoRoll, TrackView };

}
