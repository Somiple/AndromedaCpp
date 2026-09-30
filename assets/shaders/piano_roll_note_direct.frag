#version 330

out vec4 fragColor;

in vec2 uv;
in vec3 color;
in vec3 color2;

in float noteWidth;
in float noteHeight;

uniform float width;
uniform float height;

void main() {
    vec3 n_color = color;

    //The border is drawn inside the note, so it needs a note big enough to hold
    //one on each side and still show fill between them. Below that there is
    //nothing to outline, and letting the border win paints the note entirely in
    //the dark edge colour - which is how a note that is merely thin (a short
    //one, or any note at all once the keys are zoomed down to a couple of
    //pixels) ended up as dark as the sub-pixel bars that are supposed to be the
    //only thing that means density. Each axis decides for itself: a long note
    //on a two-pixel key still has room for its left and right edges.
    float border_width = 1.0;
    float min_span = 3.0 * border_width;

    float bx = noteWidth * width >= min_span ? border_width / width : 0.0;
    float by = noteHeight * height >= min_span ? border_width / height : 0.0;

    if (uv.x * noteWidth < bx || (1.0 - uv.x) * noteWidth < bx ||
        uv.y * noteHeight < by || (1.0 - uv.y) * noteHeight < by) {
        n_color = color2;
    }

    fragColor = vec4(n_color, 1.0);
}
