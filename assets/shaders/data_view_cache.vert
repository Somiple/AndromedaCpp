#version 330

// The velocity strip's handles, as last drawn, laid back over the strip.
//
// One triangle that covers the viewport: (0,0), (2,0), (0,2) in texture space.
// The cache texture is exactly the viewport's size, so every pixel centre lands
// on a texel centre and nearest sampling hands back that texel unchanged.

out vec2 uv;

void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
