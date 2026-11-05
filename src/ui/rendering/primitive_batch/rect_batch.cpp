#include "rect_batch.h"

namespace andromeda::app::rendering {

void RectBatch::push_rect(std::array<float, 2> pos, std::array<float, 2> size,
	std::uint32_t color) {
	const std::array<PrimVertex, 4> vertices{ {
		{ {pos[0],           pos[1]           }, color },
		{ {pos[0] + size[0], pos[1]           }, color },
		{ {pos[0] + size[0], pos[1] + size[1] }, color },
		{ {pos[0],           pos[1] + size[1] }, color }
	} };
	
	push(vertices);
}

void RectBatch::push_v_line(float x, std::array<float, 2> y, std::uint32_t color, float thickness) {
	const float h_thickness = thickness * 0.5;
	const std::array<PrimVertex, 4> vertices{ {
		{ { x - h_thickness, y[0] }, color},
		{ { x + h_thickness, y[0] }, color},
		{ { x + h_thickness, y[1] }, color},
		{ { x - h_thickness, y[1] }, color}
	} };

	push(vertices);
}

void RectBatch::push_h_line(std::array<float, 2> x, float y, std::uint32_t color, float thickness) {
	const float h_thickness = thickness * 0.5;

	const std::array<PrimVertex, 4> vertices{ {
		{ { x[0], y - h_thickness}, color},
		{ { x[1], y - h_thickness}, color},
		{ { x[1], y + h_thickness}, color},
		{ { x[0], y + h_thickness}, color}
	} };

	push(vertices);
}

}