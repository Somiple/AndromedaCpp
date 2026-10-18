#include "overlay_drawer.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace andromeda::editor::overlays {

Overlay* OverlayDrawer::add_overlay(uint8_t layer, std::unique_ptr<Overlay> overlay) {
	if (!overlay) return nullptr;

	auto& bucket = _overlay_map[layer];
	bucket.push_back(std::move(overlay));
	return bucket.back().get();
}

bool OverlayDrawer::clear_layer(uint8_t layer) { return _overlay_map.erase(layer) > 0; }
void OverlayDrawer::clear_all() { _overlay_map.clear(); }

void OverlayDrawer::draw_overlays(const TransformFn& to_screen) const {
	for (const auto& [layer, overlays] : _overlay_map) {
		for (const auto& overlay : overlays) {
			overlay->draw(to_screen);
		}
	}
}

}