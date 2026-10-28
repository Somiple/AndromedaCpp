#pragma once

#include "core/math/vector2.h"
#include <functional>
#include <cstdint>

namespace andromeda::editor::overlays {

using andromeda::util::math::Vector2;

using ScreenRect = std::pair<Vector2<float>, Vector2<float>>;
using OverlayRect = std::pair<Vector2<int64_t>, Vector2<int64_t>>;
using TransformFn = std::function<ScreenRect(const OverlayRect&)>;

class Overlay {
public:
	explicit Overlay() = default;
	virtual ~Overlay() = default;
	
	virtual void draw(const TransformFn& to_screen) = 0;
};

}