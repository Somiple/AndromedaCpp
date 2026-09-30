#version 330

layout (location = 0) in uint kbMeta0;

out vec2 uv;
out vec3 keyColor;
out vec2 keyDims;
flat out int meta;

uniform float keyboardHeight;
uniform float prBarBottom;
uniform float prBarTop;
uniform float width;
uniform sampler2D noteColorTexture;

const int whiteIndex[12] = int[12](
    0, 0, 1, 1, 2,
    3, 3, 4, 4, 5, 5, 6
);

void main() {
    bool isBlack = (kbMeta0 & uint(1 << 31)) != 0u;
    meta = int(kbMeta0 >> 30u) & 3;

    float x_pos = 0.0f;
    float y_pos = 0.0f;

    uint key = kbMeta0 & uint(0x7F);
    float kbWidth = keyboardHeight / width;

    int octave = int(key) / 12;
    int n = int(key) % 12;

    float whiteWidth = 128.0 / 75.0;
    float blackScale = 0.65;

    float whiteKeys = 74.666666;
    float whiteHeight = 1.0 / whiteKeys;
    float whiteY = float(octave * 7 + whiteIndex[n]) * whiteHeight;

    float y_bottom = mix(prBarBottom, prBarTop, (isBlack ? (float(key) / 128.0) : whiteY));
    float y_top = mix(prBarBottom, prBarTop, (isBlack ? (float(key + 1u) / 128.0) : whiteY + whiteHeight));

    float x_left = 0.0;
    float x_right = isBlack ? kbWidth * blackScale : kbWidth;

    vec2 uv_ = vec2(0.0);
    if (int(gl_VertexID % 4) == 0) {
        x_pos = x_left;
        y_pos = y_bottom;
        uv_ = vec2(0.0, 0.0);
    } else if (int(gl_VertexID % 4) == 1) {
        x_pos = x_right;
        y_pos = y_bottom;
        uv_ = vec2(1.0, 0.0);
    } else if (int(gl_VertexID % 4) == 2) {
        x_pos = x_right;
        y_pos = y_top;
        uv_ = vec2(1.0, 1.0);
    } else if (int(gl_VertexID % 4) == 3) {
        x_pos = x_left;
        y_pos = y_top;
        uv_ = vec2(0.0, 1.0);
    }

    uv = uv_;
    keyColor = texture2D(noteColorTexture, vec2(float((kbMeta0 >> 7u) & uint(0xF)) / 16.0, 0.5)).rgb;
    keyDims = vec2(x_right - x_left, y_top - y_bottom);

    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, 0.0, 1.0);
}