#version 330 core
in vec2 TexCoord;
out vec4 FragColor;
uniform vec4 lineColor;
uniform bool reticle = false;
void main() {
    float coverage = reticle
        ? 1.0 - smoothstep(0.7, 1.0, length(TexCoord * 2.0 - 1.0))
        : smoothstep(0.0, 0.22, TexCoord.x) * smoothstep(0.0, 0.22, 1.0 - TexCoord.x);
    if (coverage <= 0.001) discard;
    FragColor = vec4(lineColor.rgb, lineColor.a * coverage);
}
