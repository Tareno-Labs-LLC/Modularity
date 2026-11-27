#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform sampler2D texture1;
uniform sampler2D overlayTex;
uniform sampler2D normalMap;
uniform samplerCube reflectionCube;
uniform float mixAmount = 0.2;
uniform bool hasOverlay = false;
uniform bool hasNormalMap = false;
uniform bool hasReflectionCast = false;
uniform bool unlit = false;
uniform float reflectionIntensity = 0.0;
uniform float reflectionFadeStart = 4.0;
uniform float reflectionFadeEnd = 24.0;
uniform vec4 uvTransform = vec4(1.0, 1.0, 0.0, 0.0);

uniform vec3 viewPos;
uniform vec3 materialColor = vec3(1.0);
uniform float materialAlpha = 1.0;

uniform float ambientStrength = 0.2;
uniform vec3 ambientColor = vec3(1.0);
uniform float specularStrength = 0.5;
uniform bool specularEnabled = true;
uniform float shininess = 32.0;
uniform float normalMapIntensity = 1.0;

// -- Physically based shading --
// Off by default. When off every uniform below is ignored and the Blinn-Phong
// reinterpretation further down runs exactly as it always has, so materials
// authored before this existed are untouched.
uniform bool usePbr = false;
uniform float pbrMetallic = 0.0;
uniform float pbrRoughness = 0.5;
uniform float occlusionStrength = 1.0;
uniform vec3 emissionColor = vec3(1.0);
uniform float emissionStrength = 0.0;
uniform bool hasMetallicRoughnessMap = false;
uniform bool hasMetallicMap = false;
uniform bool hasRoughnessMap = false;
uniform bool hasOpacityMap = false;
uniform bool hasEmissiveMap = false;
uniform bool hasOcclusionMap = false;
// Fragments below this alpha are discarded outright. 0 disables the test, which
// leaves every material authored before this on the ordinary blended path.
uniform float alphaCutoff = 0.0;
// Set for the depth-only draw that gives a texture-transparent surface its solid
// pixels in the depth buffer (colour writes are masked off). Only the alpha test
// above matters there, so the lighting is skipped.
uniform bool depthPrepass = false;
// Units 12-14. Units 0-11 are already spoken for (albedo, overlay, normal, four
// shadow cubes, four directional shadows, reflection cube), which leaves one
// spare against the 16 the GL 3.3 core spec guarantees. Anything added here in
// future needs a plan for that ceiling, not another sampler.
uniform sampler2D metallicRoughnessMap;
uniform sampler2D emissiveMap;
uniform sampler2D occlusionMap;
// Units 17-19. Past the 16 GL 3.3 core guarantees, so the renderer only binds
// these when the driver reports enough fragment texture units and otherwise
// leaves the has* flags above false, falling back to the packed map. Every
// 4.3-class part reports 32.
// GLES 2 parts expose 8 fragment units; albedo, overlay, normal, reflection
// cube, packed metallic-roughness, emissive, occlusion and opacity fill them, so
// the ES 2 profile drops the two single-channel maps (the packed map still works).
#ifndef MODU_ES2
uniform sampler2D metallicMap;
uniform sampler2D roughnessMap;
#endif
uniform sampler2D opacityMap;

// -- Parallax occlusion mapping --
// Unit 15, the last one GL 3.3 core guarantees. Any further map has to share a
// slot or move to an array texture.
#ifndef MODU_ES2
uniform bool hasHeightMap = false;
uniform float heightScale = 0.0;
uniform int parallaxSteps = 32;
uniform bool parallaxInvertHeight = false;
uniform sampler2D heightMap;
#endif

// -- Voxel cone traced global illumination --
// Unit 16, past the 16 that GL 3.3 core guarantees. That is deliberate and it is
// why the renderer refuses to switch GI on unless the driver reports at least 17
// fragment texture units - which every GL 4.3 part does in practice, and 4.3 is
// already the floor for the voxelization pass. Nothing here is sampled while
// giEnabled is false, so a 3.3 context simply never touches it.
#ifndef MODU_ES2
uniform bool giEnabled = false;
uniform sampler3D voxelVolume;
uniform vec3 giVolumeMin = vec3(0.0);
uniform float giVolumeExtent = 1.0;
uniform float giVolumeResolution = 128.0;
uniform float giIntensity = 1.0;
uniform int giConeSteps = 24;
#endif

// -- Image-based lighting --
// Unit 22: the captured sky cube, with a full mip chain. The renderer only sets
// hasEnvironmentCube when the driver has that many units, and the ES 2 profile
// never declares it. Diffuse sky light comes from its smallest levels, specular
// reflections from the level the roughness asks for.
#ifndef MODU_ES2
uniform samplerCube environmentCube;
uniform bool hasEnvironmentCube = false;
uniform bool skyLightingReflections = true;
uniform float environmentIntensity = 1.0;
uniform float environmentMaxLod = 0.0;
// Sunlight reflected off a generic ground (albedo times the main directional
// light's irradiance on a flat surface). The sky cube's lower half is haze, not
// ground, so below the horizon lighting blends to this plus the sky's own share.
uniform vec3 environmentGroundSun = vec3(0.0);
#endif
// True when reflectionCube holds a local ReflectionCast probe rather than the sky.
uniform bool reflectionIsProbe = false;
uniform float reflectionMaxLod = 0.0;

// -- Hemisphere ambient --
// Cheap sky-above / ground-below tint for the flat ambient term, used only when no
// sky cube feeds image-based lighting. 0 (the default) leaves the flat ambient
// exactly as it was; 1 is a full cool-sky / warm-ground gradient.
uniform float ambientHemisphere = 0.0;
vec3 ambientHemisphereTint(vec3 N)
{
    if (ambientHemisphere <= 0.0) return vec3(1.0);
#ifndef MODU_ES2
    if (hasEnvironmentCube && environmentIntensity > 0.0) return vec3(1.0);
#endif
    const vec3 skyTint = vec3(0.80, 0.95, 1.25);
    const vec3 groundTint = vec3(0.55, 0.45, 0.35);
    return mix(vec3(1.0), mix(groundTint, skyTint, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)),
               clamp(ambientHemisphere, 0.0, 1.0));
}

// -- Clearcoat --
// Second specular lobe over the base (car paint lacquer). 0 is off.
uniform float clearcoat = 0.0;
uniform float clearcoatRoughness = 0.04;

// -- Surface wetness --
// surfaceWetness 0 skips the whole wet path. wetnessReceive is per draw (UI and
// sprites stay dry).
uniform float surfaceWetness = 0.0;
uniform float rainAmount = 0.0;
uniform float puddleAmount = 0.5;
uniform float wetTime = 0.0;
uniform bool wetnessReceive = true;

uniform bool fogEnabled = false;
uniform int fogMode = 0;
uniform vec3 fogColor = vec3(0.65, 0.72, 0.78);
uniform float fogStart = 20.0;
uniform float fogEnd = 120.0;
uniform float fogDensity = 0.015;
uniform float fogHeight = 0.0;
uniform float fogHeightFalloff = 0.0;

#ifdef MODU_ES2
// The renderer still uploads every light by name; slots past 4 resolve to
// location -1 and the driver ignores them.
const int MAX_LIGHTS = 4;
#else
const int MAX_LIGHTS = 32;
#endif
const int MAX_SHADOW_MAPS = 4;
uniform int lightCount = 0; // up to MAX_LIGHTS

// type: 0 dir, 1 point, 2 spot, 3 area (rect)
uniform int lightTypeArr[MAX_LIGHTS];
uniform vec3 lightDirArr[MAX_LIGHTS];
uniform vec3 lightPosArr[MAX_LIGHTS];
uniform vec3 lightColorArr[MAX_LIGHTS];
uniform float lightIntensityArr[MAX_LIGHTS];
uniform float lightRangeArr[MAX_LIGHTS];
uniform float lightInnerCosArr[MAX_LIGHTS];
uniform float lightOuterCosArr[MAX_LIGHTS];
uniform vec2 lightAreaSizeArr[MAX_LIGHTS];
uniform float lightAreaFadeArr[MAX_LIGHTS];
#ifndef MODU_ES2
uniform int lightShadowMapArr[MAX_LIGHTS];
uniform int lightShadowKindArr[MAX_LIGHTS]; // 0 off, 1 cube, 2 directional
uniform int lightShadowModeArr[MAX_LIGHTS]; // 0 off, 1 hard, 2 soft
uniform float lightShadowBiasArr[MAX_LIGHTS];
uniform float lightShadowSoftnessArr[MAX_LIGHTS];
uniform float lightShadowFarArr[MAX_LIGHTS];
uniform mat4 lightShadowMatrixArr[MAX_LIGHTS];
// x opacity, y outside opacity, z min distance, w max distance. Static opacity
// is sent as x == y, so the distance terms drop out.
uniform vec4 lightShadowOpacityArr[MAX_LIGHTS];
uniform vec2 lightShadowOpacityFadeArr[MAX_LIGHTS]; // x fade in, y fade out

uniform samplerCube shadowCube0;
uniform samplerCube shadowCube1;
uniform samplerCube shadowCube2;
uniform samplerCube shadowCube3;
// -- Shadow filtering --
//
// 0 hard, 1 low (8 taps), 2 medium (16), 3 high (32, two rings). The old filter
// was a fixed 3x3 whose SPACING was scaled by softness, so a soft shadow was
// nine samples smeared over up to eighty texels - ten possible occlusion values
// spread wide, which reads as concentric bands rather than a penumbra.
uniform int shadowFilterQuality = 2;
// Beyond this distance the tap count halves, and halves again at twice it. A
// penumbra a hundred metres away is a handful of pixels; paying 32 taps for it
// buys nothing.
uniform float shadowLodDistance = 30.0;
// Percentage-closer soft shadows: search for occluders first, size the penumbra
// from how far behind them the receiver sits. This is what makes a shadow tight
// where it touches and wide where it does not, instead of uniformly blurry.
uniform bool shadowContactHardening = true;
uniform sampler2D dirShadow0;
uniform sampler2D dirShadow1;
uniform sampler2D dirShadow2;
uniform sampler2D dirShadow3;
#endif

