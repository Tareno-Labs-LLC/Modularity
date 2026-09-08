#version 330 core
// Depth-aware separable blur for the volumetric buffer.
//
// The march offsets each pixel's first sample by a per-pixel dither so the
// integration error shows up as fine grain rather than as concentric bands.
// That trade is the right one - grain is far less objectionable than rings -
// but the grain is still there, and it is worst exactly where the effect is
// strongest and the step count lowest. This pass averages it away.
//
// Run twice, horizontally then vertically. A separable pass is 2*(2R+1) taps
// against (2R+1)^2 for the equivalent 2D kernel - at radius 4 that is 18 taps
// instead of 81, which is the difference between affordable and not on mobile.
//
// The depth term is what keeps it from being a plain blur: without it a shaft
// bleeds across the silhouette of whatever is in front of it, and the halo
// reads as worse than the noise it replaced.
out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D sourceTex;
uniform sampler2D sceneDepth;

// One texel step along the axis being blurred, in the SOURCE buffer's texel
// size - which is half-resolution when the volumetric pass is, so this has to
// be passed in rather than derived from the depth buffer.
uniform vec2 blurDirection = vec2(0.0);
uniform int radius = 3;
uniform float nearPlane = 0.1;
uniform float farPlane = 100.0;

// Perspective depth is wildly non-linear, so comparing raw values would make
// the edge test hypersensitive up close and useless far away.
float linearizeDepth(float d)
{
    float z = d * 2.0 - 1.0;
    return (2.0 * nearPlane * farPlane) /
           (farPlane + nearPlane - z * (farPlane - nearPlane));
}

void main()
{
    vec3 centerColor = texture(sourceTex, TexCoord).rgb;
    float centerDepth = linearizeDepth(texture(sceneDepth, TexCoord).r);

    // Gaussian sigma tied to the radius, so widening the kernel widens the
    // falloff instead of flattening it into a box blur.
    float sigma = max(float(radius) * 0.5, 0.5);
    float twoSigmaSq = 2.0 * sigma * sigma;

    vec3 sum = centerColor;
    float weightSum = 1.0;

    int r = clamp(radius, 1, 8);
    for (int i = 1; i <= 8; ++i) {
        if (i > r) break;
        float spatial = exp(-float(i * i) / twoSigmaSq);

        // Both sides in one iteration - the weights are symmetric, so this
        // halves the loop count for free.
        for (int side = 0; side < 2; ++side) {
            vec2 uv = TexCoord + blurDirection * float(i) * (side == 0 ? 1.0 : -1.0);
            float d = linearizeDepth(texture(sceneDepth, uv).r);

            // Relative rather than absolute depth difference, which makes the
            // edge threshold scale-invariant: a 10cm gap matters at 1m and does
            // not at 100m, and a fixed sigma would have to be retuned for every
            // scene.
            float depthWeight = exp(-abs(d - centerDepth) / (centerDepth * 0.1 + 0.01));
            float w = spatial * depthWeight;

            sum += texture(sourceTex, uv).rgb * w;
            weightSum += w;
        }
    }

    FragColor = vec4(sum / max(weightSum, 1e-4), 1.0);
}
