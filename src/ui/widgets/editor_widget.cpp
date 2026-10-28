#include "ui/widgets/editor_widget.h"

#include <glad/glad.h>

#include <algorithm>
#include <cstdlib>

#include <imgui.h>

#include "ui/app/main_window.h"
#include "ui/app/editor_app.h"
#include "ui/app/managers.h"

namespace andromeda::app {

void EditorWidget::update() {
    if (!context.parent->is_mouse_over_ui()) {
        handle_navigation();
    }

    if (component != nullptr) {
        component->set_rect(context.work_rect);
    }

    handle_input();
    draw_overlays();
}

void EditorWidget::draw_gl() {
    const editor::ViewRect& rect = context.work_rect;
    if (!target_renderer || rect.width <= 0.0f || rect.height <= 0.0f) {
        return;
    }

    const std::optional<GlSurface> surface = context.parent->gl_surface();
    if (!surface) {
        return;
    }

    const auto vp_x = static_cast<GLint>(rect.left * surface->scale_x);
    const auto vp_y = static_cast<GLint>(static_cast<float>(surface->fb_h) -
                                         (rect.top + rect.height) * surface->scale_y);
    const auto vp_w = static_cast<GLsizei>(rect.width * surface->scale_x);
    const auto vp_h = static_cast<GLsizei>(rect.height * surface->scale_y);

    static const bool tiny = std::getenv("ANDROMEDA_TINY_VIEWPORT") != nullptr;
    if (tiny) {
        glViewport(vp_x, vp_y, std::max(1, vp_w / 8), std::max(1, vp_h / 8));
    } else {
        glViewport(vp_x, vp_y, vp_w, vp_h);
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(vp_x, vp_y, vp_w, vp_h);

    // gl passes run before imgui, so blend is set here; handle shaders rely on alpha
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ONE);

    // fixed rust bug: cleared before setting the colour, so it used the previous one
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    target_renderer->window_size(ImVec2(static_cast<float>(vp_w), static_cast<float>(vp_h)));
    target_renderer->app_scale(context.parent->app_scale());
    target_renderer->draw();

    glDisable(GL_SCISSOR_TEST);
}

void EditorWidget::draw_playhead_line() const {
    const MainWindow& parent = *context.parent;
    const editor::ViewRect& rect = context.work_rect;

    // const auto [min_tick, max_tick] = parent.get_view_tick_range_with_playback();
    const auto tick_view = parent.get_central_tick_view();
    if (tick_view.zoom <= 0.0f) return;

    const auto* timeline = parent.controller().get_project_manager()->get_timeline();
    const auto* app = parent.get_editor_app();
    const auto* playback = app->get_playback_manager();

    // absolute playhead tick pos
    const double playhead_tick = static_cast<double>(playback->get_playhead_tick_pos());
    double playhead_offset = playhead_tick - static_cast<double>(tick_view.pos);

    // skip rendering if playhead's outside view
    if (playhead_offset < 0.0 || playhead_offset > static_cast<double>(tick_view.zoom))
        return;

    const float kb_width = keyboard_width();
    const float ui_pos = static_cast<float>(playhead_offset / static_cast<double>(tick_view.zoom))
        * (rect.width - kb_width) + rect.left + kb_width;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect({rect.left, rect.top}, {rect.left + rect.width, rect.top + rect.height}, true);
    dl->AddLine({ui_pos, rect.top}, {ui_pos, rect.top + rect.height},
                IM_COL32(255, 255, 255, 255), 1.0f);
    dl->PopClipRect();
}

}
