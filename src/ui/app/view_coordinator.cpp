// refactor: new translation unit. MainWindow::switch_view, set_render_type and
// on_current_track_changed moved here unchanged; the window now only forwards.

#include "ui/app/view_coordinator.h"

#include <mutex>

#include "editor/editor_controller.h"
#include "editor/project/project_manager.h"
#include "ui/widgets/editor_widget.h"

namespace andromeda::app {

ViewCoordinator::ViewCoordinator(ViewCoordinatorServices services) noexcept
    : m_services(services) {}

void ViewCoordinator::set_widgets(EditorWidget* piano_roll, EditorWidget* track_view,
                                  EditorWidget* data_view) {
    m_piano_roll = piano_roll;
    m_track_view = track_view;
    m_data_view = data_view;
}

void ViewCoordinator::switch_view(rendering::RenderType to) {
    // was MainWindow::switch_view: the viewport position carries over to the incoming view.
    auto& nav = m_services.navigation;
    auto& track_nav = m_services.track_navigation;
    {
        std::unique_lock pr_lock(nav->mutex);
        std::unique_lock tv_lock(track_nav->mutex);
        if (to == rendering::RenderType::TrackView) {
            track_nav->value.tick_pos = nav->value.tick_pos;
            track_nav->value.tick_pos_smoothed = nav->value.tick_pos_smoothed;
        } else {
            nav->value.tick_pos = track_nav->value.tick_pos;
            nav->value.tick_pos_smoothed = track_nav->value.tick_pos_smoothed;
        }
    }

    set_render_type(to);
}

void ViewCoordinator::set_render_type(rendering::RenderType to) {
    // refactor: this is now the only place that activates a renderer; switch_view() no longer
    // calls set_active() itself. the data-view renderer is activated together with the piano
    // roll because each widget builds its own renderer object, so it is a distinct renderer
    // rather than a second activation of the same one.
    const bool changed = to != m_services.active_type;

    EditorWidget* const shown =
        to == rendering::RenderType::TrackView
            ? m_track_view
            : m_piano_roll;
    EditorWidget* const hidden =
        to == rendering::RenderType::TrackView
            ? m_piano_roll
            : m_track_view;

    hidden->target_renderer->set_active(false);
    m_services.active_type = to;
    shown->target_renderer->set_active(true);

    // only on a real switch: the first set_render_type() call runs from init_widgets() while
    // active_type already holds this value, and must not activate the data view at startup.
    if (changed && to == rendering::RenderType::PianoRoll) {
        m_data_view->target_renderer->set_active(true);
    }
}

void ViewCoordinator::on_current_track_changed(std::uint16_t track) {
    // refactor: selection now lives only on EditorController. set_active_track() performs the
    // track-list validation itself, so the old validate_tracks() call here (a no-op that only
    // logged a deprecation warning) and the navigation mirror are both gone.
    m_services.controller.set_active_track(track);
}

} // namespace andromeda::app
