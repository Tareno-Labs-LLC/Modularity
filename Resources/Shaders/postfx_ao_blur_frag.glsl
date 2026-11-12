#version 330 core
in vec2 TexCoord;
out vec2 FragColor;
uniform sampler2D sourceTex;
uniform vec2 stepUv;
uniform float radius;
uniform vec2 sourceScale = vec2(1.0);
uniform vec2 sourceTexelSize;
vec2 boundedUv(vec2 uv) {
    return clamp(uv, sourceTexelSize * 0.5, sourceScale - sourceTexelSize * 0.5);
}
void main() {
    vec2 uv = TexCoord * sourceScale;
    vec2 center = texture(sourceTex, boundedUv(uv)).rg;
    if (center.y <= 0.0) { FragColor = center; return; }
    float sum = center.x;
    float weightSum = 1.0;
    float tolerance = max(radius * 0.12, center.y * 0.002);
    for (int i = -3; i <= 3; ++i) {
        if (i == 0) continue;
        vec2 tap = texture(sourceTex, boundedUv(uv + stepUv * float(i))).rg;
        float difference = abs(tap.y - center.y) / max(tolerance, 0.0001);
        float weight = exp2(-float(i * i) * 0.22 - difference * difference);
        weight *= step(0.00001, tap.y);
        sum += tap.x * weight;
        weightSum += weight;
    }
    // Preserve center depth for the full-resolution joint bilateral upsample.
    FragColor = vec2(sum / weightSum, center.y);
}
