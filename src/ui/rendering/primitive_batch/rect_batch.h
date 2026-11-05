#pragma once
#include "primitive_batch.h"

namespace andromeda::app::rendering {

class RectBatch : public PrimitiveBatch {
public:
	RectBatch() : PrimitiveBatch(4, { 0, 1, 2, 2, 3, 0 }) {}
	void push_rect(std::array<float, 2> pos, std::array<float, 2> size,
		std::uint32_t color);
	void push_v_line(float x, std::array<float, 2> y, std::uint32_t color, float thickness);
	void push_h_line(std::array<float, 2> x, float y, std::uint32_t color, float thickness);
private:
};

}