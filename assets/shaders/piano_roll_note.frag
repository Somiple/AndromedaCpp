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

    //Same rule as piano_roll_note_direct.frag: the border is drawn inside the
    //note, so a note without room for one on each side plus fill between them
    //is drawn flat. Otherwise the border swallows every thin note and paints it
    //in the dark edge colour, which is meant to be reserved for the sub-pixel
    //bars.
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
