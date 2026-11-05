#include "primitive_batch.h"
#include <algorithm>

namespace {

#pragma region shader code
constexpr const char* PRIM_VERT = R"(#version 330 core
layout (location = 0) in vec2 vPos;
layout (location = 1) in uint vColor;

out vec3 color;

void main() {
	vec3 decoded = vec3(
		float((vColor & 0xFF0000u) >> 16u) / 255.0,
		float((vColor & 0xFF00u) >> 8u) / 255.0,
		float(vColor & 0xFFu) / 255.0
	);

	gl_Position = vec4(vPos * 2.0 - 1.0, 0.0, 1.0);
	color = decoded;
})";

constexpr const char* PRIM_FRAG = R"(#version 330 core
in vec3 color;
out vec4 fragColor;

void main() {
	fragColor = vec4(color, 1.0);
})";

#pragma endregion
}

namespace andromeda::app::rendering {

PrimitiveBatch::PrimitiveBatch(std::size_t max_verts,
	const std::vector<std::uint32_t> base_indices) {
	
	program_ = ShaderProgram::create("Primitive", PRIM_VERT, PRIM_FRAG);

	const std::size_t num_indices = base_indices.size();

	n_base_vertices_ = max_verts;
	n_base_indices_ = num_indices;

	std::vector<std::uint32_t> indices(PRIMITIVES_PER_PASS * num_indices);
	for (std::size_t i = 0; i < PRIMITIVES_PER_PASS; i++) {
		for (std::size_t j = 0; j < num_indices; j++) {
			indices[i * num_indices + j] = i * max_verts + base_indices[j];
		}
	}

	vbo_ = Buffer(GL_ARRAY_BUFFER);
	{
		vbo_.bind();
		glBufferData(GL_ARRAY_BUFFER, PRIMITIVES_PER_PASS * max_verts * sizeof(PrimVertex), nullptr, GL_DYNAMIC_DRAW);
	}

	ibo_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
	ibo_.set_data(std::span<const std::uint32_t>(indices), GL_STATIC_DRAW);

	vao_ = VertexArray::create();
	const GLint pos_attrib = program_.get_attrib_location("vPos");
	SET_ATTRIBUTE(GL_FLOAT, vao_, pos_attrib, PrimVertex, position);
	const GLint col_attrib = program_.get_attrib_location("vColor");
	SET_ATTRIBUTE(GL_UNSIGNED_INT, vao_, col_attrib, PrimVertex, color);

	queued_vertices_.reserve(PRIMITIVES_PER_PASS * max_verts);
}

void PrimitiveBatch::push(std::span<const PrimVertex> vertices) {
	if (vertices.size() != n_base_vertices_)
		throw std::runtime_error("Pushed an invalid number of vertices");

	queued_vertices_.insert(queued_vertices_.end(), vertices.begin(), vertices.end());
	
	if (queued_vertices_.size() >= PRIMITIVES_PER_PASS * n_base_vertices_)
		draw();
}

void PrimitiveBatch::draw() {
	if (queued_vertices_.empty()) return;

	glUseProgram(program_.id());
	vao_.bind();
	vbo_.bind();
	ibo_.bind();

	std::size_t total_prims = queued_vertices_.size() / n_base_vertices_;
	std::size_t prims_drawn = 0;

	while (prims_drawn < total_prims) {
		std::size_t prims_to_draw = std::min(static_cast<std::size_t>(PRIMITIVES_PER_PASS), total_prims - prims_drawn);
		std::size_t verts_to_draw = prims_to_draw * n_base_vertices_;
		std::size_t vert_offset = prims_drawn * n_base_vertices_;

		glBufferSubData(GL_ARRAY_BUFFER, 0, verts_to_draw * sizeof(PrimVertex), queued_vertices_.data() + vert_offset);
		glDrawElements(GL_TRIANGLES, prims_to_draw * n_base_indices_, GL_UNSIGNED_INT, nullptr);
		prims_drawn += prims_to_draw;
	}
	glUseProgram(0);

	queued_vertices_.clear();
}

}