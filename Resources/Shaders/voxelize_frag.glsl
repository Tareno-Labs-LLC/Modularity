#version 430 core
// Voxelization, stage 3 of 3: direct lighting written into the radiance volume.
//
// What lands in a voxel is albedo * direct light - the light a surface would
// bounce, not the light falling on it. The cone tracer in frag.glsl then gathers
// those as the first indirect bounce.
//
// Writes with imageStore and no atomics, so where several triangles land in one
// voxel the last one wins rather than averaging. At a sane voxel size that shows
// up only on thin double-sided geometry, and averaging would need an atomic
// accumulate-and-count pass over a separate integer volume. Worth revisiting if
// flicker on dense meshes ever becomes the complaint.
//
// rgba16f, not rgba8. A sunlit patch in an HDR scene sits well above 1.0, and
// an 8-bit volume stored it clamped - so the light it bounced was weaker than
// reality by exactly the amount it was brighter than white, which is the whole
// reason a sun patch failed to fill a room. Costs twice the memory; that is the
// trade, and it is the right one for the thing this feature exists to do.
layout (rgba16f, binding = 0) uniform writeonly image3D voxelVolume;

in vec3 gWorldPos;
in vec3 gNormal;
in vec2 gUv;

uniform vec3 volumeMin;
uniform float volumeExtent;
uniform int volumeResolution;

uniform sampler2D albedoTex;
uniform bool hasAlbedoTex = false;

// The previous bounce's volume. Empty on the first pass, so that pass stores
// direct light only; each later pass adds what the one before it lit.
uniform sampler3D previousVolume;
uniform bool hasPreviousBounce = false;
uniform int giConeSteps = 24;
uniform vec3 materialColor = vec3(1.0);
uniform vec3 emissionColor = vec3(1.0);
uniform float emissionStrength = 0.0;
uniform vec3 ambientColor = vec3(1.0);
uniform float ambientStrength = 0.2;

const int MAX_LIGHTS = 32;
const int MAX_SHADOW_MAPS = 4;
uniform int lightCount = 0;

uniform int lightTypeArr[MAX_LIGHTS];
uniform vec3 lightDirArr[MAX_LIGHTS];
uniform vec3 lightPosArr[MAX_LIGHTS];
uniform vec3 lightColorArr[MAX_LIGHTS];
uniform float lightIntensityArr[MAX_LIGHTS];
uniform float lightRangeArr[MAX_LIGHTS];
uniform float lightInnerCosArr[MAX_LIGHTS];
uniform float lightOuterCosArr[MAX_LIGHTS];
uniform int lightShadowMapArr[MAX_LIGHTS];
uniform int lightShadowKindArr[MAX_LIGHTS];
uniform float lightShadowBiasArr[MAX_LIGHTS];
uniform float lightShadowFarArr[MAX_LIGHTS];
uniform mat4 lightShadowMatrixArr[MAX_LIGHTS];

uniform samplerCube shadowCube0;
uniform samplerCube shadowCube1;
uniform samplerCube shadowCube2;
uniform samplerCube shadowCube3;
uniform sampler2D dirShadow0;
uniform sampler2D dirShadow1;
uniform sampler2D dirShadow2;
uniform sampler2D dirShadow3;

float sampleShadowCube(int mapIndex, vec3 dir)
{
    if (mapIndex == 0) return texture(shadowCube0, dir).r;
    if (mapIndex == 1) return texture(shadowCube1, dir).r;
    if (mapIndex == 2) return texture(shadowCube2, dir).r;
    if (mapIndex == 3) return texture(shadowCube3, dir).r;
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

float visibility(int i, vec3 worldPos)
{
    int kind = lightShadowKindArr[i];
    int mapIndex = lightShadowMapArr[i];
    // No shadow map means no occlusion information, so the light counts in full.
    // Unlike the volumetric march this is the right default here: a scene with
    // shadows off should still bounce light, just without contact darkening.
    if (kind <= 0 || mapIndex < 0 || mapIndex >= MAX_SHADOW_MAPS) return 1.0;

    float bias = max(lightShadowBiasArr[i], 0.0001);
    if (kind == 2) {
        vec4 lightSpace = lightShadowMatrixArr[i] * vec4(worldPos, 1.0);
        vec3 proj = lightSpace.xyz / max(lightSpace.w, 0.0001);
        proj = proj * 0.5 + 0.5;
        if (proj.z <= 0.0 || proj.z >= 1.0) return 1.0;
        if (proj.x <= 0.0 || proj.x >= 1.0 || proj.y <= 0.0 || proj.y >= 1.0) return 1.0;
        return (proj.z - bias > sampleDirectionalShadow(mapIndex, proj.xy)) ? 0.0 : 1.0;
    }

    vec3 toPos = worldPos - lightPosArr[i];
    float dist = length(toPos);
    if (dist <= 0.0001) return 1.0;
    float far = max(lightShadowFarArr[i], 0.001);
    return (dist - bias > sampleShadowCube(mapIndex, toPos) * far) ? 0.0 : 1.0;
}

// Deliberately the same cone scheme as frag.glsl's gatherGiDiffuse, duplicated
// because GLSL 330/430 has no include and this file must stay standalone. If one
// changes, change both - a receiver gathering differently from an injector makes
// bounce energy drift between passes in a way that is very hard to see.
vec4 traceBounceCone(vec3 origin, vec3 dir, float aperture)
{
    float voxelSize = volumeExtent / max(float(volumeResolution), 1.0);
    float dist = voxelSize;
    vec4 accum = vec4(0.0);

    for (int i = 0; i < 64; ++i) {
        if (i >= giConeSteps || accum.a >= 0.99) break;

        float diameter = max(voxelSize, 2.0 * aperture * dist);
        float lod = log2(max(diameter / voxelSize, 1.0));

        vec3 pos = origin + dir * dist;
        vec3 uvw = (pos - volumeMin) / max(volumeExtent, 1e-6);
        if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) break;

        // Premultiplied front-to-back, matching frag.glsl: the mip chain already
        // weights rgb by coverage, so no extra samp.a on the colour.
        accum += (1.0 - accum.a) * textureLod(previousVolume, uvw, lod);
        dist += diameter * 0.5;
    }
    return accum;
}