#ifndef MODU_ES2
float sampleShadowCube(int mapIndex, vec3 sampleDir)
{
    if (mapIndex == 0) return texture(shadowCube0, sampleDir).r;
    if (mapIndex == 1) return texture(shadowCube1, sampleDir).r;
    if (mapIndex == 2) return texture(shadowCube2, sampleDir).r;
    if (mapIndex == 3) return texture(shadowCube3, sampleDir).r;
    return 1.0;
}

float sampleDirectionalShadow(int mapIndex, vec2 uv)
{
    if (mapIndex == 0) return texture(dirShadow0, uv).r;
    if (mapIndex == 1) return texture(dirShadow1, uv).r;
    if (mapIndex == 2) return texture(dirShadow2, uv).r;
    if (mapIndex == 3) return texture(dirShadow3, uv).r;
    return 1.0;
}

vec2 getDirectionalShadowTexelSize(int mapIndex)
{
    if (mapIndex == 0) return 1.0 / vec2(textureSize(dirShadow0, 0));
    if (mapIndex == 1) return 1.0 / vec2(textureSize(dirShadow1, 0));
    if (mapIndex == 2) return 1.0 / vec2(textureSize(dirShadow2, 0));
    if (mapIndex == 3) return 1.0 / vec2(textureSize(dirShadow3, 0));
    return vec2(0.0);
}

// Vogel disk: an even, isotropic point distribution that takes a rotation for
// free. Poisson tables have to be stored; this is generated, so the tap count
// can vary per pixel without carrying several tables.
//
// The point is sqrt((i + 0.5) / N) * (cos(i*g + phi), sin(i*g + phi)). Only the
// phi rotation and the 1/sqrt(N) scale vary, so the unrotated, unscaled points
// sqrt(i + 0.5) * (cos(i*g), sin(i*g)) with g = 2.39996323 are tabulated and
// the per-pixel rotation is applied by angle addition. Same sample set, with
// no sqrt/cos/sin per tap - those dominate the filter on small integrated GPUs.
// 32 entries covers the largest tap count shadowTapCount() can return.
const vec2 kVogelDisk[32] = vec2[32](
    vec2(0.707106781, 0.0),
    vec2(-0.903088678, 0.827303354),
    vec2(0.138231927, -1.57508474),
    vec2(1.13828528, 1.48469075),
    vec2(-2.08889288, -0.369494969),
    vec2(1.978781, -1.25873974),
    vec2(-0.66186239, 2.46210036),
    vec2(-1.26224739, -2.43037683),
    vec2(2.73856936, 1.00011892),
    vec2(-2.8490234, 1.17603812),
    vec2(1.37341538, -2.93491571),
    vec2(1.01492414, 3.23572696),
    vec2(-3.05898547, -1.77274023),
    vec2(3.58853502, -0.788933722),
    vec2(-2.19002373, 3.11509166),
    vec2(-0.505952318, -3.90435811),
    vec2(3.10602264, 2.61775158),
    vec2(-4.17972756, 0.172851209),
    vec2(3.04878572, -3.03395874),
    vec2(-0.203968423, 4.41116729),
    vec2(-2.90094024, -3.47628332),
    vec2(4.59540098, 0.618295934),
    vec2(-3.89366765, 2.70912388),
    vec2(1.06396571, -4.72947957),
    vec2(2.46092898, 4.29462787),
    vec2(-4.81086613, -1.53478568),
    vec2(4.67313587, -2.15912046),
    vec2(-2.02450974, 4.83749525),
    vec2(-1.80685329, -5.02347302),
    vec2(4.80781582, 2.52683737),
    vec2(-5.34026263, 1.40769138),
    vec2(3.03543122, -4.72082168)
);

// rot = (cos(phi), sin(phi)), invSqrtCount = inversesqrt(float(count)).
vec2 vogelDiskSample(int index, vec2 rot, float invSqrtCount)
{
    vec2 p = kVogelDisk[index];
    return vec2(p.x * rot.x - p.y * rot.y, p.x * rot.y + p.y * rot.x) * invSqrtCount;
}

// Per-pixel rotation. Without it every pixel samples the same offsets and the
// kernel's own shape shows up as banding; with it the error becomes noise,
// which reads as a soft edge instead of a ring.
float interleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

int shadowTapCount(float viewDistance)
{
    int taps = 1;
    if (shadowFilterQuality >= 3)      taps = 32;
    else if (shadowFilterQuality == 2) taps = 16;
    else if (shadowFilterQuality == 1) taps = 8;
    else                               return 1;

    // Distance LOD. Kept a power-of-two ladder so the falloff is predictable
    // rather than a smooth ramp that quietly costs more everywhere.
    if (shadowLodDistance > 0.0) {
        if (viewDistance > shadowLodDistance * 2.0)      taps /= 4;
        else if (viewDistance > shadowLodDistance)       taps /= 2;
    }
    return max(taps, 4);
}

// Average depth of whatever is occluding this point, or -1 when nothing is.
float directionalBlockerDepth(int mapIndex, vec2 uv, float refDepth, float bias,
                              vec2 texelSize, float searchRadius, int taps, vec2 rot)
{
    float sum = 0.0;
    float count = 0.0;
    float invSqrtTaps = inversesqrt(float(taps));
    vec2 offsetScale = texelSize * searchRadius;
    for (int i = 0; i < taps; ++i) {
        vec2 offset = vogelDiskSample(i, rot, invSqrtTaps) * offsetScale;
        float d = sampleDirectionalShadow(mapIndex, uv + offset);
        if (refDepth - bias > d) {
            sum += d;
            count += 1.0;
        }
    }
    return (count > 0.0) ? (sum / count) : -1.0;
}

float computeShadowOcclusion(int lightIndex, vec3 lightToFrag, float nl, vec3 worldPos)
{
    int mode = lightShadowModeArr[lightIndex];
    int mapIndex = lightShadowMapArr[lightIndex];
    int shadowKind = lightShadowKindArr[lightIndex];
    if (mode <= 0 || mapIndex < 0 || mapIndex >= MAX_SHADOW_MAPS || shadowKind <= 0) return 0.0;

    float baseBias = max(lightShadowBiasArr[lightIndex], 0.0001);
    float slopeBias = baseBias * (1.0 - clamp(nl, 0.0, 1.0));
    float bias = max(baseBias * 0.25, slopeBias);

    if (shadowKind == 2) {
        vec4 lightSpace = lightShadowMatrixArr[lightIndex] * vec4(worldPos, 1.0);
        vec3 projCoords = lightSpace.xyz / max(lightSpace.w, 0.0001);
        projCoords = projCoords * 0.5 + 0.5;
        if (projCoords.z <= 0.0 || projCoords.z >= 1.0) return 0.0;
        if (projCoords.x <= 0.0 || projCoords.x >= 1.0 || projCoords.y <= 0.0 || projCoords.y >= 1.0) return 0.0;

        float softness = max(lightShadowSoftnessArr[lightIndex], 0.0);
        vec2 texelSize = getDirectionalShadowTexelSize(mapIndex);
        if (mode == 1 || softness <= 0.0001 || texelSize.x <= 0.0 || texelSize.y <= 0.0 ||
            shadowFilterQuality <= 0) {
            float closestDepth = sampleDirectionalShadow(mapIndex, projCoords.xy);
            return (projCoords.z - bias > closestDepth) ? 1.0 : 0.0;
        }

        float viewDistance = length(worldPos - viewPos);
        int taps = shadowTapCount(viewDistance);
        float phi = interleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
        vec2 rot = vec2(cos(phi), sin(phi));
        float maxRadius = max(1.0, softness * 80.0);

        // Contact hardening: with no occluder found the point is lit outright,
        // which also skips the filter entirely for the fully-lit majority of the
        // screen. Where there is one, the penumbra grows with the gap between
        // blocker and receiver - the ratio is the similar-triangles term from
        // the PCSS paper.
        float radius = maxRadius;
        if (shadowContactHardening) {
            float blocker = directionalBlockerDepth(mapIndex, projCoords.xy, projCoords.z,
                                                    bias, texelSize, maxRadius,
                                                    max(taps / 2, 4), rot);
            if (blocker < 0.0) return 0.0;
            float gap = max(projCoords.z - blocker, 0.0);
            radius = clamp(gap / max(blocker, 0.0001) * maxRadius, 1.0, maxRadius);
        }

        // Gaussian-weighted accumulation. Weighting by distance from the centre
        // is what turns a disc of binary tests into a smooth falloff; a flat
        // average of the same taps still steps.
        // dot(disk, disk) is (i + 0.5) / taps, so exp(-2 * dot) is the geometric
        // series exp(-1/taps) * exp(-2/taps)^i: two exp per pixel, not one per tap.
        float shadow = 0.0;
        float weightSum = 0.0;
        float invTaps = 1.0 / float(taps);
        float invSqrtTaps = inversesqrt(float(taps));
        float w = exp(-invTaps);
        float wStep = exp(-2.0 * invTaps);
        vec2 offsetScale = texelSize * radius;
        float refDepth = projCoords.z - bias;
        for (int i = 0; i < taps; ++i) {
            vec2 offset = vogelDiskSample(i, rot, invSqrtTaps) * offsetScale;
            float sampleDepth = sampleDirectionalShadow(mapIndex, projCoords.xy + offset);
            shadow += w * ((refDepth > sampleDepth) ? 1.0 : 0.0);
            weightSum += w;
            w *= wStep;
        }
        return shadow / max(weightSum, 0.0001);
    }

    float farPlane = max(lightShadowFarArr[lightIndex], 0.001);
    float currentDepth = length(lightToFrag);
    if (currentDepth <= 0.0001) return 0.0;
    float softness = max(lightShadowSoftnessArr[lightIndex], 0.0);
    if (mode == 1 || softness <= 0.0001 || shadowFilterQuality <= 0) {
        float hardDepth = sampleShadowCube(mapIndex, lightToFrag) * farPlane;
        return (currentDepth - bias > hardDepth) ? 1.0 : 0.0;
    }

    // Sample across the plane facing the light rather than along twenty fixed
    // cube-corner directions. Those twenty were unevenly spaced and identical
    // for every pixel, so the kernel's own shape showed through as banding on a
    // wide penumbra - the same failure as the directional path, just less
    // obvious because there were twenty taps instead of nine.
    vec3 lightDirN = lightToFrag / currentDepth;
    vec3 up = abs(lightDirN.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 tangent = normalize(cross(up, lightDirN));
    vec3 bitangent = cross(lightDirN, tangent);

    float viewDistance = length(worldPos - viewPos);
    int taps = shadowTapCount(viewDistance);
    float phi = interleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
    vec2 rot = vec2(cos(phi), sin(phi));
    float refDepth = currentDepth - bias;
    float maxRadius = softness * (1.0 + currentDepth / farPlane);

    // Same contact-hardening idea as the directional path: size the penumbra by
    // how far the receiver sits behind whatever is blocking it.
    float radius = maxRadius;
    if (shadowContactHardening) {
        float blockerSum = 0.0;
        float blockerCount = 0.0;
        int searchTaps = max(taps / 2, 4);
        float invSqrtSearchTaps = inversesqrt(float(searchTaps));
        for (int i = 0; i < searchTaps; ++i) {
            vec2 disk = vogelDiskSample(i, rot, invSqrtSearchTaps);
            vec3 offset = (tangent * disk.x + bitangent * disk.y) * maxRadius;
            float d = sampleShadowCube(mapIndex, lightToFrag + offset) * farPlane;
            if (refDepth > d) {
                blockerSum += d;
                blockerCount += 1.0;
            }
        }
        if (blockerCount <= 0.0) return 0.0;
        float blocker = blockerSum / blockerCount;
        float gap = max(currentDepth - blocker, 0.0);
        radius = clamp(gap / max(blocker, 0.0001) * maxRadius, maxRadius * 0.05, maxRadius);
    }

    // Same geometric-series Gaussian weights as the directional filter.
    float shadow = 0.0;
    float weightSum = 0.0;
    float invTaps = 1.0 / float(taps);
    float invSqrtTaps = inversesqrt(float(taps));
    float w = exp(-invTaps);
    float wStep = exp(-2.0 * invTaps);
    for (int i = 0; i < taps; ++i) {
        vec2 disk = vogelDiskSample(i, rot, invSqrtTaps);
        vec3 offset = (tangent * disk.x + bitangent * disk.y) * radius;
        float closestDepth = sampleShadowCube(mapIndex, lightToFrag + offset) * farPlane;
        shadow += w * ((refDepth > closestDepth) ? 1.0 : 0.0);
        weightSum += w;
        w *= wStep;
    }
    return shadow / max(weightSum, 0.0001);
}

// How strongly this light's shadow darkens a receiver at lightDistance. The
// light's opacity holds from min to max distance and blends to the outside
// opacity across fade in (inward of min) and fade out (outward of max). A
// zero-width fade is a hard edge; smoothstep is undefined there, so it's
// branched rather than passed equal edges.
float shadowOpacityAt(int lightIndex, float lightDistance)
{
    vec4 o = lightShadowOpacityArr[lightIndex];
    if (o.x == o.y) return o.x;
    vec2 fade = lightShadowOpacityFadeArr[lightIndex];
    float inner = (fade.x > 0.0) ? smoothstep(o.z - fade.x, o.z, lightDistance)
                                 : step(o.z, lightDistance);
    float outer = (fade.y > 0.0) ? 1.0 - smoothstep(o.w, o.w + fade.y, lightDistance)
                                 : step(lightDistance, o.w);
    return mix(o.y, o.x, inner * outer);
}
#endif // !MODU_ES2 (shadows)

const float PI = 3.14159265359;

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    // x^5 as three multiplies. pow() goes through the GPU's transcendental
    // unit, which runs at a fraction of ALU rate on low-end parts, and this is
    // evaluated per light per fragment.
    float x = 1.0 - clamp(cosTheta, 0.0, 1.0);
    float x2 = x * x;
    return F0 + (1.0 - F0) * (x2 * x2 * x);
}

