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
//One depth slice per track, so the depth test can drop covered notes.
uniform float noteDepth;
uniform float width;

void main() {
    float noteStart = noteRect.x;
    float noteLength = noteRect.y;
    float noteBottom = noteRect.z;
    float noteTop = noteRect.w;

    //Same rule as piano_roll_note_direct.vert: a note whose note-on length puts
    //it under one pixel wide on screen has no shape left to draw, so it is
    //shaded as one flat dark bar with no highlight. `noteLength` is a fraction
    //of the note area, which is the window minus the keyboard.
    float noteAreaPx = max(width - keyboardHeight, 1.0);
    bool dense = noteLength * noteAreaPx < 1.0;

    vec3 n_color = texture2D(noteColorTexture, vec2(float(noteMeta & uint(0xF)) / 16.0, 0.5)).rgb;
    color2 = n_color * 0.5;

    n_color = mix(
        vec3(1.0),
        n_color,
        float((noteMeta & uint(0xFF0)) >> uint(4)) / 128.0
    );

    if ((noteMeta & uint(1 << 13)) != uint(0)) {
        n_color = vec3(1.0, 0.5, 0.5);
        color2 = vec3(0.9, 0.4, 0.4);
    }

    float grayFactor = float((noteMeta & (uint(3) << uint(14))) >> uint(14)) / 2.0;
    n_color = mix(n_color, vec3(0.5, 0.5, 0.5), grayFactor);
    color2 = mix(color2, vec3(0.5) / 2.0, grayFactor);

    //Sub-pixel notes get no highlight: the playhead crossing a wall of them
    //lights up whole regions at once, which reads as a flicker rather than as
    //the notes that are actually sounding.
    if (!dense && (noteMeta & uint(1 << 12)) != uint(0)) {
        n_color += vec3(0.5);
        color2 += 0.25;
    }

    //Below a pixel there is no room for a fill and a border, so the note is one
    //flat dark bar. Darkening what the passes above worked out, rather than
    //resetting to the track colour, keeps velocity, selection and the onion
    //wash visible in the shade.
    if (dense) {
        n_color *= 0.5;
        color2 = n_color;
    }

    color = n_color;

    // color = noteColor;
    // color2 = noteColor2;
    vec2 uv_;
    float x_pos = 0.0f;
    float y_pos = 0.0f;

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
    float kbWidth = keyboardHeight / width;
    x_pos = (x_pos * (1.0 - kbWidth)) + kbWidth;
    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, noteDepth, 1.0);
}