#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

// One step up the bloom chain: tent-filter the smaller, already combined level
// and blend it over this level's downsample. The weights along the chain sum
// to one, so the result stays as bright as the thresholded scene.
uniform sampler2D lowTex;
uniform sampler2D highTex;
// Texel size of lowTex.
uniform vec2 lowTexelSize;
// How much of the wider levels reaches this one; higher spreads the glow further.
uniform float scatter = 0.6;

vec3 tap(vec2 offset) {
    return texture(lowTex, TexCoord + lowTexelSize * offset).rgb;
}

void main() {
    vec3 low = tap(vec2(0.0, 0.0)) * 4.0;
    low += (tap(vec2(-1.0, 0.0)) + tap(vec2(1.0, 0.0)) +
            tap(vec2(0.0, -1.0)) + tap(vec2(0.0, 1.0))) * 2.0;
    low += tap(vec2(-1.0, -1.0)) + tap(vec2(1.0, -1.0)) +
           tap(vec2(-1.0, 1.0)) + tap(vec2(1.0, 1.0));
    low *= 1.0 / 16.0;

    vec3 high = texture(highTex, TexCoord).rgb;
    FragColor = vec4(mix(high, low, scatter), 1.0);
}