float distributionGGX(float NdotH, float roughness)
{
    float a = max(roughness * roughness, 0.02);
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 0.0001);
}

// Smith-Schlick visibility G / (4 * NdotV * NdotL), with the cosine
// factors cancelled analytically. Forming G first and clamping that division
// to 0.0001 incorrectly forces the highlight to zero at grazing angles.
float visibilitySmithSchlick(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float v = NdotV * (1.0 - k) + k;
    float l = NdotL * (1.0 - k) + k;
    return 0.25 / (v * l);
}

vec3 evaluateDirectSpecular(
    vec3 N, vec3 V, vec3 L, vec3 F0,
    float roughness, float smoothness,
    float specPower, float specEnergy,
    vec3 lightColor, float intensity,
    bool energyConserving)
{
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    if (NdotL <= 0.0 || NdotV <= 0.0) {
        return vec3(0.0);
    }

    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = distributionGGX(NdotH, roughness);
    float visibility = visibilitySmithSchlick(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlick(VdotH, F0);
    vec3 microfacet = D * visibility * F;

    if (energyConserving) {
        // Cook-Torrance alone. The extra lobe and the visibility scale below are
        // there to make the Blinn-Phong knobs feel responsive across their range;
        // once real roughness drives D and G they only add light the surface
        // never received, which reads as a plastic sheen on rough materials.
        return microfacet * lightColor * intensity * NdotL;
    }

    // Extra direct reflection lobe so high smoothness pops clearly under direct lights.
    float refl = pow(max(dot(reflect(-L, N), V), 0.0), specPower * 1.25);
    float blinn = pow(max(NdotH, 0.0), specPower);
    vec3 enhanced = F * (0.8 * refl + 0.25 * blinn) * specEnergy;

    // Keep very rough surfaces close to matte, while preserving high-smoothness highlights.
    float smoothVisibility = mix(0.03, 1.0, smoothness * smoothness);

    return (microfacet * smoothVisibility + enhanced) * lightColor * intensity * NdotL;
}

// Clearcoat lobe: GGX with Kelemen's visibility and a fixed F0 of 0.04 (a clear
// lacquer or a water film). Returns the lobe's reflectance times NdotL, and the
// Fresnel term that the layer below loses to it.
float clearcoatLobe(vec3 Nc, vec3 V, vec3 L, float roughness, float amount, out float Fc)
{
    vec3 H = normalize(V + L);
    float NoH = max(dot(Nc, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    float NoL = max(dot(Nc, L), 0.0);
    float x = 1.0 - VoH;
    float x2 = x * x;
    Fc = (0.04 + 0.96 * x2 * x2 * x) * amount;
    if (NoL <= 0.0) return 0.0;
    float D = distributionGGX(NoH, roughness);
    float visibility = 0.25 / max(VoH * VoH, 1e-4);
    return D * visibility * Fc * NoL;
}

// Split-sum environment BRDF without a lookup texture: Karis' analytic fit
// ("Physically Based Shading on Mobile"). Scale and bias for F0 integrated over
// the lobe, so IBL specular stays energy-plausible across roughness.
vec3 envBRDFApprox(vec3 F0, float roughness, float NoV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

// The sky and probe cubes hold display-encoded colour (they are rendered by the
// same shaders that write the screen), so lighting decodes them first.
vec3 decodeEnvironment(vec3 c)
{
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

// Roughness to the fraction of the mip chain to read. The chain is a plain box
// filter rather than a GGX prefilter, so this curve leans a little sharp.
float environmentMipFraction(float roughness)
{
    return roughness * (1.7 - 0.7 * roughness);
}

#ifndef MODU_ES2
const float kGroundAlbedo = 0.12;

// What a generic lit ground looks like: the sun term from the renderer plus
// the ground's share of sky light (the cube's 1x1 level is its average).
vec3 environmentGround()
{
    vec3 skyAverage = decodeEnvironment(textureLod(environmentCube, vec3(0.0, 1.0, 0.0), environmentMaxLod).rgb);
    return environmentGroundSun + skyAverage * kGroundAlbedo;
}

// The sky cube with the ground substituted below the horizon. The blend
// widens with the mip so rough reflections get a soft horizon.
vec3 sampleSkyCube(vec3 dir, float lod, vec3 ground)
{
    vec3 sky = decodeEnvironment(textureLod(environmentCube, dir, lod).rgb);
    float width = 0.04 + 0.5 * lod / max(environmentMaxLod, 1.0);
    return mix(sky, ground, smoothstep(0.02, -width, dir.y));
}

// Hemisphere irradiance: the sky from above, the ground from below. The sky
// read is tilted upward so its blurred level does not pull in the haze band.
vec3 environmentIrradiance(vec3 N)
{
    vec3 ground = environmentGround();
    vec3 skyDir = normalize(vec3(N.x, max(N.y, 0.0) + 0.35, N.z));
    vec3 sky = decodeEnvironment(textureLod(environmentCube, skyDir, max(environmentMaxLod - 1.0, 0.0)).rgb);
    return mix(sky, ground, 0.5 - 0.5 * N.y);
}

vec3 sampleEnvironmentSpecular(vec3 R, float roughness)
{
    float mip = environmentMipFraction(roughness);
    vec3 sky = vec3(0.0);
    if (hasEnvironmentCube && skyLightingReflections) {
        sky = sampleSkyCube(R, mip * environmentMaxLod, environmentGround()) * environmentIntensity;
    }
    if (hasReflectionCast && reflectionIsProbe) {
        // The probe weight fades toward its blend distance; blend toward the sky
        // there instead of toward black.
        vec3 probe = decodeEnvironment(textureLod(reflectionCube, R, mip * reflectionMaxLod).rgb);
        float w = max(reflectionIntensity, 0.0);
        return probe * w + sky * (1.0 - min(w, 1.0));
    }
    return sky;
}

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
#endif // !MODU_ES2 (image-based lighting, wetness)

float computeFogFactor()
{
    if (!fogEnabled) {
        return 0.0;
    }

    float dist = length(viewPos - FragPos);
    float factor = 0.0;
    if (fogMode == 1) {
        factor = 1.0 - exp(-max(fogDensity, 0.0) * dist);
    } else if (fogMode == 2) {
        float densityDistance = max(fogDensity, 0.0) * dist;
        factor = 1.0 - exp(-(densityDistance * densityDistance));
    } else {
        factor = smoothstep(fogStart, max(fogStart + 0.01, fogEnd), dist);
    }

    if (fogHeightFalloff > 0.0001) {
        float heightWeight = 1.0 - exp(-abs(FragPos.y - fogHeight) * fogHeightFalloff);
        factor *= clamp(heightWeight, 0.0, 1.0);
    }

    return clamp(factor, 0.0, 1.0);
}

vec3 applyFog(vec3 color)
{
    return mix(color, fogColor, computeFogFactor());
}

// Marches one cone through the mip chain of the radiance volume, widening as it
// goes: a cone is approximated by sampling progressively coarser mips at
// progressively larger radii, which is the whole trick that makes voxel GI
// affordable compared with tracing many rays.
//
// Front-to-back accumulation with an opacity term, so the first surface a cone
// meets occludes what is behind it instead of every voxel along the line summing
// together.
#ifndef MODU_ES2
vec4 traceGiCone(vec3 origin, vec3 dir, float aperture)
{
    float voxelSize = giVolumeExtent / max(giVolumeResolution, 1.0);
    float dist = voxelSize;
    vec4 accum = vec4(0.0);

    for (int i = 0; i < 64; ++i) {
        if (i >= giConeSteps || accum.a >= 0.99) break;

        float diameter = max(voxelSize, 2.0 * aperture * dist);
        float lod = log2(max(diameter / voxelSize, 1.0));

        vec3 pos = origin + dir * dist;
        vec3 uvw = (pos - giVolumeMin) / max(giVolumeExtent, 1e-6);
        // Outside the volume there is nothing recorded. Stopping rather than
        // clamping matters: clamping would smear the volume's border voxels
        // across everything beyond it.
        if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) break;

        vec4 samp = textureLod(voxelVolume, uvw, lod);
        // Premultiplied front-to-back: rgb and alpha advance together, with no
        // extra samp.a on the colour.
        //
        // The volume is already coverage-premultiplied and it is the mip chain
        // that makes it so. Empty voxels are (0,0,0,0), so a coarse level's rgb
        // is the average of "radiance where there is surface, zero where there
        // is not" - which is radiance times coverage. Weighting by samp.a again
        // would attenuate by coverage squared, and since a widening cone spends
        // nearly all of its length in coarse mips, that is the difference
        // between GI that lights a room and GI you cannot see.
        accum += (1.0 - accum.a) * samp;

        dist += diameter * 0.5;
    }
    return accum;
}

// Six cones over the hemisphere: one up the normal and five at 60 degrees to it,
// spaced evenly around. Enough to read as bounced light without the banding a
// sparser set gives on flat walls.
vec3 gatherGiDiffuse(vec3 P, vec3 N)
{
    vec3 up = (abs(N.y) < 0.95) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    float voxelSize = giVolumeExtent / max(giVolumeResolution, 1.0);
    // Start clear of the surface's own voxel, or every fragment gathers the
    // light it just emitted and the whole scene self-illuminates.
    vec3 origin = P + N * voxelSize * 1.5;

    const float aperture = 0.577;   // tan(30 degrees): a 60 degree cone
    vec3 total = traceGiCone(origin, N, aperture).rgb * 0.25;

    for (int i = 0; i < 5; ++i) {
        float angle = 1.2566370614 * float(i);   // 72 degrees apart
        vec3 dir = normalize(0.5 * N + 0.866 * (cos(angle) * T + sin(angle) * B));
        total += traceGiCone(origin, dir, aperture).rgb * 0.15;
    }
    return total;
}
#endif // !MODU_ES2 (voxel GI)

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

// Tangent frame from screen-space derivatives rather than a vertex attribute.
// The mesh vertex format is pos/normal/uv with no tangent, and adding one would
// change a stride baked into the mesh cache, the model loader, the static merge
// and the lightmapper - so the frame is reconstructed here instead. Must be fed
// the UNDISPLACED uv: the derivatives have to describe the surface's own
// parameterisation, not the one parallax has already walked along.
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

#ifndef MODU_ES2
float sampleSurfaceDepth(vec2 uv)
{
    // Stored as height (white high); the march works in depth (white deep).
    float h = textureLod(heightMap, uv, 0.0).r;
    if (parallaxInvertHeight) return h;
    return 1.0 - h;
}

// Steep parallax march plus the occlusion refinement: walk the view ray through
// depth layers until it passes under the surface, then interpolate between the
// last two samples so the hit is not quantised to a layer boundary.
vec2 parallaxOcclusionUv(vec2 uv, vec3 viewTangent)
{
    // Grazing rays cross many texels and need the steps; head-on rays barely
    // move and would only be paying for them.
    float maxSteps = clamp(float(parallaxSteps), 4.0, 256.0);
    float minSteps = max(4.0, maxSteps * 0.25);
    float numLayers = mix(maxSteps, minSteps, clamp(abs(viewTangent.z), 0.0, 1.0));

    float layerDepth = 1.0 / numLayers;
    // Dividing by z is what makes the offset grow at grazing angles; clamped
    // away from zero so a ray parallel to the surface does not blow up into a
    // smear across the whole texture.
    vec2 P = (viewTangent.xy / max(abs(viewTangent.z), 0.05)) * heightScale;
    vec2 deltaUv = P / numLayers;

    vec2 currentUv = uv;
    float currentLayerDepth = 0.0;
    float currentDepth = sampleSurfaceDepth(currentUv);

    // Bounded loop: a while() on a texture-dependent condition is legal in GLSL
    // 3.30 but not something every driver unrolls happily.
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
#endif // !MODU_ES2 (parallax)


#ifdef MODU_TERRAIN
// ---- Terrain surface (compiled with the MODU_TERRAIN define) -------------------------------------
// The terrain mesh uses the standard vertex stage: TexCoord is the position across the terrain
// (0..1 on both axes), FragPos/Normal are world space. Everything below only replaces where the
// base colour, normal, roughness and occlusion come from; lighting, shadows, IBL and fog below
// are the unchanged material path.
// Unit 0 (texture1): RGBA8 splat, R grass, G forest floor, B path, A rock.
// terrainAlbedo: Texture2DArray, rgb albedo (gamma), a height proxy used for height blending.
// terrainNormal: Texture2DArray, rg tangent normal xy (0..1), b roughness, a ambient occlusion.
uniform sampler2DArray terrainAlbedo;
uniform sampler2DArray terrainNormal;
uniform vec4 terrainTiling = vec4(4.0);          // metres per repeat, layers 0..3
uniform vec4 terrainNormalStrength = vec4(1.0);
uniform vec3 terrainTint0 = vec3(1.0);
uniform vec3 terrainTint1 = vec3(1.0);
uniform vec3 terrainTint2 = vec3(1.0);
uniform vec3 terrainTint3 = vec3(1.0);
uniform float terrainSize = 256.0;               // local metres across the terrain
uniform vec2 terrainGrassShade = vec2(0.0);         // x grass distance (0 = no grass), y how much the ground darkens under it
uniform float terrainRockSlope = 0.8;            // normal.y below which slopes turn to rock

float terrainHash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float terrainNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(terrainHash(i), terrainHash(i + vec2(1.0, 0.0)), u.x),
               mix(terrainHash(i + vec2(0.0, 1.0)), terrainHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

struct TerrainSurface {
    vec3 albedo;      // gamma space, like a sampled albedo texture
    vec3 normal;      // world space
    float roughness;
    float ao;
};

// One planar layer: two samples at different scale/rotation blended by a slow noise to hide repetition.
void terrainSamplePlanar(int layer, vec2 p, vec2 dpdx, vec2 dpdy, float tiling, float normalStrength,
                         out vec4 albedo, out vec4 nrm)
{
    vec2 uv1 = p / tiling;
    vec2 d1x = dpdx / tiling;
    vec2 d1y = dpdy / tiling;
    const mat2 M = mat2(0.62, -0.78, 0.78, 0.62) * 0.57;
    vec2 uv2 = M * uv1 + vec2(0.31, 0.77);
    vec2 d2x = M * d1x;
    vec2 d2y = M * d1y;
    float k = smoothstep(0.30, 0.70, terrainNoise(p * 0.045 + float(layer) * 17.3));
    float l = float(layer);
    vec4 a = mix(textureGrad(terrainAlbedo, vec3(uv1, l), d1x, d1y),
                 textureGrad(terrainAlbedo, vec3(uv2, l), d2x, d2y), k);
    vec4 n = mix(textureGrad(terrainNormal, vec3(uv1, l), d1x, d1y),
                 textureGrad(terrainNormal, vec3(uv2, l), d2x, d2y), k);
    n.rg = (n.rg - 0.5) * normalStrength + 0.5;
    albedo = a;
    nrm = n;
}

vec3 terrainUnpackNormal(vec2 rg)
{
    vec2 xy = rg * 2.0 - 1.0;
    return vec3(xy, sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

TerrainSurface terrainEvaluate(vec3 worldPos, vec3 geomN, vec2 terrainUv)
{
    TerrainSurface ts;
    vec4 splat = texture(texture1, terrainUv);
    vec2 p = (terrainUv - 0.5) * terrainSize;        // terrain-local XZ metres
    vec2 dpdx = dFdx(p);
    vec2 dpdy = dFdy(p);
    vec3 dwx = dFdx(worldPos);
    vec3 dwy = dFdy(worldPos);

    // Slopes become rock regardless of the paint.
    float slopeRock = smoothstep(terrainRockSlope, terrainRockSlope - 0.16, geomN.y);
    vec4 w = splat;
    w.a = max(w.a, slopeRock);
    float rgbSum = w.r + w.g + w.b;
    if (rgbSum > 1e-4) w.rgb = w.rgb / rgbSum * (1.0 - w.a);
    else w.r = 1.0 - w.a;

    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - geomN * geomN.x);
    vec3 B = cross(geomN, T);

    vec4 albedoS[4];
    vec4 nrmS[4];
    albedoS[0] = albedoS[1] = albedoS[2] = albedoS[3] = vec4(0.0);
    nrmS[0] = nrmS[1] = nrmS[2] = nrmS[3] = vec4(0.5, 0.5, 0.85, 1.0);
    float wv[4] = float[](w.r, w.g, w.b, w.a);
    float tiles[4] = float[](terrainTiling.x, terrainTiling.y, terrainTiling.z, terrainTiling.w);
    float strengths[4] = float[](terrainNormalStrength.x, terrainNormalStrength.y, terrainNormalStrength.z, terrainNormalStrength.w);
    for (int i = 0; i < 3; ++i) {
        if (wv[i] > 0.004) terrainSamplePlanar(i, p, dpdx, dpdy, tiles[i], strengths[i], albedoS[i], nrmS[i]);
    }
    // Rock is triplanar so cliffs do not stretch.
    vec3 rockWorldNormal = geomN;
    if (wv[3] > 0.004) {
        vec3 bw = pow(abs(geomN), vec3(4.0));
        bw /= max(bw.x + bw.y + bw.z, 1e-4);
        float t = tiles[3];
        vec4 aX = vec4(0.0), aY = vec4(0.0), aZ = vec4(0.0);
        vec4 nX = vec4(0.5, 0.5, 1.0, 1.0), nY = nX, nZ = nX;
        vec4 tmpA, tmpN;
        if (bw.x > 0.02) { terrainSamplePlanar(3, worldPos.zy, dwx.zy, dwy.zy, t, strengths[3], aX, nX); }
        if (bw.y > 0.02) { terrainSamplePlanar(3, worldPos.xz, dwx.xz, dwy.xz, t, strengths[3], aY, nY); }
        if (bw.z > 0.02) { terrainSamplePlanar(3, worldPos.xy, dwx.xy, dwy.xy, t, strengths[3], aZ, nZ); }
        albedoS[3] = aX * bw.x + aY * bw.y + aZ * bw.z;
        nrmS[3] = nX * bw.x + nY * bw.y + nZ * bw.z;
        // Whiteout blend of the three projected tangent normals.
        vec3 tx = terrainUnpackNormal(nX.rg);
        vec3 ty = terrainUnpackNormal(nY.rg);
        vec3 tz = terrainUnpackNormal(nZ.rg);
        tx = vec3(tx.xy + geomN.zy, abs(tx.z) * geomN.x);
        ty = vec3(ty.xy + geomN.xz, abs(ty.z) * geomN.y);
        tz = vec3(tz.xy + geomN.xy, abs(tz.z) * geomN.z);
        rockWorldNormal = normalize(tx.zyx * bw.x + ty.xzy * bw.y + tz.xyz * bw.z);
    }

    // Height blending: the layer whose texture is "taller" at this pixel wins the transition.
    float hv[4];
    float top = 0.0;
    for (int i = 0; i < 4; ++i) {
        hv[i] = wv[i] > 0.004 ? wv[i] + albedoS[i].a * 0.55 : 0.0;
        top = max(top, hv[i]);
    }
    float bwv[4];
    float bsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        bwv[i] = max(hv[i] - (top - 0.30), 0.0) * step(0.004, wv[i]);
        bsum += bwv[i];
    }
    bsum = max(bsum, 1e-4);

    vec3 tints[4] = vec3[](terrainTint0, terrainTint1, terrainTint2, terrainTint3);
    vec3 col = vec3(0.0);
    vec3 nTangent = vec3(0.0);
    float rough = 0.0;
    float ao = 0.0;
    vec3 rockN = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float b = bwv[i] / bsum;
        if (b <= 0.0) continue;
        col += albedoS[i].rgb * tints[i] * b;
        rough += nrmS[i].b * b;
        ao += nrmS[i].a * b;
        if (i < 3) nTangent += (T * (terrainUnpackNormal(nrmS[i].rg).x) + B * (terrainUnpackNormal(nrmS[i].rg).y) +
                                geomN * (terrainUnpackNormal(nrmS[i].rg).z)) * b;
        else rockN = rockWorldNormal * b;
    }
    vec3 nWorld = normalize(nTangent + rockN);
    // Large-scale tint drift breaks up the flat look of a tiled ground, strongest on the grass layer.
    float macro = terrainNoise(p * 0.018) * 0.6 + terrainNoise(p * 0.071 + 5.7) * 0.4;
    vec3 drift = mix(vec3(0.90, 0.97, 0.86), vec3(1.10, 1.04, 0.92), macro);
    col *= mix(vec3(1.0), drift, 0.35 + 0.65 * w.r);
    if (terrainGrassShade.x > 0.0) {
        // Under 3D grass the ground is the dark, shaded gap between the blades; it fades back to the
        // plain tile where the grass stops.
        float under = (1.0 - smoothstep(0.3 * terrainGrassShade.x, 1.05 * terrainGrassShade.x, length(viewPos - worldPos))) * w.r;
        col *= mix(vec3(1.0), vec3(0.50, 0.55, 0.38), under * terrainGrassShade.y);
    }
    ts.albedo = clamp(col, 0.0, 1.0);
    ts.normal = nWorld;
    ts.roughness = clamp(rough, 0.05, 1.0);
    ts.ao = clamp(ao, 0.0, 1.0);
    return ts;
}
#endif // MODU_TERRAIN

#if (defined(MODU_GRASS) || defined(MODU_WATER)) && !defined(MODU_ES2)
// ---- Vegetation and water surfaces (compiled with MODU_GRASS / MODU_WATER) ---------------------------
// Both skip the material path entirely: one lean lighting function each (sun + shadow map, nearby point
// and spot lights, sky image based light, fog), written for surfaces that cover a lot of the screen.

// Directional shadow for thin, unlit-by-self geometry: four rotated taps, with the receiver nudged
// toward the light so a root sunk into the terrain does not shadow itself.
float vegetationShadow(int i, vec3 worldPos, vec3 toLight)
{
    if (lightShadowKindArr[i] != 2 || lightShadowModeArr[i] <= 0) return 0.0;
    int mapIndex = lightShadowMapArr[i];
    if (mapIndex < 0 || mapIndex >= MAX_SHADOW_MAPS) return 0.0;
    vec4 ls = lightShadowMatrixArr[i] * vec4(worldPos + toLight * 0.10, 1.0);
    vec3 pc = ls.xyz / max(ls.w, 0.0001) * 0.5 + 0.5;
    if (pc.z <= 0.0 || pc.z >= 1.0 || pc.x <= 0.0 || pc.x >= 1.0 || pc.y <= 0.0 || pc.y >= 1.0) return 0.0;
    vec2 texel = getDirectionalShadowTexelSize(mapIndex);
    float bias = max(lightShadowBiasArr[i], 0.0001) * 1.5;
    float phi = interleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
    vec2 rot = vec2(cos(phi), sin(phi));
    float occluded = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec2 o = vogelDiskSample(k, rot, 0.5) * texel * 2.0;
        occluded += (pc.z - bias > sampleDirectionalShadow(mapIndex, pc.xy + o)) ? 1.0 : 0.0;
    }
    return occluded * 0.25;
}

vec3 vegetationAmbientScale()
{
    float ambientScale = (ambientStrength <= 0.2) ? ambientStrength * 5.0 : 1.0 + (ambientStrength - 0.2) * 1.25;
    return vec3(ambientScale);
}
#endif

#if defined(MODU_GRASS) && !defined(MODU_ES2)
in vec4 vGrassColor;       // rgb display colour, a ambient occlusion
in vec3 vGrassGround;      // terrain normal (world)

void grassMain()
{
    vec3 V = normalize(viewPos - FragPos);
    vec3 N = normalize(Normal);
    if (dot(N, V) < 0.0) N = -N;
    float t = TexCoord.y;
    // Lit with a bent normal pulled toward the terrain's: a field reads as one soft surface, not as
    // thousands of individually shaded ribbons.
    vec3 Nl = normalize(mix(N, normalize(vGrassGround), 0.62));
    vec3 albedo = pow(max(vGrassColor.rgb, vec3(0.0)), vec3(2.2));
    float ao = vGrassColor.a;

    vec3 lighting = vec3(0.0);
    float ambientScale = vegetationAmbientScale().x;
    if (hasEnvironmentCube && environmentIntensity > 0.0) {
        vec3 irradiance = environmentIrradiance(Nl);
        // Sky light also reaches the blade from behind (grass is thin).
        vec3 through = environmentIrradiance(-Nl);
        lighting += albedo * (irradiance + through * 0.35) * (ao * ambientScale * environmentIntensity);
    } else {
        lighting += albedo * ambientStrength * ambientColor * ambientHemisphereTint(Nl) * ao;
    }

    float thickness = mix(0.45, 1.0, t);   // tips are thin: more light gets through
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= lightCount) break;
        float intensity = lightIntensityArr[i];
        if (intensity <= 0.0) continue;
        int ltype = lightTypeArr[i];
        vec3 L;
        float attenuation = 1.0;
        if (ltype == 0) {
            L = -normalize(lightDirArr[i]);
        } else if (ltype == 1 || ltype == 2) {
            vec3 d = lightPosArr[i] - FragPos;
            float dist = length(d);
            L = d / max(dist, 1e-4);
            float range = lightRangeArr[i];
            if (range > 0.0) {
                if (dist > range) continue;
                float falloff = clamp(1.0 - dist / range, 0.0, 1.0);
                attenuation = falloff * falloff;
            }
            if (ltype == 2) {
                float cosTheta = dot(-L, normalize(lightDirArr[i]));
                attenuation *= smoothstep(lightOuterCosArr[i], lightInnerCosArr[i], cosTheta);
            }
        } else {
            continue;
        }
        if (attenuation <= 0.0) continue;

        float nl = dot(Nl, L);
        // Wrapped diffuse: thin blades scatter, so the terminator is soft.
        float diffuse = clamp(nl * 0.75 + 0.25, 0.0, 1.0);
        // Light shining through the blade toward the eye (looking at the sun through a field), and light
        // striking the back of the blade.
        float toward = pow(clamp(dot(-L, V), 0.0, 1.0), 3.0);
        float transmission = (toward * 0.85 + clamp(-nl, 0.0, 1.0) * 0.30) * thickness;
        vec3 Hv = normalize(L + V);
        float sheen = pow(max(dot(N, Hv), 0.0), 48.0) * 0.06 * t;
        float shadow = ltype == 0 ? vegetationShadow(i, FragPos, L) : 0.0;
        vec3 lightColor = lightColorArr[i] * (intensity * attenuation);
        lighting += (1.0 - shadow) * lightColor *
                    (albedo * (diffuse * mix(0.55, 1.0, ao) + transmission * vec3(1.05, 1.15, 0.55)) + vec3(sheen));
    }

    vec3 finalColor = pow(max(lighting, vec3(0.0)), vec3(1.0 / 2.2));
    FragColor = vec4(applyFog(finalColor), 1.0);
}
#endif // MODU_GRASS

#if defined(MODU_WATER) && !defined(MODU_ES2)
uniform sampler2D grassHeightTex;   // terrain heights (R32F), shared with the grass pass
uniform vec4 grassTerrain;          // x size, y resolution, z height offset, w height scale (terrain-local metres)
uniform mat4 waterInverse;          // world -> terrain-local
uniform vec3 waterShallow;          // display colours
uniform vec3 waterDeep;
uniform vec4 waterParams;           // x clarity (m), y wave scale, z wave strength, w sky reflection
uniform vec4 waterParams2;          // x foam, y time (s)

float waterBedHeight(vec2 l)
{
    float res = grassTerrain.y;
    float cell = grassTerrain.x / (res - 1.0);
    vec2 f = clamp((l + 0.5 * grassTerrain.x) / cell, vec2(0.0), vec2(res - 1.001));
    ivec2 i0 = ivec2(f);
    vec2 t = f - vec2(i0);
    float h00 = texelFetch(grassHeightTex, i0, 0).r;
    float h10 = texelFetch(grassHeightTex, i0 + ivec2(1, 0), 0).r;
    float h01 = texelFetch(grassHeightTex, i0 + ivec2(0, 1), 0).r;
    float h11 = texelFetch(grassHeightTex, i0 + ivec2(1, 1), 0).r;
    return grassTerrain.z + mix(mix(h00, h10, t.x), mix(h01, h11, t.x), t.y) * grassTerrain.w;
}

// Sum of travelling sine waves plus a scrolling noise ripple: a height-field gradient, no textures.
vec3 waterNormal(vec2 p, float time, float scale, float strength, float distanceFade)
{
    vec2 g = vec2(0.0);
    const vec2 dirs[5] = vec2[5](vec2(0.92, 0.38), vec2(-0.55, 0.83), vec2(0.18, -0.98), vec2(-0.87, -0.49), vec2(0.62, -0.78));
    const float freqs[5] = float[5](1.9, 3.1, 4.7, 7.3, 11.1);
    const float speeds[5] = float[5](0.9, 1.15, 1.45, 1.8, 2.3);
    const float amps[5] = float[5](0.050, 0.036, 0.026, 0.017, 0.011);
    for (int k = 0; k < 5; ++k) {
        float f = freqs[k] / max(scale, 0.05);
        float phase = dot(p, dirs[k]) * f + time * speeds[k];
        g += dirs[k] * (cos(phase) * f * amps[k]);
    }
    vec2 q = p / max(scale, 0.05);
    float e = 0.06;
    float n0 = wetValueNoise(q * 5.0 + vec2(time * 0.35, -time * 0.22));
    float nx = wetValueNoise((q + vec2(e, 0.0)) * 5.0 + vec2(time * 0.35, -time * 0.22));
    float nz = wetValueNoise((q + vec2(0.0, e)) * 5.0 + vec2(time * 0.35, -time * 0.22));
    g += vec2(nx - n0, nz - n0) / e * 0.045;
    g *= strength * distanceFade;
    return normalize(vec3(-g.x, 1.0, -g.y));
}

void waterMain()
{
    vec3 localPos = (waterInverse * vec4(FragPos, 1.0)).xyz;
    float bed = waterBedHeight(localPos.xz);
    float depth = localPos.y - bed;
    if (depth <= 0.004) discard;

    vec3 V = normalize(viewPos - FragPos);
    float viewDistance = length(viewPos - FragPos);
    float time = waterParams2.y;
    float distanceFade = 1.0 - smoothstep(25.0, 90.0, viewDistance) * 0.6;
    vec3 N = waterNormal(FragPos.xz, time, waterParams.y, waterParams.z, distanceFade);
    float NoV = clamp(dot(N, V), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);
    fresnel = clamp(fresnel, 0.0, 1.0);

    float absorb = 1.0 - exp(-depth / max(waterParams.x, 0.05));
    vec3 shallow = pow(max(waterShallow, vec3(0.0)), vec3(2.2));
    vec3 deep = pow(max(waterDeep, vec3(0.0)), vec3(2.2));
    vec3 body = mix(shallow, deep, smoothstep(0.0, 1.0, absorb));

    // Light on the water body: sky from above plus the sun's share, shadowed by trees.
    vec3 skyLight;
    if (hasEnvironmentCube && environmentIntensity > 0.0) {
        skyLight = environmentIrradiance(vec3(0.0, 1.0, 0.0)) * environmentIntensity * vegetationAmbientScale().x;
    } else {
        skyLight = ambientStrength * ambientColor * ambientHemisphereTint(vec3(0.0, 1.0, 0.0)) * 3.0;
    }
    vec3 sunBody = vec3(0.0);
    vec3 specular = vec3(0.0);
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= lightCount) break;
        float intensity = lightIntensityArr[i];
        if (intensity <= 0.0) continue;
        int ltype = lightTypeArr[i];
        vec3 L;
        float attenuation = 1.0;
        if (ltype == 0) {
            L = -normalize(lightDirArr[i]);
        } else if (ltype == 1 || ltype == 2) {
            vec3 d = lightPosArr[i] - FragPos;
            float dist = length(d);
            L = d / max(dist, 1e-4);
            float range = lightRangeArr[i];
            if (range > 0.0) {
                if (dist > range) continue;
                float falloff = clamp(1.0 - dist / range, 0.0, 1.0);
                attenuation = falloff * falloff;
            }
            if (ltype == 2) {
                float cosTheta = dot(-L, normalize(lightDirArr[i]));
                attenuation *= smoothstep(lightOuterCosArr[i], lightInnerCosArr[i], cosTheta);
            }
        } else {
            continue;
        }
        if (attenuation <= 0.0 || L.y <= 0.0) continue;
        float shadow = 0.0;
        if (lightShadowKindArr[i] != 0) {
            shadow = computeShadowOcclusion(i, FragPos - lightPosArr[i], max(L.y, 0.05), FragPos);
        }
        vec3 lightColor = lightColorArr[i] * (intensity * attenuation) * (1.0 - shadow);
        sunBody += lightColor * (0.18 * L.y);
        vec3 Hv = normalize(L + V);
        float nh = max(dot(N, Hv), 0.0);
        specular += lightColor * (pow(nh, 900.0) * 18.0 + pow(nh, 90.0) * 0.45) * fresnel;
    }
    vec3 bodyLit = body * (skyLight * 0.9 + sunBody);

    // Sky reflection from the environment cube; a flat sky colour when there is none.
    vec3 R = reflect(-V, N);
    R.y = abs(R.y);
    vec3 reflection;
    if (hasEnvironmentCube && environmentIntensity > 0.0) {
        reflection = sampleEnvironmentSpecular(R, 0.04);
    } else {
        reflection = ambientColor * (ambientStrength * 5.0 + 0.1) * mix(vec3(0.7, 0.8, 1.0), vec3(0.5, 0.7, 1.0), R.y) * 2.0;
    }
    reflection *= waterParams.w * (skyLightingReflections ? 1.0 : 0.0);

    vec3 color = bodyLit * (1.0 - fresnel) + reflection * fresnel + specular;
    float alpha = mix(0.30, 0.97, smoothstep(0.0, 1.0, absorb)) + fresnel * 0.9;

    // Shore: soft edge and a ragged band of foam where the water thins out.
    float shoreNoise = wetValueNoise(FragPos.xz * 3.1 + vec2(time * 0.12, time * 0.08)) * 0.7 +
                       wetValueNoise(FragPos.xz * 9.0 - vec2(time * 0.2, 0.0)) * 0.3;
    float foam = (1.0 - smoothstep(0.02, 0.10 + 0.20 * shoreNoise, depth)) * waterParams2.x;
    foam = clamp(foam * (0.55 + 0.9 * shoreNoise), 0.0, 1.0);
    vec3 foamColor = vec3(0.78, 0.82, 0.80) * (skyLight * 0.5 + vec3(0.25));
    color = mix(color, foamColor, foam);
    alpha = max(alpha, foam);
    alpha *= smoothstep(0.004, 0.06, depth);

    vec3 finalColor = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    FragColor = vec4(applyFog(finalColor), clamp(alpha, 0.0, 1.0));
}
#endif // MODU_WATER

