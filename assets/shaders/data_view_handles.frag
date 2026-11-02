#version 330

out vec4 fragColor;

in vec2 uv;
in vec3 color;
in vec3 color2;

in float handleWidth;
in float handleHeight;

uniform float width;
uniform float height;

void main() {
    vec3 n_color = vec3(0.0);
    float alpha = 0.0;
    float border_width = 2.0;

    float absHeight = abs(handleHeight);
    float distFromHandleTop = handleHeight >= 0.0
        ? (1.0 - uv.y) * absHeight
        : uv.y * absHeight;

    if (uv.x * handleWidth <= border_width / width ||
        distFromHandleTop <= border_width / height) {
        n_color = color;
        alpha = 1.0;
    }

    fragColor = vec4(n_color, alpha);
}