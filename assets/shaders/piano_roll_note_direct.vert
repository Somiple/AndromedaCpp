#version 330

// Same output as piano_roll_note.vert, but fed raw note data instead of a
// CPU-built instance.
//
// There are no vertex attributes at all. Instancing this quad meant four
// vertices and six indices per instance, which is far too little work to fill a
// warp - measured at ~6 ns per vertex on a 5090, with the fragment stage proven
// (by collapsing every quad to zero area, which changed nothing) to cost none of
// it. So the draw is one flat glDrawArrays of count*6 vertices and the note is
// fetched from the resident buffer, bound as a texture buffer, by index:
//
//   texel .x = uint start
//   texel .y = uint length
//   texel .z = key | velocity << 8 | channel << 16
//
// Everything the CPU used to compute per note - screen rect, colour index,
// velocity shading, playing/selected flags - happens here instead.

out vec2 uv;
out vec3 color;
out vec3 color2;

out float noteWidth;
out float noteHeight;

uniform sampler2D noteColorTexture;
uniform usamplerBuffer selectedBits;
//midi::Note itself, three 32-bit words wide.
uniform usamplerBuffer noteData;
//The survivors of the screen-space coverage pass, as absolute note indices.
//Only bound for a track that pass thinned out; `indexed` says which it is.
uniform usamplerBuffer noteIndex;
uniform int indexed;

uniform float width;
uniform float keyboardHeight;
//One depth slice per track, so the depth test can drop covered notes.
uniform float noteDepth;

uniform float tickPos;      // left edge of the view, in ticks
uniform float zoomTicks;
uniform float keyPos;       // bottom of the view, in keys
uniform float zoomKeys;

uniform float playbackPos;
uniform float highlightSize;
uniform int   isPlaying;

// 0 = by channel, 1 = by track, 2 = by channel+track. Mirrors NoteColors::get_index.
uniform int  colorMode;
uniform int  trackIndex;
uniform uint onionColorMeta; // 0 full colour, 1 partial, 2 grey
uniform int  selectionEnabled;
uniform uint firstNote;      // absolute index of the draw's first note
//Where this track's notes start inside the slab they share with other tracks.
//Only the note fetch is offset by it: the selection bitmap and the survivor
//list are both indexed within the track.
uniform uint noteBase;

//Two triangles, in the winding the index buffer used to supply: 0 1 2, 0 2 3.
const vec2 CORNERS[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    int  local  = gl_VertexID / 6;
    int  corner = gl_VertexID - local * 6;

    //`firstNote` walks the survivor list on the indexed path and the note buffer
    //itself on the flat one; either way it is what splits a track too long for a
    //signed vertex count into chunks.
    uint index;
    if (indexed != 0) {
        index = texelFetch(noteIndex, int(firstNote) + local).r;
    } else {
        index = firstNote + uint(local);
    }

    uvec4 raw = texelFetch(noteData, int(noteBase + index));

    float noteStart  = float(raw.x);
    float noteLength = float(raw.y);

    float key      = float(raw.z & 255u);
    float velocity = float((raw.z >> 8u) & 255u);
    uint  channel  = (raw.z >> 16u) & 255u;

    float rectStart  = (noteStart - tickPos) / zoomTicks;
    float rectLength = noteLength / zoomTicks;
    float rectBottom = (key - keyPos) / zoomKeys;
    float rectTop    = ((key + 1.0) - keyPos) / zoomKeys;

    //Density is a property of the picture, not of the note data: a note whose
    //note-on length puts it under one pixel wide has no shape left to draw.
    //`rectLength` is a fraction of the note area, which is the window minus the
    //keyboard.
    float noteAreaPx = max(width - keyboardHeight, 1.0);
    bool  dense      = rectLength * noteAreaPx < 1.0;

    uint colorIndex;
    if (colorMode == 1) {
        colorIndex = uint(trackIndex) & 15u;
    } else if (colorMode == 2) {
        colorIndex = (uint(trackIndex) + channel) & 15u;
    } else {
        colorIndex = channel & 15u;
    }

    vec3 n_color = texture2D(noteColorTexture, vec2(float(colorIndex) / 16.0, 0.5)).rgb;
    color2 = n_color * 0.5;

    n_color = mix(vec3(1.0), n_color, velocity / 128.0);

    bool selected = false;
    if (selectionEnabled != 0) {
        uint word = texelFetch(selectedBits, int(index >> 5u)).r;
        selected = ((word >> (index & 31u)) & 1u) != 0u;
    }

    if (selected) {
        n_color = vec3(1.0, 0.5, 0.5);
        color2 = vec3(0.9, 0.4, 0.4);
    }

    float grayFactor = float(onionColorMeta) / 2.0;
    n_color = mix(n_color, vec3(0.5, 0.5, 0.5), grayFactor);
    color2 = mix(color2, vec3(0.5) / 2.0, grayFactor);

    //A sub-pixel note is never highlighted: the playhead sweeping a wall of
    //them lights up whole regions at once, which reads as a flicker rather than
    //as the notes that are actually sounding.
    float noteEnd = noteStart + noteLength;
    bool playing = !dense && isPlaying != 0 &&
                   noteStart < playbackPos + highlightSize &&
                   noteEnd > playbackPos - highlightSize;
    if (playing) {
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

    noteWidth = rectLength;
    noteHeight = rectTop - rectBottom;

    vec2 c = CORNERS[corner];
    uv = c;

    float x_pos = rectStart + rectLength * c.x;
    float y_pos = mix(rectBottom, rectTop, c.y);

    float kbWidth = keyboardHeight / width;
    x_pos = (x_pos * (1.0 - kbWidth)) + kbWidth;
    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, noteDepth, 1.0);
}
