#version 330

// The velocity strip, fed the resident note buffer instead of a CPU-built
// instance - the same change piano_roll_note_direct.vert is to
// piano_roll_note.vert, and for the same reason: the strip's geometry is
// derived entirely from `start`, `length` and `velocity`, all of which the GPU
// already holds.
//
//   texel .x = uint start
//   texel .y = uint length
//   texel .z = key | velocity << 8 | channel << 16
//
// There are no vertex attributes. One flat glDrawArrays of count*12 vertices
// draws a track; the note is fetched by index.
//
// Twelve, not six: a handle is drawn as the two strips that are ever visible
// rather than as its whole box. data_view_handles.frag paints only the box's
// left 2px and top 2px and writes alpha 0 everywhere inside, so every other
// fragment of the box was rasterised, shaded and blended to leave the pixel as
// it was. On a file of long notes that inside is nearly all of the strip's
// cost - thousands of full-height boxes stacked on every column. The two
// strips cover exactly the pixels the border test passed, so the picture is
// the same and the fill is the perimeter instead of the area.

out vec3 color;

uniform sampler2D noteColorTexture;
uniform usamplerBuffer selectedBits;
uniform usamplerBuffer noteData;

uniform float tickPos;
uniform float zoomTicks;

// 0 = by channel, 1 = by track, 2 = by channel+track. Mirrors NoteColors::get_index.
uniform int  colorMode;
uniform int  trackIndex;
uniform uint onionColorMeta; // 0 full colour, 1 partial, 2 grey
uniform int  selectionEnabled;
uniform uint firstNote;      // absolute index of the draw's first note
uniform uint noteStride;     // every n-th note: the strip's density cap, 1 below it
uniform uint noteBase;       // where this track starts inside its slab

uniform float playbackPos;
uniform float highlightSize;
uniform int   isPlaying;

// The strip's size in pixels, which is what the border's 2px are measured in.
uniform float width;
uniform float height;

const float BORDER_PX = 2.0;

//Two triangles, in the winding the index buffer used to supply.
const vec2 CORNERS[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    int  local  = gl_VertexID / 12;
    int  corner = gl_VertexID - local * 12;

    uint index = firstNote + uint(local) * noteStride;
    uvec4 raw = texelFetch(noteData, int(noteBase + index));

    float noteStart  = float(raw.x);
    float noteLength = float(raw.y);
    float velocity   = float((raw.z >> 8u) & 255u);
    uint  channel    = (raw.z >> 16u) & 255u;

    uint colorIndex;
    if (colorMode == 1) {
        colorIndex = uint(trackIndex) & 15u;
    } else if (colorMode == 2) {
        colorIndex = (uint(trackIndex) + channel) & 15u;
    } else {
        colorIndex = channel & 15u;
    }

    vec3 n_color = texture2D(noteColorTexture, vec2(float(colorIndex) / 16.0, 0.5)).rgb;

    //data_view_handles.vert: the handle darkens with velocity rather than
    //lightening, which is the opposite of the piano roll's shading.
    n_color = mix(n_color / 2.0, n_color, velocity / 128.0);

    if (selectionEnabled != 0) {
        uint word = texelFetch(selectedBits, int(index >> 5u)).r;
        if (((word >> (index & 31u)) & 1u) != 0u) {
            n_color = vec3(1.0, 0.5, 0.5);
        }
    }

    float grayFactor = float(onionColorMeta) / 2.0;
    n_color = mix(n_color, vec3(0.5, 0.5, 0.5), grayFactor);

    float noteEnd = noteStart + noteLength;
    if (isPlaying != 0 && noteStart < playbackPos + highlightSize &&
        noteEnd > playbackPos - highlightSize) {
        n_color += vec3(0.5);
    }

    color = n_color;

    //The handle: a bar from the bottom of the strip up to the velocity, of
    //which only the left and top edges are ever painted.
    float handleStart  = (noteStart - tickPos) / zoomTicks;
    float handleLength = noteLength / zoomTicks;
    float handleTop    = velocity / 127.0;

    //The border test was `uv.x * width <= 2px` and `(1 - uv.y) * height <= 2px`
    //inside the box, so each strip is 2px or the whole box if it is thinner.
    //
    //`<=` includes a pixel centre sitting exactly 2px in, and rasterising a
    //strip that ends there does not - which on a dense strip picked a different
    //handle for a whole column of pixels here and there. A hair past 2px puts
    //that centre back inside.
    const float EDGE_SLACK_PX = 0.001;
    float edgeW = min((BORDER_PX + EDGE_SLACK_PX) / width, handleLength);
    float edgeH = min((BORDER_PX + EDGE_SLACK_PX) / height, handleTop);

    //Vertices 0-5 are the left edge, 6-11 the top edge.
    int quad = corner / 6;
    vec2 c = CORNERS[corner - quad * 6];

    vec2 lo;
    vec2 hi;
    if (quad == 0) {
        lo = vec2(handleStart, 0.0);
        hi = vec2(handleStart + edgeW, handleTop);
    } else {
        lo = vec2(handleStart, handleTop - edgeH);
        hi = vec2(handleStart + handleLength, handleTop);
    }

    float x_pos = mix(lo.x, hi.x, c.x);
    float y_pos = mix(lo.y, hi.y, c.y);

    gl_Position = vec4(vec2(x_pos, y_pos) * 2.0 - 1.0, 0.0, 1.0);
}
