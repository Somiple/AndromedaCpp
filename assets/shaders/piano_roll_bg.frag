#version 330

out vec4 fragColor;

in vec2 uv;
in float oddBarFac;
in float bLength;

uniform float width;
uniform float height;
uniform float ppqNorm;
uniform float keyZoom;

void main() {
    float key_pos = uv.y * 128.0;
    int key_int = int(key_pos) % 12;
    float key_sharp_fac = (key_int == 1 || key_int == 3 || key_int == 6 || key_int == 8 || key_int == 10) ? 0.9 : 1.0;

    float beat_pos = uv.x * bLength / ppqNorm;
    int beat_int = int(beat_pos) % 2;
    float beat_odds_fac = (beat_int == 0) ? 0.95 : 1.0;

    vec3 color = vec3(0.2, 0.2, 0.25);
    color *= key_sharp_fac;
    color *= beat_odds_fac;
    color *= oddBarFac;

    if (uv.x * bLength <= 3.0 / width) {
        color *= 0.1;
    }

    if (fract(beat_pos * 4.0) * ppqNorm <= 4.0 / width ||
        fract(beat_pos) * ppqNorm <= 2.0 / width) {
        color *= 0.1;
    }

    if (fract(key_pos) <= 128.0 / height * keyZoom) {
        color *= 0.6;
    }

    if (fract(key_pos / 12.0) <= (128.0 / 6.0) / height * keyZoom) {
        color *= 0.1;
    }

    fragColor = vec4(color, 1.0);
}