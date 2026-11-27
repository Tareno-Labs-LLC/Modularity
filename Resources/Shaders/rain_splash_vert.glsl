#version 330 core

// Rain splashes (RenderRain.cpp). Like the streaks, no vertex attributes: each instance
// is one splash slot that re-spawns at a new random spot every cycle, its height read
// from the top-down rain occlusion map, i.e. the first surface rain would hit there.
// Vertices 0-5 are a flat expanding ring, 6-11 a camera-facing crown of droplets.

uniform mat4 view;
uniform mat4 projection;
uniform vec3 cameraPos;
uniform float rainTime;
uniform float splashRadius;      // m around the camera
uniform float splashPeriod;      // s per splash cycle
uniform float rainAlpha;
uniform float pixelWorld;

uniform mat4 occlusionMatrix;
uniform mat4 occlusionInverse;
uniform sampler2D occlusionDepth;

out vec2 vUv;
out float vAlpha;
out vec3 vWorld;
flat out int vKind;     // 1 ring, 2 crown
out float vPhase;

uint splashHashU(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float splashHash(uint x) {
    return float(splashHashU(x) & 0x00FFFFFFu) / 16777215.0;
}

void hide() {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vUv = vec2(0.0);
    vAlpha = 0.0;
    vWorld = cameraPos;
    vKind = 0;
    vPhase = 0.0;
}

void main()
{
    uint slot = uint(gl_InstanceID);
    float offset = splashHash(slot * 7u + 11u);
    float rate = 0.8 + 0.4 * splashHash(slot * 7u + 12u);
    float cycleTime = rainTime * rate / splashPeriod + offset;
    float cycle = floor(cycleTime);
    float phase = cycleTime - cycle;

    uint seed = splashHashU(slot * 0x9E3779B9u + uint(int(cycle) & 0x00FFFFFF) * 0x85EBCA6Bu);
    // Uniform over a disc, so the ring of splashes has no square corners.
    float angle = splashHash(seed) * 6.2831853;
    float radius = sqrt(splashHash(seed + 1u)) * splashRadius;
    vec2 xz = cameraPos.xz + vec2(cos(angle), sin(angle)) * radius;

    // Where does rain land at this xz? Project a point at camera height to find the
    // texel, read the depth there and turn it back into a world position.
    vec4 probe = occlusionMatrix * vec4(xz.x, cameraPos.y, xz.y, 1.0);
    vec2 uv = probe.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0) { hide(); return; }
    float surface = textureLod(occlusionDepth, uv, 0.0).r;
    if (surface >= 0.9999) { hide(); return; }   // nothing below: open void
    vec4 hit = occlusionInverse * vec4(probe.xy, surface * 2.0 - 1.0, 1.0);
    vec3 ground = hit.xyz / hit.w;

    vec3 toCamera = cameraPos - ground;
    float dist = length(toCamera);
    float distanceFade = 1.0 - smoothstep(splashRadius * 0.6, splashRadius, dist);
    float size = 0.04 + 0.05 * splashHash(seed + 2u);
    // Distant splashes would be sub-pixel; grow them a bit and fade instead of flickering.
    float minSize = pixelWorld * dist * 3.0;
    float coverage = size / max(size, minSize);
    size = max(size, minSize);

    int vertexId = gl_VertexID;
    int kind = vertexId < 6 ? 1 : 2;
    int corner = vertexId % 6;
    int ci = corner == 0 ? 0 : (corner == 1 || corner == 4) ? 1 : (corner == 2 || corner == 3) ? 2 : 3;
    vec2 c = vec2((ci == 1 || ci == 3) ? 1.0 : -1.0, (ci >= 2) ? 1.0 : -1.0);

    vec3 world;
    float alpha;
    if (kind == 1) {
        // Ring: expands over the whole cycle, just above the surface.
        float ringSize = size * (0.5 + 1.8 * phase);
        world = ground + vec3(c.x * ringSize, 0.012, c.y * ringSize);
        alpha = (1.0 - phase) * (1.0 - phase);
    } else {
        // Crown: the droplets thrown up in the first part of the cycle.
        if (phase > 0.4) { hide(); return; }
        vec3 flatToCam = vec3(toCamera.x, 0.0, toCamera.z);
        float flatLen = length(flatToCam);
        vec3 right = flatLen > 1e-3 ? normalize(cross(vec3(0.0, 1.0, 0.0), flatToCam / flatLen))
                                    : vec3(1.0, 0.0, 0.0);
        float t = phase / 0.4;
        float crownWidth = size * (1.0 + 1.2 * t);
        float crownHeight = size * (0.9 + 0.6 * splashHash(seed + 3u));
        world = ground + right * (c.x * crownWidth) + vec3(0.0, (c.y * 0.5 + 0.5) * crownHeight, 0.0);
        alpha = 1.0 - t;
    }

    vAlpha = rainAlpha * alpha * distanceFade * coverage;
    if (vAlpha <= 0.0005) { hide(); return; }
    vUv = c;
    vWorld = world;
    vKind = kind;
    vPhase = phase;
    gl_Position = projection * view * vec4(world, 1.0);
}
