#version 330

// Handles are opaque (alpha 1) and the rest of the cache is cleared to 0, so
// with the strip's premultiplied blending a handle texel replaces the bar
// underneath and an empty one leaves it alone - what drawing the handles
// themselves did.

in vec2 uv;

out vec4 fragColor;

uniform sampler2D handles;

void main() {
    fragColor = texture(handles, uv);
}
