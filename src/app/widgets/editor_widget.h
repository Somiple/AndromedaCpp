#pragma once

#include <memory>
#include <optional>

#include "app/rendering.h"
#include "editor/editor_component.h"
#include "editor/util.h"

namespace andromeda::app {

class MainWindow;

struct WidgetContext {
    editor::ViewRect work_rect;
    MainWindow* parent = nullptr;
};

// smoothed left edge and width of a view, in ticks
struct TickView {
    float pos = 0.0f;
    float zoom = 0.0f;
};

// framebuffer size and how many of its pixels one imgui unit covers
struct GlSurface {
    int fb_w = 0;
    int fb_h = 0;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
};

// a view of the project: a renderer drawing into work_rect and the editor component that
// takes the input landing on it
class EditorWidget {
public:
    explicit EditorWidget(MainWindow& parent) { context.parent = &parent; }
    virtual ~EditorWidget() = default;

    EditorWidget(const EditorWidget&) = delete;
    EditorWidget& operator=(const EditorWidget&) = delete;

    // must run inside the imgui window the widget is laid out in
    void update();

    // the renderer pass over work_rect, skipped while the rect is empty; gl context thread only
    void draw_gl();

    [[nodiscard]] virtual TickView tick_view() const { return {}; }
    // moves the view at once, the smoothed position included
    virtual void jump_to_tick(float pos) { (void)pos; }
    virtual void zoom_by(float x_fac, float y_fac) {
        (void)x_fac;
        (void)y_fac;
    }
    // width of whatever the widget draws left of tick 0
    [[nodiscard]] virtual float keyboard_width() const { return 0.0f; }

    WidgetContext context;
    std::shared_ptr<rendering::Renderer> target_renderer;
    // owned by the editor controller
    editor::EditorComponent* component = nullptr;

protected:
    virtual void handle_navigation() {}
    // editor/ stays free of imgui, so the mouse and keys of a frame are read here and handed over
    virtual void handle_input() {}
    // drawn into the current imgui window, on top of the gl pass
    virtual void draw_overlays() {}

    void draw_playhead_line() const;
};

}
