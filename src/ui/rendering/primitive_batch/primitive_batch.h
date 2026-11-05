#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <span>

#include "ui/rendering/rendering.h"
#include "ui/rendering/buffers.h"
#include "ui/rendering/shaders.h"

namespace andromeda::app::rendering {

constexpr const unsigned PRIMITIVES_PER_PASS = 32768;

using VertexPos = std::array<float, 2>;

#pragma pack(push, 1)
struct PrimVertex {
	VertexPos position;
	std::uint32_t color;
};
#pragma pack(pop)

class PrimitiveScope;

// base class for drawing primitives in batches
class PrimitiveBatch {
public:
	PrimitiveBatch(std::size_t max_verts,
		const std::vector<std::uint32_t> base_indices);

	virtual ~PrimitiveBatch() = default;
protected:
	void push(std::span<const PrimVertex> vertices);
private:
	// flushes queued vertices for the primitive and draws it.
	void draw();

	std::vector<PrimVertex> queued_vertices_{};

	ShaderProgram program_;
	VertexArray vao_;
	Buffer vbo_;
	Buffer ibo_;

	std::size_t n_base_vertices_ = 0;
	std::size_t n_base_indices_ = 0;

	friend class PrimitiveScope;
};

// RAII thingy for automatically flushing things.
class PrimitiveScope {
public:
	PrimitiveScope(PrimitiveBatch* batch)
		: batch_(batch) {}
	
	~PrimitiveScope() {
		if (!batch_) return;
		batch_->draw();
	}

	PrimitiveScope(const PrimitiveScope&) = delete;
	PrimitiveScope& operator=(const PrimitiveScope&) = delete;

	PrimitiveScope(PrimitiveScope&&) = delete;
	PrimitiveScope& operator=(PrimitiveScope&&) = delete;
private:
	PrimitiveBatch* batch_;
};

}