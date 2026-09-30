#version 330

layout (location = 0) in float barStart;
layout (location = 1) in float barLength;
layout (location = 2) in uint barNumber;

out vec2 uv;
out float oddBarFac;
out float bLength;

uniform float width;
uniform float prBarBottom;
uniform float prBarTop;
uniform float keyboardHeight;

void main() {
    float x_pos = 0.0f;
    float y_pos = 0.0f;
    vec2 uv_ = vec2(0.0);
    if (int(gl_VertexID % 4) == 0) {
        x_pos = barStart;
        y_pos = prBarBottom;
        uv_ = vec2(0.0, 0.0);
    } else if (int(gl_VertexID % 4) == 1) {
        x_pos = barStart + barLength;
        y_pos = prBarBottom;
        uv_ = vec2(1.0, 0.0);
    } else if (int(gl_VertexID % 4) == 2) {
        x_pos = barStart + barLength;
        y_pos = prBarTop;
        uv_ = vec2(1.0, 1.0);
    } else if (int(gl_VertexID % 4) == 3) {
        x_pos = barStart;
        y_pos = prBarTop;
        uv_ = vec2(0.0, 1.0);
    }

    uv = uv_;
    oddBarFac = (int(barNumber) % 2 == 1) ? 0.8 : 1.0;
    bLength = barLength;

    float kbWidth = keyboardHeight / width;
    x_pos = x_pos * (1.0 - kbWidth) + kbWidth;
    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, 0.0, 1.0);
}