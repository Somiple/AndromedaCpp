#version 330

out vec4 fragColor;

in vec2 uv;
in vec3 keyColor;
in vec2 keyDims;
flat in int meta;

uniform float width;
uniform float height;

void main() {
    float w = 1.0 / width;
    float h = 1.0 / height;
    
    float keyWidth = keyDims.x;
    float keyHeight = keyDims.y;

    vec3 color = vec3(0.0);
    if ((meta & 1) == 0) { color = ((meta & 2) != 0) ? vec3(0.1) : vec3(1.0); }
    else { color = keyColor; }

    if (uv.y * keyHeight <= h || (uv.x >= 0.95 && uv.x <= 0.95 + w / keyWidth)) color *= 0.5;
    if (uv.x > 0.95) color *= 0.7;
    fragColor = vec4(color, 1.0); 
}