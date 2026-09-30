#version 330
layout (location = 0) in vec2 vPos;

layout (location = 1) in vec4 handleRect;
layout (location = 2) in uint handleMeta;

out vec2 uv;
out vec3 color;
out vec3 color2;

out float handleWidth;
out float handleHeight;

uniform sampler2D noteColorTexture;

void main() {
    vec3 n_color = texture2D(noteColorTexture, vec2(float(handleMeta & uint(0xF)) / 16.0, 0.5)).rgb;
    color2 = n_color * 0.5;

    n_color = mix(
        n_color / 2.0,
        n_color,
        float((handleMeta & uint(0xFF0)) >> uint(4)) / 128.0
    );

    if ((handleMeta & uint(1 << 13)) != uint(0)) {
        n_color = vec3(1.0, 0.5, 0.5);
    }

    float grayFactor = float((handleMeta & (uint(3) << uint(14))) >> uint(14)) / 2.0;
    n_color = mix(n_color, vec3(0.5, 0.5, 0.5), grayFactor);

    if ((handleMeta & uint(1 << 12)) != uint(0)) {
        n_color += vec3(0.5);
        color2 += 0.25;
    }

    color = n_color;

    vec2 uv_;
    float x_pos = 0.0f;
    float y_pos = 0.0f;

    float handleStart = handleRect.x;
    float handleLength = handleRect.y;
    float handleBottom = handleRect.z;
    float handleTop = handleRect.w;

    handleWidth = handleLength;
    handleHeight = handleTop - handleBottom;

    if (int(gl_VertexID % 4) == 0) {
        x_pos = handleStart;
        y_pos = handleBottom;
        uv_ = vec2(0.0, 0.0);
    } else if (int(gl_VertexID % 4) == 1) {
        x_pos = handleStart + handleLength;
        y_pos = handleBottom;
        uv_ = vec2(1.0, 0.0);
    } else if (int(gl_VertexID % 4) == 2) {
        x_pos = handleStart + handleLength;
        y_pos = handleTop;
        uv_ = vec2(1.0, 1.0);
    } else if (int(gl_VertexID % 4) == 3) {
        x_pos = handleStart;
        y_pos = handleTop;
        uv_ = vec2(0.0, 1.0);
    }

    uv = uv_;
    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, 0.0, 1.0);
}