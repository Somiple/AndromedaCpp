#version 330
layout (location = 0) in vec2 vPos;

layout (location = 1) in float barStart;
layout (location = 2) in float barLength;
layout (location = 3) in uint barNumber;

out vec2 uv;
out float oddBarFac;
out float bLength;
out float bHeight;

void main() {
    float x_pos = 0.0f;
    float y_pos = 0.0f;
    if (int(gl_VertexID % 4) == 0) {
        x_pos = barStart;
        y_pos = 0.0f;
    } else if (int(gl_VertexID % 4) == 1) {
        x_pos = barStart + barLength;
        y_pos = 0.0f;
    } else if (int(gl_VertexID % 4) == 2) {
        x_pos = barStart + barLength;
        y_pos = 1.0f;
    } else if (int(gl_VertexID % 4) == 3) {
        x_pos = barStart;
        y_pos = 1.0f;
    }

    uv = vPos;
    oddBarFac = (int(barNumber) % 2 == 1) ? 0.8 : 1.0;
    bLength = barLength;
    bHeight = 1.0f;

    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, 0.0, 1.0);
}