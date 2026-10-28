#pragma once

// refactor: new file. view activation and the active-track fact used to live on MainWindow;
// they belong to this coordinator (see docs/ARCHITECTURE.md section 11).

#include <cstdint>
#include <memory>

#include "core/shared.h"
#include "editor/navigation.h"
#include "ui/rendering/rendering.h"

namespace andromeda::editor {
class EditorController;
}

namespace andromeda::app {

class EditorWidget;

/**
 * @brief Narrow dependency bundle for ViewCoordinator.
 *
 * References only: never a MainWindow back-pointer, so the coordinator cannot reach window
 * state it has no business touching. The view widgets are not part of it because they are
 * created after this coordinator exists; they arrive through set_widgets().
 */
struct ViewCoordinatorServices {
    /// Piano-roll viewport state; its `mutex` guards the value.
    util::SharedPtr<editor::PianoRollNavigation>& navigation;
    /// Track-view viewport state; independent from `navigation`.
    util::SharedPtr<editor::TrackViewNavigation>& track_navigation;
    /// Owns the active-track selection; the coordinator only forwards into it.
    editor::EditorController& controller;
    /// The window's render-type storage, written when the active view changes.
    rendering::RenderType& active_type;
};

/**
 * @brief Owns "which view is active and how it is activated".
 *
 * Before this class, MainWindow::switch_view and MainWindow::set_render_type both touched
 * renderer activation and MainWindow::on_current_track_changed was a second writer of the
 * current-track fact. Both now live here, so the window only forwards.
 *
 * @note Never throws. Must only be used on the thread that owns the widgets.
 */
class ViewCoordinator {
public:
    /**
     * @brief Stores the dependency bundle.
     * @param services borrowed references; every one must outlive this object.
     */
    explicit ViewCoordinator(ViewCoordinatorServices services) noexcept;

    /**
     * @brief Supplies the view widgets once MainWindow::init_widgets has built them.
     * @param piano_roll piano-roll view widget.
     * @param track_view track-view widget.
     * @param data_view data-view widget.
     */
    void set_widgets(EditorWidget* piano_roll, EditorWidget* track_view, EditorWidget* data_view);

    /**
     * @brief Selects a central view, carrying the viewport position across.
     *        Was MainWindow::switch_view.
     * @param to the view to show.
     */
    void switch_view(rendering::RenderType to);

    /**
     * @brief Activates the renderer of @p to and deactivates the other central renderer.
     *        Was MainWindow::set_render_type; this is the only place that activates a
     *        renderer. The data-view renderer is a distinct object and is activated here
     *        together with the piano roll.
     * @param to the view to activate.
     */
    void set_render_type(rendering::RenderType to);

    /**
     * @brief Makes @p track the active track through EditorController.
     *        Was MainWindow::on_current_track_changed.
     * @param track track index to activate.
     */
    void on_current_track_changed(std::uint16_t track);

private:
    ViewCoordinatorServices m_services;   ///< borrowed references, see ViewCoordinatorServices

    // refactor: EditorWidget* rather than the concrete widget types, so the coordinator stays
    // view-agnostic and the window converts implicitly when it calls set_widgets().
    EditorWidget* m_piano_roll = nullptr;   ///< piano-roll view widget
    EditorWidget* m_track_view = nullptr;   ///< track-view widget
    EditorWidget* m_data_view = nullptr;    ///< data-view widget
};

} // namespace andromeda::app
