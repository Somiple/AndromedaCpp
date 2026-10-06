#version 330

layout (location = 0) in vec2 vPos;
layout (location = 1) in vec4 noteRect;
layout (location = 2) in uint noteMeta;

out vec2 uv;
out vec3 color;
out vec3 color2;

out float noteWidth;
out float noteHeight;

uniform sampler2D noteColorTexture;
uniform float keyboardHeight;
uniform float noteDepth;
uniform float width;

void main() {
    float noteStart = noteRect.x;
    float noteLength = noteRect.y;
    float noteBottom = noteRect.z;
    float noteTop = noteRect.w;

    // Notes narrower than one screen pixel have no room for a normal fill
    // and border, so they are rendered as a flat dark bar.
    float noteAreaPx = max(width - keyboardHeight, 1.0);
    bool dense = noteLength * noteAreaPx < 1.0;

    // Base note color comes from the low 4 bits of noteMeta.
    vec3 n_color = texture2D(
        noteColorTexture,
        vec2(float(noteMeta & uint(0xF)) / 16.0, 0.5)
    ).rgb;

    vec3 n_color2 = n_color * 0.5;

    // Velocity / intensity.
    float intensity =
        float((noteMeta & uint(0xFF0)) >> uint(4)) / 128.0;

    n_color = mix(vec3(1.0), n_color, intensity);

    // Special state color.
    if ((noteMeta & uint(1 << 13)) != uint(0)) {
        n_color = vec3(1.0, 0.5, 0.5);
        n_color2 = vec3(0.9, 0.4, 0.4);
    }

    // Gray factor:
    // 0.0 = original color
    // 0.5 = partially desaturated
    // 1.0 = fully grayscale
    //
    // Desaturate toward the color's own luminance instead of a fixed gray.
    // This preserves the note's perceived brightness and hue relationships
    // much better, especially for dense notes.
    float grayFactor =
        float((noteMeta & (uint(3) << uint(14))) >> uint(14)) / 2.0;

    float luminance =
        dot(n_color, vec3(0.299, 0.587, 0.114));

    float luminance2 =
        dot(n_color2, vec3(0.299, 0.587, 0.114));

    vec3 grayscale = vec3(luminance);
    vec3 grayscale2 = vec3(luminance2);

    n_color = mix(n_color, grayscale, grayFactor);
    n_color2 = mix(n_color2, grayscale2, grayFactor);

    // Sub-pixel notes intentionally get no highlight. Otherwise a playhead
    // crossing a dense wall of tiny notes can make a whole region flicker.
    if (!dense && (noteMeta & uint(1 << 12)) != uint(0)) {
        n_color += vec3(0.5);
        n_color2 += vec3(0.25);
    }

    // Dense notes are rendered as a flat dark bar.
    // This darkens whatever color/gray state was already calculated above,
    // without changing its saturation or gray factor.
    if (dense) {
        n_color *= 0.5;
        n_color2 *= 0.5;
    }

    color = n_color;
    color2 = n_color2;

    vec2 uv_;
    float x_pos;
    float y_pos;

    noteWidth = noteLength;
    noteHeight = noteTop - noteBottom;

    int vertex = gl_VertexID & 3;

    if (vertex == 0) {
        x_pos = noteStart;
        y_pos = noteBottom;
        uv_ = vec2(0.0, 0.0);
    } else if (vertex == 1) {
        x_pos = noteStart + noteLength;
        y_pos = noteBottom;
        uv_ = vec2(1.0, 0.0);
    } else if (vertex == 2) {
        x_pos = noteStart + noteLength;
        y_pos = noteTop;
        uv_ = vec2(1.0, 1.0);
    } else {
        x_pos = noteStart;
        y_pos = noteTop;
        uv_ = vec2(0.0, 1.0);
    }

    uv = uv_;

    float kbWidth = keyboardHeight / width;
    x_pos = x_pos * (1.0 - kbWidth) + kbWidth;

    gl_Position = vec4(
        vec2(x_pos, y_pos) * 2.0 - 1.0,
        noteDepth,
        1.0
    );
}