void main()
{
#if defined(MODU_GRASS) && !defined(MODU_ES2)
    grassMain();
}
#elif defined(MODU_WATER) && !defined(MODU_ES2)
    waterMain();
}
#else
    vec2 uv = TexCoord * uvTransform.xy + uvTransform.zw;
    vec3 norm = normalize(Normal);
    // Before normal mapping: the clearcoat and any water sit on the smooth shape,
    // not on the base layer's surface detail.
    vec3 geomN = norm;
    vec3 viewDir = normalize(viewPos - FragPos);

    // Parallax is part of the PBR feature set: the Inspector hides the height
    // controls while Physically Based is off, so the shader must not keep
    // displacing with values the user can no longer see. gbuffer_frag.glsl
    // mirrors this gate.
#ifdef MODU_ES2
    bool wantsParallax = false;
#else
    bool wantsParallax = usePbr && hasHeightMap && heightScale > 0.0;
#endif

    // Both parallax and normal mapping want the same tangent frame, so it is
    // built once here. The condition is uniform across the draw, which keeps the
    // derivatives inside it well defined.
    bool wantsTangentFrame = hasNormalMap || wantsParallax;
    mat3 TBN = mat3(1.0);
    if (wantsTangentFrame) {
        TBN = computeTBN(uv, norm);
    }

    // Displacement runs before every other fetch: albedo, overlay, normal and
    // the PBR maps all have to be read at the displaced coordinate or the
    // surface detail slides against the silhouette.
