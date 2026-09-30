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
    if (uv.x * handleWidth <= border_width / width || (1.0 - uv.y) * handleHeight <= border_width / height) { 
        n_color = color;
        alpha = 1.0;
    }

    fragColor = vec4(n_color, alpha);
}