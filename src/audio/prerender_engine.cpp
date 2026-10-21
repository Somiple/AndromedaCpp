#include "prerender_engine.h"
#include "app/main_window.h"

namespace andromeda::audio {

// a stub for now, until we figure some stuff out
void PrerenderEngine::initialize() { }

void PrerenderEngine::destroy() { }

void PrerenderEngine::start() {
	auto* timeline = _app->editor_controller.get_project_manager()->get_timeline();
	_app->get_timer()->start(timeline->tick_to_secs(_app->playhead->start_tick));
}

void PrerenderEngine::stop() {
	_app->get_timer()->stop();
}

void PrerenderEngine::reset() {
	stop();
}

bool PrerenderEngine::is_playing() {
	return _app->get_timer()->is_running();
}

void PrerenderEngine::render_settings() {
	ImGui::Text("Needs implementation");
}

}