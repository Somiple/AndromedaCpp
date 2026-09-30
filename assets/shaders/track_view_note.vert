#version 330
layout (location = 0) in vec2 vPos;

layout (location = 1) in vec4 noteRect;
layout (location = 2) in uint noteMeta;

out vec2 uv;
out vec3 color;

out float noteWidth;
out float noteHeight;

uniform sampler2D noteColorTexture;
uniform float zoomTicks;
uniform float zoomTracks;

void main() {
    color = texture2D(noteColorTexture, vec2(float((noteMeta & uint(0xF))) / 16.0, 0.5)).rgb;
    if ((noteMeta & uint(1 << 13)) != uint(0)) {
        color = vec3(0.9, 0.4, 0.4);
    }

    vec2 uv_;
    float x_pos = 0.0f;
    float y_pos = 0.0f;

    float noteStart = noteRect.x * zoomTicks;
    float noteLength = noteRect.y * zoomTicks;
    float noteBottom = noteRect.z * zoomTracks;
    float noteTop = noteRect.w * zoomTracks;

    noteWidth = noteLength;
    noteHeight = noteTop - noteBottom;

    if (int(gl_VertexID % 4) == 0) {
        x_pos = noteStart;
        y_pos = noteBottom;
        uv_ = vec2(0.0, 0.0);
    } else if (int(gl_VertexID % 4) == 1) {
        x_pos = noteStart + noteLength;
        y_pos = noteBottom;
        uv_ = vec2(1.0, 0.0);
    } else if (int(gl_VertexID % 4) == 2) {
        x_pos = noteStart + noteLength;
        y_pos = noteTop;
        uv_ = vec2(1.0, 1.0);
    } else if (int(gl_VertexID % 4) == 3) {
        x_pos = noteStart;
        y_pos = noteTop;
        uv_ = vec2(0.0, 1.0);
    }

    uv = uv_;
    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, 0.0, 1.0);
}