#version 330

out vec4 fragColor;

in vec2 uv;
in vec3 color;

in float noteWidth;
in float noteHeight;

uniform float width;
uniform float height;

void main() {
    vec3 n_color = color;

    fragColor = vec4(n_color, 1.0);
}