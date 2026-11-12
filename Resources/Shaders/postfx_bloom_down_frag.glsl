#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

// One step down the bloom chain: a 13-tap filter (Jimenez, "Next Generation
// Post Processing in Call of Duty") built from bilinear taps, so a half-size
// target reads a 6x6 source footprint without aliasing.
uniform sampler2D sourceTex;
// Texel size of sourceTex, the level being read.
uniform vec2 sourceTexelSize;
// First step only: Karis-average each tap group so a lone bright pixel cannot
// flicker the whole bloom, then apply the soft-knee threshold.
uniform bool prefilter = false;
uniform float threshold = 1.0;
uniform float softKnee = 0.25;

float luma(vec3 c) {
    return dot(c, vec3(0.2125, 0.7154, 0.0721));
}

vec3 applyThreshold(vec3 c) {
    float l = luma(c);
    float knee = max(threshold * softKnee, 1e-4);
    float soft = clamp(l - threshold + knee, 0.0, 2.0 * knee);
    soft = (soft * soft) / max(4.0 * knee + 1e-4, 1e-4);
    float contribution = max(soft, l - threshold);
    contribution /= max(l, 1e-4);
    return c * clamp(contribution, 0.0, 1.0);
}

vec3 tap(vec2 offset) {
    return texture(sourceTex, TexCoord + sourceTexelSize * offset).rgb;
}

void main() {
    vec3 a = tap(vec2(-2.0,  2.0));
    vec3 b = tap(vec2( 0.0,  2.0));
    vec3 c = tap(vec2( 2.0,  2.0));
    vec3 d = tap(vec2(-2.0,  0.0));
    vec3 e = tap(vec2( 0.0,  0.0));
    vec3 f = tap(vec2( 2.0,  0.0));
    vec3 g = tap(vec2(-2.0, -2.0));
    vec3 h = tap(vec2( 0.0, -2.0));
    vec3 i = tap(vec2( 2.0, -2.0));
    vec3 j = tap(vec2(-1.0,  1.0));
    vec3 k = tap(vec2( 1.0,  1.0));
    vec3 l = tap(vec2(-1.0, -1.0));
    vec3 m = tap(vec2( 1.0, -1.0));

    // The inner box carries half the weight, the four overlapping corner boxes
    // an eighth each.
    vec3 inner = (j + k + l + m) * 0.25;
    vec3 topLeft = (a + b + d + e) * 0.25;
    vec3 topRight = (b + c + e + f) * 0.25;
    vec3 bottomLeft = (d + e + g + h) * 0.25;
    vec3 bottomRight = (e + f + h + i) * 0.25;

    vec3 color;
    if (prefilter) {
        float w0 = 0.5 / (1.0 + luma(inner));
        float w1 = 0.125 / (1.0 + luma(topLeft));
        float w2 = 0.125 / (1.0 + luma(topRight));
        float w3 = 0.125 / (1.0 + luma(bottomLeft));
        float w4 = 0.125 / (1.0 + luma(bottomRight));
        color = (inner * w0 + topLeft * w1 + topRight * w2 + bottomLeft * w3 + bottomRight * w4) /
                (w0 + w1 + w2 + w3 + w4);
        // HDR sources can hold inf/NaN from a bad pixel; one of those would
        // spread over the whole chain.
        if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
        color = applyThreshold(min(color, vec3(65000.0)));
    } else {
        color = inner * 0.5 + (topLeft + topRight + bottomLeft + bottomRight) * 0.125;
    }

    FragColor = vec4(max(color, vec3(0.0)), 1.0);
}
