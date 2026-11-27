#version 330 core
// Screen-space surface buffer for the reflection and GI passes.
//
// Deliberately a separate prepass over the same draw list rather than a second
// colour attachment on the main pass. The mesh loop runs an open set of material
// shaders (clouds, scrolling UV, unlit, the 2.5D pair, and anything a project
// supplies); a fragment shader that does not write an output leaves that
// attachment undefined, so bolting MRT onto the main pass would mean editing
// every one of them and hoping no project ever adds another. This pairs with the
// existing vert.glsl / skinned_vert.glsl unchanged - it only needs FragPos,
// Normal and TexCoord, which both already provide - and only runs when a pass
// that reads it is switched on.
//
// Layout: octahedral normal in RG, roughness in B, metallic in A. Octahedral
// costs one channel less than storing the vector, which is what makes all four
// values fit in a single RGBA16F target.
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform vec4 uvTransform = vec4(1.0, 1.0, 0.0, 0.0);

uniform sampler2D normalMap;
uniform sampler2D metallicRoughnessMap;
uniform sampler2D metallicMap;
uniform sampler2D roughnessMap;
uniform bool hasMetallicMap = false;
uniform bool hasRoughnessMap = false;
uniform sampler2D heightMap;
uniform bool hasNormalMap = false;
uniform bool hasMetallicRoughnessMap = false;
uniform bool hasHeightMap = false;
uniform float normalMapIntensity = 1.0;
uniform float heightScale = 0.0;
uniform int parallaxSteps = 32;
uniform bool parallaxInvertHeight = false;

uniform vec3 viewPos;
uniform bool usePbr = false;
uniform float pbrMetallic = 0.0;
uniform float pbrRoughness = 0.5;
uniform float specularStrength = 0.5;
uniform float shininess = 32.0;

// Clearcoat and wetness, mirrored from frag.glsl: where a coat (lacquer or
// water) dominates, SSR has to trace the coat's roughness and normal, since that
// is the reflection the forward pass shows.
uniform float clearcoat = 0.0;
uniform float clearcoatRoughness = 0.04;
uniform float surfaceWetness = 0.0;
uniform float rainAmount = 0.0;
uniform float puddleAmount = 0.5;
uniform float wetTime = 0.0;
uniform bool wetnessReceive = true;

// -- Wet surfaces --
//
// Rain wetness, standing water and ripples, all procedural in world space so
// nothing has to be authored per material. Kept identical in frag.glsl and
// gbuffer_frag.glsl (GLSL 330 has no include): SSR reads roughness and normal
// from the G-buffer, so if the two diverge the reflections stop matching the
// surface they sit on.
//
// The water is modelled as a coat over the base material: a thin film on a
// merely wet surface (rough, partial), a flat near-mirror sheet in a puddle.
// That reuses the clearcoat lobe instead of adding a third one.
float wetHash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 wetHash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float wetValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = wetHash12(i);
    float b = wetHash12(i + vec2(1.0, 0.0));
    float c = wetHash12(i + vec2(0.0, 1.0));
    float d = wetHash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// One layer of expanding rain rings: at most one drop per cell per period,
// re-rolled every strike so the pattern never visibly loops. The ring never
// leaves its own cell, so no neighbour cells are visited. Returns the slope of
// the water surface in world XZ.
vec2 rainRippleLayer(vec2 p, float t, float density)
{
    vec2 cell = floor(p);
    float cycle = t + wetHash12(cell) * 7.31;
    float phase = fract(cycle);
    vec2 h = wetHash22(cell + floor(cycle) * vec2(0.618, 1.414));
    if (h.x > density) return vec2(0.0);
    vec2 d = p - (cell + 0.3 + h * 0.4);
    float r = length(d);
    float x = (r - phase * 0.3) * 24.0;
    if (abs(x) > 3.14159) return vec2(0.0);
    float fade = 1.0 - phase;
    float wave = sin(x) * (1.0 - abs(x) * 0.3183) * fade * fade;
    return d / max(r, 1e-4) * wave;
}

