#version 330 core
in vec2 TexCoord;
out vec4 FragColor;
uniform sampler2D canvasTex;
uniform vec4 uvTransform;
uniform float materialAlpha = 1.0;
void main() {
    vec2 uv = TexCoord * uvTransform.xy + uvTransform.zw;
    // Transparent canvas corners and holes retain the geometry behind the UI.
    if (texture(canvasTex, uv).a * materialAlpha < 0.02) discard;
    // UI participates as a ray hit/occluder, never as a reflective receiver.
    FragColor = vec4(0.0, 0.0, 1.0, 0.0);
}
