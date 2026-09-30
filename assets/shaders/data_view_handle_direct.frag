#version 330

// data_view_handles.frag painted a handle's left and top 2px and wrote alpha 0
// over the rest of its box. The direct path's vertex shader now emits only
// those two edges, so every fragment that reaches here is one that border test
// passed - it is the handle's colour, opaque, and nothing is left to decide.

out vec4 fragColor;

in vec3 color;

void main() {
    fragColor = vec4(color, 1.0);
}
