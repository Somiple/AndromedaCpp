#version 330

out vec4 fragColor;

in vec2 uv;
in float oddBarFac;
flat in int isCurrTrack;
in float bLength;
in float bHeight;

uniform float width;
uniform float height;
uniform float ppqNorm;

void main() {
    float beat_pos = uv.x * bLength / ppqNorm;
    int beat_int = int(beat_pos) % 2;
    float beat_odds_fac = (beat_int == 0) ? 0.9 : 1.0;

    vec3 color = vec3(0.2, 0.2, 0.25);
    color *= beat_odds_fac;
    color *= oddBarFac;

    if (isCurrTrack == 1) {
        color += vec3(0.15);
    }

    if (uv.x * bLength <= 2.75 / width) {
        color *= 0.1;
    }

    if (fract(beat_pos) * ppqNorm <= 2.0 / width) {
        color *= 0.1;
    }

    if (uv.y * bHeight <= 1.0 / height) {
        color *= 0.1;
    }
    
    fragColor = vec4(color, 1.0);
}