#ifndef MODU_ES2
    if (wantsParallax) {
        // TBN is orthonormal enough that its transpose inverts it, which is the
        // cheap way to get the view ray into tangent space.
        vec3 viewTangent = normalize(transpose(TBN) * viewDir);
        uv = parallaxOcclusionUv(uv, viewTangent);
    }
#endif

    // Texture mixing (corrected)
#ifdef MODU_TERRAIN
    TerrainSurface ts = terrainEvaluate(FragPos, geomN, TexCoord);
    vec4 tex1 = vec4(ts.albedo, 1.0);
#else
    vec4 tex1 = texture(texture1, uv);
#endif
    vec3 texColor = tex1.rgb;
    if (hasOverlay) {
        vec4 overlay = texture(overlayTex, uv);
        texColor = mix(texColor, overlay.rgb, overlay.a * mixAmount);
    }
    vec3 baseColor = texColor * materialColor;
    // A dedicated opacity map replaces the albedo's alpha channel rather than
    // multiplying it: the assets that need one ship an opaque JPEG base colour
    // whose alpha is a constant 1, so multiplying would work, but an opaque PNG
    // base map would then silently zero the mask.
    float alpha = hasOpacityMap ? (texture(opacityMap, uv).r * materialAlpha)
                                : (tex1.a * materialAlpha);
    // Cutout before blending. Tested first so a material with a cutoff behaves
    // identically whether its mask comes from a map or from the albedo's alpha.
    if (alphaCutoff > 0.0 && alpha < alphaCutoff) {
        discard;
    }
    if (alpha <= 0.001) {
        discard;
    }
    if (depthPrepass) {
        FragColor = vec4(0.0);
        return;
    }

    if (unlit) {
        // Emission works here too - a glowing sprite is one of the main reasons
        // to reach for it. Added straight to baseColor rather than to linear
        // radiance because the unlit path never decodes or re-encodes, so this
        // is already the same space the value is written in. The practical
        // difference from the lit path is only where the bloom threshold falls.
        if (emissionStrength > 0.0) {
            vec3 emission = emissionColor;
            if (hasEmissiveMap) {
                emission *= texture(emissiveMap, uv).rgb;
            }
            baseColor += emission * emissionStrength;
        }
        FragColor = vec4(applyFog(baseColor), alpha);
        return;
    }

    // Normal map (tangent-space), using the frame built above main's fetches.
    if (hasNormalMap) {
        vec3 mapN = texture(normalMap, uv).xyz * 2.0 - 1.0;
        mapN.xy *= max(normalMapIntensity, 0.0);
        mapN = normalize(mapN);
        norm = viewFacingMappedNormal(normalize(TBN * mapN), normalize(Normal), viewDir);
    }

