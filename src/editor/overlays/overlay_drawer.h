#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <utility>

#include "overlay.h"   // Overlay, OverlayPoint, ScreenRect

namespace andromeda::editor { struct ComponentContext; } 

namespace andromeda::editor::overlays {

using andromeda::editor::ComponentContext;

// each editor component will own an overlay drawer
class OverlayDrawer {
public:
	explicit OverlayDrawer(ComponentContext* context) : _shared_context(context) { }

	Overlay* add_overlay(uint8_t layer, std::unique_ptr<Overlay> overlay);

	template <typename T, typename... Args>
	T* emplace_overlay(uint8_t layer, Args&&... args) {
		return static_cast<T*>(add_overlay(layer, std::make_unique<T>(std::forward<Args>(args)...)));
	}

	bool clear_layer(uint8_t layer);
	void clear_all();

	void draw_overlays(const TransformFn& to_screen) const;
private:
	ComponentContext* _shared_context = nullptr;
	// uint8_t for the overlay ID
	std::map<uint8_t, std::vector<std::unique_ptr<Overlay>>> _overlay_map{};
};

}