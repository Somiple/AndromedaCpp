#version 330

// Untextured vertex-colour shader used by the MIDI event lane. Positions arrive in pixels
// relative to the lane rect with +y pointing up, which is what the GL viewport already is.

layout (location = 0) in vec2 a_pos;
layout (location = 1) in vec4 a_color;

out vec4 v_color;

uniform float u_viewport_x;
uniform float u_viewport_y;

void main() {
    vec2 ndc = (a_pos / vec2(u_viewport_x, u_viewport_y)) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_color = a_color;
}