vec2 rainRipples(vec2 xz, float t, float amount)
{
    vec2 p = xz * 3.3;
    float density = clamp(amount * 0.85 + 0.1, 0.0, 1.0);
    vec2 s = rainRippleLayer(p, t * 1.1, density);
    s += rainRippleLayer(p * 1.17 + vec2(0.37, 0.61), t * 0.93 + 0.33, density);
    s += rainRippleLayer(p * 0.83 + vec2(0.71, 0.19), t * 1.27 + 0.66, density);
    return s;
}

// wet: 0..1 global wetness. Modifies the base layer (albedo darkening, lower
// roughness, flattened normal in puddles) and raises the coat to the water
// film where that is stronger than the material's own clearcoat.
void applySurfaceWetness(vec3 P, vec3 geomN, float wet, float rain, float time,
                         float puddles, float metallic,
                         inout vec3 albedo, inout float roughness, inout vec3 norm,
                         inout float coat, inout float coatRough, inout vec3 coatN)
{
    float up = geomN.y;
    // Rough dielectrics soak water in; metals, glass and lacquer do not.
    float porosity = (1.0 - metallic) * smoothstep(0.15, 0.6, roughness);
    // Undersides stay mostly dry, walls catch less than the ground.
    float wetLevel = wet * (up > -0.25 ? mix(0.6, 1.0, clamp(up, 0.0, 1.0)) : 0.25);

    float vertical = 1.0 - abs(up);
    if (vertical > 0.5) {
        // Run-off streaks down walls: noise stretched vertically and sliding
        // down while it rains.
        vec2 along = vec2(-geomN.z, geomN.x);
        float streak = wetValueNoise(vec2(dot(P.xz, along) * 9.0,
                                          P.y * 1.4 + time * 0.6 * rain));
        wetLevel *= mix(1.0, mix(0.5, 1.25, streak), smoothstep(0.5, 0.9, vertical));
    }

    float puddle = 0.0;
    if (up > 0.7 && puddles > 0.0) {
        float gate = clamp(porosity * 2.0, 0.0, 1.0) * (1.0 - coat);
        if (gate > 0.01) {
            float n = wetValueNoise(P.xz * 0.21) * 0.65 + wetValueNoise(P.xz * 0.83 + 17.0) * 0.35;
            float coverage = puddles * smoothstep(0.3, 1.0, wet);
            float threshold = 1.0 - coverage * 0.75;
            float upMask = smoothstep(0.7, 0.92, up) * gate;
            puddle = smoothstep(threshold, threshold + 0.04, n) * upMask;
            // A damp ring around each puddle.
            wetLevel = max(wetLevel, smoothstep(threshold - 0.15, threshold, n) * upMask * wet);
        }
    }
    wetLevel = clamp(wetLevel, 0.0, 1.0);

    float soak = max(wetLevel, puddle);
    albedo *= 1.0 - 0.5 * porosity * soak;
    roughness = mix(roughness, max(0.08, roughness * 0.5), wetLevel);
    roughness = mix(roughness, 0.1, puddle);
    // Standing water hides the surface detail below it.
    norm = normalize(mix(norm, geomN, puddle));

    vec3 waterN = geomN;
    if (rain > 0.01 && up > 0.6) {
        vec2 slope = rainRipples(P.xz, time, rain) * mix(0.12, 0.35, puddle);
        waterN = normalize(geomN + vec3(-slope.x, 0.0, -slope.y));
    }

    float film = max(wetLevel * 0.85, puddle);
    if (film > coat) {
        coatRough = mix(mix(0.3, 0.1, wetLevel), 0.02, puddle);
        coat = film;
        coatN = waterN;
    }
    if (porosity < 0.3 && wetLevel > 0.05 && puddle < 0.5) {
        // Lacquer, metal and glass shed water into beads. Each bead is a tiny
        // lens: its normal tilts away from its centre, which breaks the coat's
        // reflection up into droplets. One cell per pixel, projected on the
        // dominant axis.
        vec3 an = abs(geomN);
        vec2 bp;
        vec3 ax;
        vec3 ay;
        if (an.y >= an.x && an.y >= an.z) { bp = P.xz; ax = vec3(1.0, 0.0, 0.0); ay = vec3(0.0, 0.0, 1.0); }
        else if (an.x >= an.z)            { bp = P.zy; ax = vec3(0.0, 0.0, 1.0); ay = vec3(0.0, 1.0, 0.0); }
        else                              { bp = P.xy; ax = vec3(1.0, 0.0, 0.0); ay = vec3(0.0, 1.0, 0.0); }
        bp *= 32.0;
        vec2 cell = floor(bp);
        vec2 h = wetHash22(cell);
        vec2 d = fract(bp) - (0.3 + h * 0.4);
        float radius = 0.1 + 0.18 * h.y;
        float r2 = dot(d, d) / (radius * radius);
        if (wetHash12(cell + 7.7) < wetLevel * 0.7 && r2 < 1.0) {
            coatN = normalize(coatN + (ax * d.x + ay * d.y) * (0.9 / radius));
        }
    }
}

