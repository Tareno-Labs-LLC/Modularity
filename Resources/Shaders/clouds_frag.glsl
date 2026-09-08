#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform bool unlit = false;
uniform vec4 uvTransform = vec4(1.0, 1.0, 0.0, 0.0);

uniform float uTime = 0.0;
uniform float uScrollSpeed = 0.05;         // UV units per second along uScrollDir
uniform vec2 uScrollDir = vec2(1.0, 0.0);  // drift axis, normalized below
uniform vec3 materialColor = vec3(1.0);
uniform float materialAlpha = 1.0;
uniform float ambientStrength = 1.0;
uniform float specularStrength = 0.0;
uniform float shininess = 32.0;
uniform vec3 viewPos;

// --- Procedural Clouds controls -------------------------------------------
uniform vec3 uCloudColor = vec3(0.85, 0.36, 0.96);     // lit / bright cloud body
uniform vec3 uCloudSkyColor = vec3(0.10, 0.02, 0.14);  // gap colour behind the clouds
uniform float uCloudScale = 3.0;       // how many cloud cells fit across the surface
uniform float uCloudCoverage = 0.5;    // 0 = clear sky, 1 = fully overcast
uniform float uCloudSoftness = 0.35;   // edge falloff width; small = crisp puffs
uniform int uCloudDetail = 5;          // fbm octaves, 1..8
uniform float uCloudSpeed = 0.15;      // billow / churn rate, independent of scroll
uniform float uCloudWarp = 0.35;       // domain warp; curls the noise into wisps
uniform float uCloudHighlight = 1.35;  // extra lift in the densest cores
uniform float uCloudStars = 0.0;       // 0 disables the starfield behind the clouds
uniform float uCloudHorizon = 0.0;     // 0 disables the horizon gradient

float cloudHash(vec2 p)
{
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float cloudNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = cloudHash(i);
    float b = cloudHash(i + vec2(1.0, 0.0));
    float c = cloudHash(i + vec2(0.0, 1.0));
    float d = cloudHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float cloudFbm(vec2 p, int octaves)
{
    float sum = 0.0;
    float amp = 0.5;
    float norm = 0.0;
    // Fixed trip count keeps this friendly to older GLSL compilers; the break
    // is what actually honours uCloudDetail.
    for (int i = 0; i < 8; ++i) {
        if (i >= octaves) break;
        sum += cloudNoise(p) * amp;
        norm += amp;
        p = p * 2.02 + vec2(17.3, 9.1);
        amp *= 0.5;
    }
    return (norm > 0.0) ? sum / norm : 0.0;
}

void main()
{
    vec2 uv = TexCoord * uvTransform.xy + uvTransform.zw;
    // pure direction; a zero vector just parks the scroll instead of NaN-ing
    vec2 dir = (length(uScrollDir) > 0.0001) ? normalize(uScrollDir) : vec2(0.0);

    int octaves = clamp(uCloudDetail, 1, 8);
    float scale = max(uCloudScale, 0.0001);
    vec2 p = uv * scale + dir * (uTime * uScrollSpeed * scale);
    float evolve = uTime * uCloudSpeed;

    // Domain warp: without it fbm reads as flat static, with it the layers curl
    // into the wispy billows the reference sky has.
    float warpAmount = max(uCloudWarp, 0.0);
    vec2 warped = p;
    if (warpAmount > 0.0) {
        vec2 warp = vec2(cloudFbm(p + vec2(0.0, evolve), octaves),
                         cloudFbm(p + vec2(5.2, 1.3) - vec2(evolve, 0.0), octaves));
        warped = p + (warp - 0.5) * 2.0 * warpAmount;
    }

    float density = cloudFbm(warped + vec2(evolve * 0.5, -evolve * 0.35), octaves);

    // Coverage slides the cut-off through the noise range; softness widens the ramp.
    float coverage = clamp(uCloudCoverage, 0.0, 1.0);
    float soft = max(uCloudSoftness, 0.001);
    float threshold = mix(0.85, 0.10, coverage);
    float mask = smoothstep(threshold, threshold + soft, density);
    float core = smoothstep(threshold + soft * 0.5, threshold + soft * 2.0, density);

    // Horizon mode thins the clouds towards the top of the surface and leaves a
    // lit band where they meet the sky. Uses raw TexCoord so UV tiling does not
    // repeat the horizon.
    float horizon = clamp(uCloudHorizon, 0.0, 1.0);
    float skyLift = 0.0;
    if (horizon > 0.0) {
        float height = clamp(TexCoord.y, 0.0, 1.0);
        float fade = smoothstep(0.15, 0.75, height);
        mask *= mix(1.0, 1.0 - fade, horizon);
        core *= mix(1.0, 1.0 - fade, horizon);
        skyLift = horizon * (1.0 - smoothstep(0.0, 0.45, height)) * 0.6;
    }

    vec3 sky = mix(uCloudSkyColor, mix(uCloudSkyColor, uCloudColor, 0.45), skyLift);
    vec3 color = mix(sky, uCloudColor, mask);
    color += uCloudColor * core * max(uCloudHighlight - 1.0, 0.0);

    float stars = clamp(uCloudStars, 0.0, 1.0);
    if (stars > 0.0) {
        // One candidate star per cell, jittered so the grid does not show.
        const float cells = 150.0;
        vec2 sp = TexCoord * cells + dir * (uTime * uScrollSpeed * cells * 0.06);
        vec2 cell = floor(sp);
        float r1 = cloudHash(cell);
        float r2 = cloudHash(cell + vec2(7.7, 3.3));
        vec2 jitter = (vec2(r2, cloudHash(cell + vec2(1.9, 8.4))) - 0.5) * 0.7;
        float d = length(fract(sp) - 0.5 - jitter);
        float present = step(1.0 - stars * 0.45, r1);
        float twinkle = 0.6 + 0.4 * sin(uTime * (1.5 + r2 * 3.0) + r1 * 6.2831);
        // Squared so even thin cloud puts the starfield behind it.
        float behindClouds = (1.0 - mask) * (1.0 - mask);
        color += vec3(smoothstep(0.22, 0.0, d) * present * twinkle) * behindClouds;
    }

    color *= materialColor;
    float alpha = materialAlpha;

    if (unlit) {
        FragColor = vec4(color, alpha);
        return;
    }

    vec3 N = normalize(Normal);
    vec3 L = normalize(vec3(0.35, 0.9, 0.2));
    vec3 V = normalize(viewPos - FragPos);
    vec3 H = normalize(L + V);

    float diffuse = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), max(shininess, 1.0)) * clamp(specularStrength, 0.0, 2.0);
    vec3 lit = color * (clamp(ambientStrength, 0.0, 1.0) + diffuse) + vec3(spec);

    FragColor = vec4(lit, alpha);
}
