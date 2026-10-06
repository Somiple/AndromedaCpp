#include "app/rendering.h"
#include "app/main_window.h"
#include "app/rendering/note_cull_helper.h"
#include "app/rendering/piano_roll.h"
#include "app/rendering/track_view.h"
#include "util/debugger.h"

namespace andromeda::app::rendering {

using util::Debugger;

void RenderManager::init_renderers(app::MainWindow* main_window) {

    Debugger::log("Initializing piano roll renderer");
    renderers_.push_back(std::make_shared<PianoRollRenderer>(main_window));

    Debugger::log("Initializing track view renderer");
    renderers_.push_back(std::make_shared<TrackViewRenderer>(main_window));
}

void RenderManager::switch_renderer(RenderType render_type) { set_active(render_type); }

void RenderManager::set_ppq(std::uint16_t ppq) {
    /*for (auto& renderer : renderers_) {
        renderer->update_ppq(ppq);
    }*/
}

Renderer* RenderManager::get_active_renderer() {
    const auto renderer = get_renderer(render_type_);
    return renderer.get();
}

std::shared_ptr<Renderer> RenderManager::get_renderer(RenderType render_type) {
    const auto index = render_type == RenderType::PianoRoll ? 0u : 1u;
    if (index >= renderers_.size()) {
        return nullptr;
    }
    return renderers_[index];
}

void RenderManager::on_event(const AndromedaEvent& event) {
    if (const auto* ppq_changed = std::get_if<PPQChanged>(&event)) {
        set_ppq(ppq_changed->new_ppq);
    }
}

void RenderManager::set_active(RenderType render_type) {
    switch (render_type) {
    case RenderType::PianoRoll:
        if (const auto other = get_renderer(RenderType::TrackView)) {
            other->set_active(false);
        }
        render_type_ = RenderType::PianoRoll;
        break;
    case RenderType::TrackView:
        if (const auto other = get_renderer(RenderType::PianoRoll)) {
            other->set_active(false);
        }
        render_type_ = RenderType::TrackView;
        break;
    }

    if (const auto active = get_renderer(render_type)) {
        active->set_active(true);
    }
}

}