vec3 gatherPreviousBounce(vec3 P, vec3 N)
{
    vec3 up = (abs(N.y) < 0.95) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    float voxelSize = volumeExtent / max(float(volumeResolution), 1.0);
    vec3 origin = P + N * voxelSize * 1.5;

    const float aperture = 0.577;   // tan(30 degrees): a 60 degree cone
    vec3 total = traceBounceCone(origin, N, aperture).rgb * 0.25;
    for (int i = 0; i < 5; ++i) {
        float angle = 1.2566370614 * float(i);
        vec3 dir = normalize(0.5 * N + 0.866 * (cos(angle) * T + sin(angle) * B));
        total += traceBounceCone(origin, dir, aperture).rgb * 0.15;
    }
    return total;
}

void main()
{
    vec3 unit = (gWorldPos - volumeMin) / max(volumeExtent, 1e-6);
    if (any(lessThan(unit, vec3(0.0))) || any(greaterThan(unit, vec3(1.0)))) {
        return;
    }
    ivec3 coord = ivec3(unit * float(volumeResolution));
    coord = clamp(coord, ivec3(0), ivec3(volumeResolution - 1));

    vec3 albedo = materialColor;
    if (hasAlbedoTex) {
        albedo *= texture(albedoTex, gUv).rgb;
    }
    // Linear, matching frag.glsl's decode - bouncing gamma-encoded colour would
    // wash the indirect light toward grey.
    albedo = pow(max(albedo, vec3(0.0)), vec3(2.2));

    vec3 norm = normalize(gNormal);
    vec3 direct = ambientStrength * ambientColor;

    int count = min(lightCount, MAX_LIGHTS);
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= count) break;
        float intensity = lightIntensityArr[i];
        if (intensity <= 0.0) continue;

        int ltype = lightTypeArr[i];
        vec3 toLight;
        float attenuation = 1.0;
        if (ltype == 0) {
            toLight = -normalize(lightDirArr[i]);
        } else {
            vec3 delta = lightPosArr[i] - gWorldPos;
            float dist = length(delta);
            if (dist <= 0.0001) continue;
            toLight = delta / dist;
            float range = lightRangeArr[i];
            if (range > 0.0) {
                if (dist > range) continue;
                float falloff = clamp(1.0 - (dist / range), 0.0, 1.0);
                attenuation = falloff * falloff;
            }
            if (ltype == 2) {
                float cosTheta = dot(-toLight, normalize(lightDirArr[i]));
                attenuation *= smoothstep(lightOuterCosArr[i], lightInnerCosArr[i], cosTheta);
            }
        }
        if (attenuation <= 0.0001) continue;

        // Two-sided: a voxel is a volume, not a face, and the triangle that
        // filled it may be the back of the surface a light is actually hitting.
        float nl = abs(dot(norm, toLight));
        if (nl <= 0.0) continue;

        direct += lightColorArr[i] * intensity * attenuation * nl * visibility(i, gWorldPos);
    }

    // Everything reaching this surface, direct and bounced, is what it goes on
    // to bounce itself. Convergence is guaranteed by albedo < 1: each pass
     // returns strictly less than the last put in.
    if (hasPreviousBounce) {
        direct += gatherPreviousBounce(gWorldPos, norm);
    }

    vec3 radiance = albedo * direct;
    if (emissionStrength > 0.0) {
        radiance += emissionColor * emissionStrength;
    }

    // No clamp: the volume is float now, so a surface lit past white bounces
    // light past white. Alpha stays coverage, which is what lets the cone tracer
    // tell an empty voxel from a black one and what makes the mip chain
    // premultiply correctly.
    imageStore(voxelVolume, coord, vec4(max(radiance, vec3(0.0)), 1.0));
}
