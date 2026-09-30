#version 330
layout (location = 0) in vec2 vPos;

layout (location = 1) in float barStart;
layout (location = 2) in float barLength;
layout (location = 3) in uint barMeta;

out vec2 uv;
out float oddBarFac;
flat out int isCurrTrack;
out float bLength;
out float bHeight;

uniform float tvBarBottom;
uniform float tvBarTop;
uniform int currTrack;

void main() {
    float x_pos = 0.0f;
    float y_pos = 0.0f;
    if (int(gl_VertexID % 4) == 0) {
        x_pos = barStart;
        y_pos = tvBarBottom;
    } else if (int(gl_VertexID % 4) == 1) {
        x_pos = barStart + barLength;
        y_pos = tvBarBottom;
    } else if (int(gl_VertexID % 4) == 2) {
        x_pos = barStart + barLength;
        y_pos = tvBarTop;
    } else if (int(gl_VertexID % 4) == 3) {
        x_pos = barStart;
        y_pos = tvBarTop;
    }

    uv = vPos;
    oddBarFac = ((uint(barMeta) >> uint(31)) == uint(1)) ? 0.8 : 1.0;
    isCurrTrack = ((uint(barMeta) & uint(0xFFFF)) == uint(currTrack)) ? 1 : 0;
    bLength = barLength;
    bHeight = tvBarTop - tvBarBottom;

    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, 0.0, 1.0);
}