#ifdef MODU_TERRAIN
    norm = viewFacingMappedNormal(ts.normal, geomN, viewDir);
#endif
    vec3 albedo = pow(max(baseColor, vec3(0.0)), vec3(2.2));
    float metallic;
    float roughness;
    float smoothness;
    if (usePbr) {
        // The sliders are the base value. A map REPLACES its channel rather
        // than scaling it. Scaling read well on paper - "the slider stays a
        // global trim" - but the metallic slider defaults to 0, so dropping in
        // an authored metalness mask multiplied it away to nothing and the
        // surface stayed a uniform dielectric however the texture was painted.
        // Replacing is also what every other engine's metallic map does.
        metallic = clamp(pbrMetallic, 0.0, 1.0);
        roughness = clamp(pbrRoughness, 0.03, 1.0);
        if (hasMetallicRoughnessMap) {
            // glTF packing: roughness in G, metallic in B.
            vec3 mr = texture(metallicRoughnessMap, uv).rgb;
            roughness = clamp(mr.g, 0.03, 1.0);
            metallic = clamp(mr.b, 0.0, 1.0);
        }
        // Dedicated single-channel maps win over the packed one, per channel, so
        // a project can mix a glTF ORM on one material with the separate
        // greyscale files Poly Haven and ambientCG ship on the next.
#ifndef MODU_ES2
        if (hasMetallicMap) {
            metallic = clamp(texture(metallicMap, uv).r, 0.0, 1.0);
        }
        if (hasRoughnessMap) {
            roughness = clamp(texture(roughnessMap, uv).r, 0.03, 1.0);
        }
#endif
        smoothness = 1.0 - roughness;
    } else {
        // Legacy reinterpretation: specular strength stands in for metalness and
        // the Blinn-Phong exponent for smoothness.
        metallic = clamp(specularStrength, 0.0, 1.0);
        smoothness = clamp(shininess / 256.0, 0.0, 1.0);
        roughness = clamp(1.0 - smoothness, 0.03, 1.0);
    }

