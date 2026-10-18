#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include "editor/util.h"
#include "editor/midi_types.h"
#include "overlay.h"

namespace andromeda::editor::overlays {

using editor::MIDITick;
using editor::ViewRect;

enum class SelectionType : uint8_t {
	Default,
	AddSelect,
	RemoveSelect,
	Delete
};

struct SelectionBoxInfo {
	SelectionType type = SelectionType::Default;
	Vector2<int64_t> start{};
	Vector2<int64_t> end{};
	Vector2<int64_t> offset{};   // draw-only, used while a selection move is in progress
};

class SelectionBox : public Overlay {
public:
	void draw(const TransformFn& to_screen) override;

	// normalized (min, max) with offset applied. Track axis is exclusive at max.
	[[nodiscard]] OverlayRect bounds() const;

	SelectionBoxInfo info{};
};

}