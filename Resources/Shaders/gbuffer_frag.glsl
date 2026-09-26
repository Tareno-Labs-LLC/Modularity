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

mat3 computeTBN(vec2 uv, vec3 N)
{
    vec3 dp1 = dFdx(FragPos);
    vec3 dp2 = dFdy(FragPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 tangent = normalize(dp1 * duv2.y - dp2 * duv1.y);
    vec3 bitangent = normalize(-dp1 * duv2.x + dp2 * duv1.x);
    return mat3(tangent, bitangent, N);
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
        norm = normalize(TBN * mapN);
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

    FragColor = vec4(octEncode(norm), roughness, metallic);
}