#ifdef MODU_TERRAIN
    metallic = 0.0;
    roughness = ts.roughness;
    smoothness = 1.0 - roughness;
#endif
    float coatAmount = usePbr ? clamp(clearcoat, 0.0, 1.0) : 0.0;
    float coatRough = clamp(clearcoatRoughness, 0.02, 1.0);
    vec3 coatN = geomN;
#ifndef MODU_ES2
    // Uniform branch: a dry scene pays one comparison.
    if (surfaceWetness > 0.001 && wetnessReceive) {
        applySurfaceWetness(FragPos, geomN, clamp(surfaceWetness, 0.0, 1.0), rainAmount, wetTime,
                            puddleAmount, metallic, albedo, roughness, norm,
                            coatAmount, coatRough, coatN);
        roughness = clamp(roughness, 0.03, 1.0);
        smoothness = 1.0 - roughness;
    }
#endif
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuseColor = albedo * (1.0 - metallic);

    // Ambient occlusion darkens indirect light only - direct light is already
    // occluded by the shadow maps, and folding AO into it double-darkens.
    float occlusion = 1.0;
#ifdef MODU_TERRAIN
    occlusion = ts.ao;
#endif
    if (hasOcclusionMap) {
        occlusion = mix(1.0, texture(occlusionMap, uv).r, clamp(occlusionStrength, 0.0, 1.0));
    }

    vec3 ambient = ambientStrength * ambientColor * ambientHemisphereTint(norm) * diffuseColor * occlusion;
    vec3 lighting = ambient;

    // Both of these depend only on the material's smoothness, so they are loop
    // invariant - computing them per light cost two pow() per light per
    // fragment. The PBR path and specular-off path never read them at all.
    float specPower = 0.0;
    float specEnergy = 0.0;
    if (specularEnabled && !usePbr) {
        specPower = mix(2.0, 4096.0, pow(smoothness, 1.35));
        specEnergy = mix(0.0, 2.8, pow(smoothness, 0.7));
    }

    // Constant trip count with a break: GLSL ES 1.00 only allows loop bounds
    // known at compile time, and has no integer min().
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= lightCount) break;
        int ltype = lightTypeArr[i];
        float intensity = lightIntensityArr[i];
        if (intensity <= 0.0) continue;

        vec3 lDirN = vec3(0.0, 1.0, 0.0);
        float attenuation = 1.0;
        bool isArea = false;
        vec3 areaNormal = vec3(0.0, 1.0, 0.0);
        vec3 areaTangent = vec3(1.0, 0.0, 0.0);
        vec3 areaBitangent = vec3(0.0, 0.0, 1.0);
        vec3 areaCenter = vec3(0.0);
        vec2 areaHalfSize = vec2(0.5);

        if (ltype == 0) {
            lDirN = -normalize(lightDirArr[i]);
        } else if (ltype == 3) { // area light approximate
            isArea = true;
            areaNormal = normalize(lightDirArr[i]);
            vec3 up = abs(areaNormal.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
            areaTangent = normalize(cross(up, areaNormal));
            areaBitangent = cross(areaNormal, areaTangent);

            areaCenter = lightPosArr[i];
            vec3 rel = FragPos - areaCenter;
            float distPlane = dot(rel, areaNormal);
            vec3 onPlane = FragPos - distPlane * areaNormal;
            areaHalfSize = lightAreaSizeArr[i] * 0.5;
            vec2 local;
            local.x = dot(onPlane - areaCenter, areaTangent);
            local.y = dot(onPlane - areaCenter, areaBitangent);

            float fade = clamp(lightAreaFadeArr[i], 0.0, 1.0);
            vec2 absLocal = abs(local);
            float edgeWeight = 1.0;
            if (fade < 0.0001) {
                if (absLocal.x > areaHalfSize.x || absLocal.y > areaHalfSize.y) continue;
            } else {
                vec2 inner = areaHalfSize * (1.0 - fade);
                vec2 delta = max(areaHalfSize - inner, vec2(0.0001));
                vec2 outside = max(absLocal - inner, vec2(0.0));
                float maxOutside = max(outside.x / delta.x, outside.y / delta.y);
                edgeWeight = 1.0 - clamp(maxOutside, 0.0, 1.0);
                if (edgeWeight <= 0.0) continue;
                edgeWeight = smoothstep(0.0, 1.0, edgeWeight);
            }

            vec3 closest = areaCenter + areaTangent * local.x + areaBitangent * local.y;

            vec3 lvec = closest - FragPos;
            float dist = length(lvec);
            if (dist < 1e-4) continue;
            lDirN = normalize(lvec);

            float range = lightRangeArr[i];
            if (range > 0.0 && dist > range) continue;
            if (range > 0.0) {
                float falloff = clamp(1.0 - (dist / range), 0.0, 1.0);
                attenuation = falloff * falloff;
            }
            float facing = max(dot(areaNormal, -lDirN), 0.0);
            attenuation *= facing * edgeWeight;
        } else {
            vec3 ldir = lightPosArr[i] - FragPos;
            float dist = length(ldir);
            lDirN = normalize(ldir);

            float range = lightRangeArr[i];
            if (range > 0.0 && dist > range) continue;
            if (range > 0.0) {
                float falloff = clamp(1.0 - (dist / range), 0.0, 1.0);
                attenuation = falloff * falloff;
            }
        }

        if (ltype == 2) {
            float cosTheta = dot(-lDirN, normalize(lightDirArr[i]));
            float spotAtten = smoothstep(lightOuterCosArr[i], lightInnerCosArr[i], cosTheta);
            attenuation *= spotAtten;
        }

        // Outside the spot/area falloff this light contributes nothing.
        if (attenuation <= 0.0) continue;

        float nl = max(dot(norm, lDirN), 0.0);
        // Both diffuse and direct specular are zero on this side of the
        // normal. Area samples can face differently from their centre ray.
        if (nl <= 0.0 && !isArea) continue;
        vec3 diffuse = nl * diffuseColor * lightColorArr[i] * intensity;
        if (usePbr) {
            // Energy split: light the microfacet layer reflects cannot also
            // scatter diffusely. Uses this light's own half-vector so the term
            // matches the Fresnel the specular lobe below actually applies.
            vec3 halfVec = normalize(viewDir + lDirN);
            diffuse *= vec3(1.0) - fresnelSchlick(max(dot(viewDir, halfVec), 0.0), F0);
        }

        vec3 specular;
        if (!specularEnabled) {
            specular = vec3(0.0);
        } else if (isArea) {
#ifdef MODU_ES2
            specular = evaluateDirectSpecular(
                norm, viewDir, lDirN, F0,
                roughness, smoothness,
                specPower, specEnergy,
                lightColorArr[i], intensity,
                usePbr
            ) * attenuation;
#else
            vec3 sampleOffsets[9] = vec3[](
                vec3(0.0),
                areaTangent * areaHalfSize.x,
                -areaTangent * areaHalfSize.x,
                areaBitangent * areaHalfSize.y,
                -areaBitangent * areaHalfSize.y,
                areaTangent * areaHalfSize.x + areaBitangent * areaHalfSize.y,
                areaTangent * areaHalfSize.x - areaBitangent * areaHalfSize.y,
                -areaTangent * areaHalfSize.x + areaBitangent * areaHalfSize.y,
                -areaTangent * areaHalfSize.x - areaBitangent * areaHalfSize.y
            );

            specular = vec3(0.0);
            float sampleWeight = 0.0;
            for (int s = 0; s < 9; ++s) {
                vec3 samplePos = areaCenter + sampleOffsets[s];
                vec3 sampleVec = samplePos - FragPos;
                float sampleDist = length(sampleVec);
                if (sampleDist < 1e-4) continue;
                vec3 sampleL = sampleVec / sampleDist;
                float sampleFacing = max(dot(areaNormal, -sampleL), 0.0);
                if (sampleFacing <= 0.0) continue;

                float sampleAtten = attenuation;
                float range = lightRangeArr[i];
                if (range > 0.0) {
                    float falloff = clamp(1.0 - (sampleDist / range), 0.0, 1.0);
                    sampleAtten *= falloff * falloff;
                }

                specular += evaluateDirectSpecular(
                    norm, viewDir, sampleL, F0,
                    roughness, smoothness,
                    specPower, specEnergy,
                    lightColorArr[i], intensity,
                    usePbr
                ) * sampleAtten * sampleFacing;
                sampleWeight += 1.0;
            }
            if (sampleWeight > 0.0) {
                specular /= sampleWeight;
            }
#endif
        } else {
            specular = evaluateDirectSpecular(
                norm, viewDir, lDirN, F0,
                roughness, smoothness,
                specPower, specEnergy,
                lightColorArr[i], intensity,
                usePbr
            );
            specular *= attenuation;
        }

        if (coatAmount > 0.0 && specularEnabled) {
            // The coat reflects first; what it reflects never reaches the base.
            float coatF;
            float coatSpec = clearcoatLobe(coatN, viewDir, lDirN, coatRough, coatAmount, coatF);
            diffuse *= 1.0 - coatF;
            specular = specular * (1.0 - coatF) +
                       coatSpec * lightColorArr[i] * (intensity * attenuation);
        }

        float shadow = 0.0;
#ifndef MODU_ES2
        if (lightShadowKindArr[i] != 0) {
            shadow = computeShadowOcclusion(i, FragPos - lightPosArr[i], nl, FragPos);
            if (shadow > 0.0 && lightTypeArr[i] != 0) {
                shadow *= shadowOpacityAt(i, length(FragPos - lightPosArr[i]));
            }
        }
#endif

        lighting += (1.0 - shadow) * (attenuation * diffuse + specular);

    }

    // Indirect diffuse from the voxel volume. Scaled by the occlusion map for
    // the same reason ambient is: this is indirect light, and the shadow maps
    // have already occluded the direct term.
