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
uniform sampler2D metallicMap;
uniform sampler2D roughnessMap;
uniform sampler2D opacityMap;

// -- Parallax occlusion mapping --
// Unit 15, the last one GL 3.3 core guarantees. Any further map has to share a
// slot or move to an array texture.
uniform bool hasHeightMap = false;
uniform float heightScale = 0.0;
uniform int parallaxSteps = 32;
uniform bool parallaxInvertHeight = false;
uniform sampler2D heightMap;

// -- Voxel cone traced global illumination --
// Unit 16, past the 16 that GL 3.3 core guarantees. That is deliberate and it is
// why the renderer refuses to switch GI on unless the driver reports at least 17
// fragment texture units - which every GL 4.3 part does in practice, and 4.3 is
// already the floor for the voxelization pass. Nothing here is sampled while
// giEnabled is false, so a 3.3 context simply never touches it.
uniform bool giEnabled = false;
uniform sampler3D voxelVolume;
uniform vec3 giVolumeMin = vec3(0.0);
uniform float giVolumeExtent = 1.0;
uniform float giVolumeResolution = 128.0;
uniform float giIntensity = 1.0;
uniform int giConeSteps = 24;

uniform bool fogEnabled = false;
uniform int fogMode = 0;
uniform vec3 fogColor = vec3(0.65, 0.72, 0.78);
uniform float fogStart = 20.0;
uniform float fogEnd = 120.0;
uniform float fogDensity = 0.015;
uniform float fogHeight = 0.0;
uniform float fogHeightFalloff = 0.0;

const int MAX_LIGHTS = 32;
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
uniform int lightShadowMapArr[MAX_LIGHTS];
uniform int lightShadowKindArr[MAX_LIGHTS]; // 0 off, 1 cube, 2 directional
uniform int lightShadowModeArr[MAX_LIGHTS]; // 0 off, 1 hard, 2 soft
uniform float lightShadowBiasArr[MAX_LIGHTS];
uniform float lightShadowSoftnessArr[MAX_LIGHTS];
uniform float lightShadowFarArr[MAX_LIGHTS];
uniform mat4 lightShadowMatrixArr[MAX_LIGHTS];

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
vec2 vogelDiskSample(int index, int count, float phi)
{
    const float kGoldenAngle = 2.39996323;
    float r = sqrt(float(index) + 0.5) / sqrt(float(count));
    float theta = float(index) * kGoldenAngle + phi;
    return vec2(r * cos(theta), r * sin(theta));
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
                              vec2 texelSize, float searchRadius, int taps, float phi)
{
    float sum = 0.0;
    float count = 0.0;
    for (int i = 0; i < taps; ++i) {
        vec2 offset = vogelDiskSample(i, taps, phi) * texelSize * searchRadius;
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

        float closestDepth = sampleDirectionalShadow(mapIndex, projCoords.xy);
        if (mode == 1) {
            return (projCoords.z - bias > closestDepth) ? 1.0 : 0.0;
        }

        float softness = max(lightShadowSoftnessArr[lightIndex], 0.0);
        vec2 texelSize = getDirectionalShadowTexelSize(mapIndex);
        if (softness <= 0.0001 || texelSize.x <= 0.0 || texelSize.y <= 0.0 ||
            shadowFilterQuality <= 0) {
            return (projCoords.z - bias > closestDepth) ? 1.0 : 0.0;
        }

        float viewDistance = length(worldPos - viewPos);
        int taps = shadowTapCount(viewDistance);
        float phi = interleavedGradientNoise(gl_FragCoord.xy) * 6.2831853;
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
                                                    max(taps / 2, 4), phi);
            if (blocker < 0.0) return 0.0;
            float gap = max(projCoords.z - blocker, 0.0);
            radius = clamp(gap / max(blocker, 0.0001) * maxRadius, 1.0, maxRadius);
        }

        // Gaussian-weighted accumulation. Weighting by distance from the centre
        // is what turns a disc of binary tests into a smooth falloff; a flat
        // average of the same taps still steps.
        float shadow = 0.0;
        float weightSum = 0.0;
        for (int i = 0; i < taps; ++i) {
            vec2 disk = vogelDiskSample(i, taps, phi);
            float w = exp(-2.0 * dot(disk, disk));
            vec2 offset = disk * texelSize * radius;
            float sampleDepth = sampleDirectionalShadow(mapIndex, projCoords.xy + offset);
            shadow += w * ((projCoords.z - bias > sampleDepth) ? 1.0 : 0.0);
            weightSum += w;
        }
        return shadow / max(weightSum, 0.0001);
    }

    float farPlane = max(lightShadowFarArr[lightIndex], 0.001);
    float currentDepth = length(lightToFrag);
    if (currentDepth <= 0.0001) return 0.0;
    float hardDepth = sampleShadowCube(mapIndex, lightToFrag) * farPlane;
    if (mode == 1) {
        return (currentDepth - bias > hardDepth) ? 1.0 : 0.0;
    }

    float softness = max(lightShadowSoftnessArr[lightIndex], 0.0);
    if (softness <= 0.0001) {
        return (currentDepth - bias > hardDepth) ? 1.0 : 0.0;
    }

    if (shadowFilterQuality <= 0) {
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
    float maxRadius = softness * (1.0 + currentDepth / farPlane);

    // Same contact-hardening idea as the directional path: size the penumbra by
    // how far the receiver sits behind whatever is blocking it.
    float radius = maxRadius;
    if (shadowContactHardening) {
        float blockerSum = 0.0;
        float blockerCount = 0.0;
        int searchTaps = max(taps / 2, 4);
        for (int i = 0; i < searchTaps; ++i) {
            vec2 disk = vogelDiskSample(i, searchTaps, phi);
            vec3 offset = (tangent * disk.x + bitangent * disk.y) * maxRadius;
            float d = sampleShadowCube(mapIndex, lightToFrag + offset) * farPlane;
            if (currentDepth - bias > d) {
                blockerSum += d;
                blockerCount += 1.0;
            }
        }
        if (blockerCount <= 0.0) return 0.0;
        float blocker = blockerSum / blockerCount;
        float gap = max(currentDepth - blocker, 0.0);
        radius = clamp(gap / max(blocker, 0.0001) * maxRadius, maxRadius * 0.05, maxRadius);
    }

    float shadow = 0.0;
    float weightSum = 0.0;
    for (int i = 0; i < taps; ++i) {
        vec2 disk = vogelDiskSample(i, taps, phi);
        float w = exp(-2.0 * dot(disk, disk));
        vec3 offset = (tangent * disk.x + bitangent * disk.y) * radius;
        float closestDepth = sampleShadowCube(mapIndex, lightToFrag + offset) * farPlane;
        shadow += w * ((currentDepth - bias > closestDepth) ? 1.0 : 0.0);
        weightSum += w;
    }
    return shadow / max(weightSum, 0.0001);
}

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

float geometrySchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / max(NdotX * (1.0 - k) + k, 0.0001);
}

float geometrySmith(float NdotV, float NdotL, float roughness)
{
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
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
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlick(VdotH, F0);
    vec3 microfacet = (D * G * F) / max(4.0 * NdotV * NdotL, 0.0001);

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
    vec3 tangent = normalize(dp1 * duv2.y - dp2 * duv1.y);
    vec3 bitangent = normalize(-dp1 * duv2.x + dp2 * duv1.x);
    return mat3(tangent, bitangent, N);
}

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
    float maxSteps = float(clamp(parallaxSteps, 4, 256));
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

void main()
{
    vec2 uv = TexCoord * uvTransform.xy + uvTransform.zw;
    vec3 norm = normalize(Normal);
    vec3 viewDir = normalize(viewPos - FragPos);

    // Both parallax and normal mapping want the same tangent frame, so it is
    // built once here. The condition is uniform across the draw, which keeps the
    // derivatives inside it well defined.
    bool wantsTangentFrame = hasNormalMap || (hasHeightMap && heightScale > 0.0);
    mat3 TBN = mat3(1.0);
    if (wantsTangentFrame) {
        TBN = computeTBN(uv, norm);
    }

    // Displacement runs before every other fetch: albedo, overlay, normal and
    // the PBR maps all have to be read at the displaced coordinate or the
    // surface detail slides against the silhouette.
    if (hasHeightMap && heightScale > 0.0) {
        // TBN is orthonormal enough that its transpose inverts it, which is the
        // cheap way to get the view ray into tangent space.
        vec3 viewTangent = normalize(transpose(TBN) * viewDir);
        uv = parallaxOcclusionUv(uv, viewTangent);
    }

    // Texture mixing (corrected)
    vec4 tex1 = texture(texture1, uv);
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
        norm = normalize(TBN * mapN);
    }

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
        if (hasMetallicMap) {
            metallic = clamp(texture(metallicMap, uv).r, 0.0, 1.0);
        }
        if (hasRoughnessMap) {
            roughness = clamp(texture(roughnessMap, uv).r, 0.03, 1.0);
        }
        smoothness = 1.0 - roughness;
    } else {
        // Legacy reinterpretation: specular strength stands in for metalness and
        // the Blinn-Phong exponent for smoothness.
        metallic = clamp(specularStrength, 0.0, 1.0);
        smoothness = clamp(shininess / 256.0, 0.0, 1.0);
        roughness = clamp(1.0 - smoothness, 0.03, 1.0);
    }
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuseColor = albedo * (1.0 - metallic);

    // Ambient occlusion darkens indirect light only - direct light is already
    // occluded by the shadow maps, and folding AO into it double-darkens.
    float occlusion = 1.0;
    if (hasOcclusionMap) {
        occlusion = mix(1.0, texture(occlusionMap, uv).r, clamp(occlusionStrength, 0.0, 1.0));
    }

    vec3 ambient = ambientStrength * ambientColor * diffuseColor * occlusion;
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

    int count = min(lightCount, MAX_LIGHTS);
    for (int i = 0; i < count; ++i) {
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

        float nl = max(dot(norm, lDirN), 0.0);
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

        float shadow = 0.0;
        if (lightShadowKindArr[i] != 0) {
            shadow = computeShadowOcclusion(i, FragPos - lightPosArr[i], nl, FragPos);
        }

        lighting += (1.0 - shadow) * (attenuation * diffuse + specular);
    }

    // Indirect diffuse from the voxel volume. Scaled by the occlusion map for
    // the same reason ambient is: this is indirect light, and the shadow maps
    // have already occluded the direct term.
    if (giEnabled && giIntensity > 0.0) {
        vec3 indirect = gatherGiDiffuse(FragPos, norm);
        lighting += diffuseColor * indirect * giIntensity * occlusion;
    }

    if (specularEnabled && hasReflectionCast && reflectionIntensity > 0.0) {
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