vec2 octWrap(vec2 v)
{
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0,
                                    v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octEncode(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    n.xy = (n.z >= 0.0) ? n.xy : octWrap(n.xy);
    return n.xy;
}

// A front-facing surface can have a back-facing shading normal after normal
// mapping, even with RGB8 textures. Move only that normal's view component into
// the visible hemisphere; keep the surface detail perpendicular to the view.
// Apply the same correction in frag.glsl and gbuffer_frag.glsl so direct light,
// cubemap reflection and SSR all use the same normal. True backfaces stay so.
vec3 viewFacingMappedNormal(vec3 N, vec3 geometricN, vec3 V)
{
    if (dot(geometricN, V) <= 0.0) return N;
    // Leave a small margin for the SSR buffer's half-float normal encoding.
    float correction = max(0.001 - dot(N, V), 0.0);
    return normalize(N + V * correction);
}

mat3 computeTBN(vec2 uv, vec3 N)
{
    vec3 dp1 = dFdx(FragPos);
    vec3 dp2 = dFdy(FragPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 tangent = dp1 * duv2.y - dp2 * duv1.y;
    vec3 bitangent = -dp1 * duv2.x + dp2 * duv1.x;
    // Faces with collapsed UVs (zero UV area) or no screen-space change give zero vectors,
    // and normalize(0) is NaN: the normal map then shades them black in speckled blotches
    // that look like z-fighting. Fall back to any frame around N so the map is just flat there.
    if (dot(tangent, tangent) < 1e-20 || dot(bitangent, bitangent) < 1e-20) {
        vec3 axis = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        tangent = cross(axis, N);
        bitangent = cross(N, tangent);
    }
    return mat3(normalize(tangent), normalize(bitangent), N);
}

float sampleSurfaceDepth(vec2 uv)
{
    float h = textureLod(heightMap, uv, 0.0).r;
    if (parallaxInvertHeight) return h;
    return 1.0 - h;
}

// Same march as frag.glsl. Duplicated rather than shared because GLSL 330 has no
// include: if one changes, change both, or reflections slide against the shading
// they are supposed to sit on.
vec2 parallaxOcclusionUv(vec2 uv, vec3 viewTangent)
{
    float maxSteps = float(clamp(parallaxSteps, 4, 256));
    float minSteps = max(4.0, maxSteps * 0.25);
    float numLayers = mix(maxSteps, minSteps, clamp(abs(viewTangent.z), 0.0, 1.0));

    float layerDepth = 1.0 / numLayers;
    vec2 P = (viewTangent.xy / max(abs(viewTangent.z), 0.05)) * heightScale;
    vec2 deltaUv = P / numLayers;

    vec2 currentUv = uv;
    float currentLayerDepth = 0.0;
    float currentDepth = sampleSurfaceDepth(currentUv);

    for (int i = 0; i < 256; ++i) {
        if (float(i) >= numLayers || currentLayerDepth >= currentDepth) break;
        currentUv -= deltaUv;
        currentDepth = sampleSurfaceDepth(currentUv);
        currentLayerDepth += layerDepth;
    }

    vec2 previousUv = currentUv + deltaUv;
    float afterDepth = currentDepth - currentLayerDepth;
    float beforeDepth = sampleSurfaceDepth(previousUv) - currentLayerDepth + layerDepth;
    float denom = afterDepth - beforeDepth;
    float weight = (abs(denom) < 1e-5) ? 0.0 : afterDepth / denom;
    return mix(currentUv, previousUv, clamp(weight, 0.0, 1.0));
}

void main()
{
    vec2 uv = TexCoord * uvTransform.xy + uvTransform.zw;
    vec3 norm = normalize(Normal);
    vec3 viewDir = normalize(viewPos - FragPos);

    // Parallax only runs for PBR materials, matching frag.glsl.
    bool wantsParallax = usePbr && hasHeightMap && heightScale > 0.0;
    bool wantsTangentFrame = hasNormalMap || wantsParallax;
    mat3 TBN = mat3(1.0);
    if (wantsTangentFrame) {
        TBN = computeTBN(uv, norm);
    }
    if (wantsParallax) {
        vec3 viewTangent = normalize(transpose(TBN) * viewDir);
        uv = parallaxOcclusionUv(uv, viewTangent);
    }

    // Normal mapping matters more here than almost anywhere: the reflected ray
    // direction comes straight off this vector, so dropping it would give
    // mirror-flat reflections on every surface that has surface detail.
    if (hasNormalMap) {
        vec3 mapN = texture(normalMap, uv).xyz * 2.0 - 1.0;
        mapN.xy *= max(normalMapIntensity, 0.0);
        mapN = normalize(mapN);
        norm = viewFacingMappedNormal(normalize(TBN * mapN), normalize(Normal), viewDir);
    }

    float roughness;
    float metallic;
    if (usePbr) {
        metallic = clamp(pbrMetallic, 0.0, 1.0);
        roughness = clamp(pbrRoughness, 0.03, 1.0);
        // Same ladder as frag.glsl: a map replaces its channel, and a
        // dedicated single-channel map outranks the packed one. SSR reads
        // roughness from this buffer, so any divergence here shows up as
        // reflections sharper or blurrier than the surface they sit on.
        if (hasMetallicRoughnessMap) {
            vec3 mr = texture(metallicRoughnessMap, uv).rgb;
            roughness = clamp(mr.g, 0.03, 1.0);
            metallic = clamp(mr.b, 0.0, 1.0);
        }
        if (hasMetallicMap) {
            metallic = clamp(texture(metallicMap, uv).r, 0.0, 1.0);
        }
        if (hasRoughnessMap) {
            roughness = clamp(texture(roughnessMap, uv).r, 0.03, 1.0);
        }
    } else {
        // Same reinterpretation frag.glsl uses for legacy materials, so a
        // Blinn-Phong surface reflects with the sharpness it already shades with.
        metallic = clamp(specularStrength, 0.0, 1.0);
        roughness = clamp(1.0 - clamp(shininess / 256.0, 0.0, 1.0), 0.03, 1.0);
    }

    vec3 geomN = normalize(Normal);
    float coatAmount = usePbr ? clamp(clearcoat, 0.0, 1.0) : 0.0;
    float coatRough = clamp(clearcoatRoughness, 0.02, 1.0);
    vec3 coatN = geomN;
    if (surfaceWetness > 0.001 && wetnessReceive) {
        vec3 albedoUnused = vec3(1.0);
        applySurfaceWetness(FragPos, geomN, clamp(surfaceWetness, 0.0, 1.0), rainAmount, wetTime,
                            puddleAmount, metallic, albedoUnused, roughness, norm,
                            coatAmount, coatRough, coatN);
        roughness = clamp(roughness, 0.03, 1.0);
    }
    if (coatAmount > 0.5) {
        // Where the coat dominates it is the sharper, visible reflection, so
        // SSR traces its normal and roughness.
        float k = smoothstep(0.5, 0.9, coatAmount);
        norm = normalize(mix(norm, coatN, k));
        roughness = mix(roughness, min(roughness, coatRough), k);
    }

    FragColor = vec4(octEncode(norm), roughness, metallic);
}