#ifndef MODU_ES2
    if (giEnabled && giIntensity > 0.0) {
        vec3 indirect = gatherGiDiffuse(FragPos, norm);
        lighting += diffuseColor * indirect * giIntensity * occlusion;
    }
#endif

    // Image-based lighting. Indirect, so the occlusion map applies, and the
    // diffuse part follows the material's ambient strength the way the flat
    // ambient term does (0.2, the default, is 1x).
    bool iblSpecular = false;
#ifndef MODU_ES2
    {
        float NoV = clamp(dot(norm, viewDir), 1e-4, 1.0);
        vec3 indirect = vec3(0.0);
        if (hasEnvironmentCube && environmentIntensity > 0.0) {
            float ambientScale = (ambientStrength <= 0.2) ? ambientStrength * 5.0
                                                          : 1.0 + (ambientStrength - 0.2) * 1.25;
            vec3 irradiance = environmentIrradiance(norm);
            vec3 kD = usePbr ? (vec3(1.0) - envBRDFApprox(F0, roughness, NoV)) : vec3(1.0);
            indirect += diffuseColor * irradiance * kD *
                        (occlusion * ambientScale * environmentIntensity);
        }
        bool haveEnvironment = (hasEnvironmentCube && skyLightingReflections) || (hasReflectionCast && reflectionIsProbe);
        if (usePbr && specularEnabled && haveEnvironment) {
            iblSpecular = true;
            vec3 R = reflect(-viewDir, norm);
            // Rough lobes lean toward the normal (Frostbite's dominant direction).
            float a = roughness * roughness;
            R = normalize(mix(norm, R, (1.0 - a) * (sqrt(1.0 - a) + a)));
            // Horizon occlusion: a normal-mapped reflection that dips under the
            // surface would otherwise fetch the ground half of the sky.
            float horizon = clamp(1.0 + 1.2 * dot(R, geomN), 0.0, 1.0);
            float specOcclusion = hasOcclusionMap
                ? clamp(pow(NoV + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion, 0.0, 1.0)
                : 1.0;
            indirect += sampleEnvironmentSpecular(R, roughness) * envBRDFApprox(F0, roughness, NoV) *
                        (horizon * horizon * specOcclusion);
        }
        if (coatAmount > 0.0 && specularEnabled) {
            float NoVc = clamp(dot(coatN, viewDir), 1e-4, 1.0);
            float coatF = envBRDFApprox(vec3(0.04), coatRough, NoVc).x * coatAmount;
            indirect *= 1.0 - coatF;
            if (haveEnvironment) {
                vec3 Rc = reflect(-viewDir, coatN);
                float horizonC = clamp(1.0 + 1.2 * dot(Rc, geomN), 0.0, 1.0);
                indirect += sampleEnvironmentSpecular(Rc, coatRough) *
                            (coatF * horizonC * horizonC * mix(1.0, occlusion, 0.5));
            }
        }
        lighting += indirect;
    }
#endif

    // Legacy cubemap blend, for materials the IBL path above does not cover.
    if (!iblSpecular && specularEnabled && hasReflectionCast && reflectionIntensity > 0.0) {
        vec3 reflectionDir = reflect(-viewDir, norm);
        float distanceFade = smoothstep(reflectionFadeStart, max(reflectionFadeStart + 0.01, reflectionFadeEnd), length(viewPos - FragPos));
        float fresnelBase = 1.0 - clamp(dot(norm, viewDir), 0.0, 1.0);
        float fresnelBase2 = fresnelBase * fresnelBase;
        float fresnel = fresnelBase2 * fresnelBase2 * fresnelBase;
        float reflectivity = smoothness * mix(0.18, 1.0, distanceFade) * mix(0.35, 1.0, fresnel);
        vec3 reflected = texture(reflectionCube, reflectionDir).rgb;
        lighting = mix(lighting, reflected, clamp(reflectivity * reflectionIntensity, 0.0, 1.0));
    }

    // Emission is added to linear radiance before the encode below, so a
    // strength above roughly 1.25 survives it above 1.0 and crosses the bloom
    // extract's default threshold on its own. That is the whole mechanism behind
    // "turn emission up and it glows" - no separate emissive bloom pass.
    // Needs an HDR (16F) color target to mean anything; on an 8-bit target the
    // value clamps at 1.0 and the material just reads as fullbright.
    if (emissionStrength > 0.0) {
        vec3 emission = emissionColor;
        if (hasEmissiveMap) {
            emission *= pow(max(texture(emissiveMap, uv).rgb, vec3(0.0)), vec3(2.2));
        }
        lighting += emission * emissionStrength;
    }

    vec3 finalColor = pow(max(lighting, vec3(0.0)), vec3(1.0 / 2.2));
    finalColor = applyFog(finalColor);
    FragColor = vec4(finalColor, alpha);
}
#endif // grass / water / material main
