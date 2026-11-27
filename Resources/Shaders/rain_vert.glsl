#version 330 core

// World-space rain streaks (RenderRain.cpp). There are no vertex attributes: every
// streak is derived from gl_InstanceID (which drop) and gl_VertexID (which corner),
// so the pass uploads nothing per frame and the drop count is just the instance count.
//
// Drops live in a world-space tile that repeats around the camera, so they stay put in
// the world as the camera moves and wrap only at the far edge of the volume. Two layers:
// a dense near box of thin streaks and a sparse far box of wider, longer ones (LOD).

uniform mat4 view;
uniform mat4 projection;
uniform vec3 cameraPos;
uniform float rainTime;          // seconds, wrapped on the CPU
uniform vec3 rainVelocity;       // world m/s: fall + wind
uniform vec3 cameraVelocity;     // world m/s, smoothed; tilts streaks like a real shutter
uniform int nearCount;           // instances [0, nearCount) are the near layer
// x half extent (m), y column height (m), z exposure (s), w width (m)
uniform vec4 layerNear;
uniform vec4 layerFar;
uniform float pixelWorld;        // world size of one pixel at 1 m distance
uniform float rainAlpha;

uniform bool occlusionEnabled;
uniform mat4 occlusionMatrix;
uniform sampler2D occlusionDepth;
uniform float occlusionBias;     // in occlusion depth units

out vec2 vUv;          // x across the streak -1..1, y tail 0 .. head 1
out float vAlpha;
out vec3 vWorld;
flat out int vKind;    // 0 streak (shared fragment shader with the splashes)
out float vPhase;

uint rainHashU(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float rainHash(uint x) {
    return float(rainHashU(x) & 0x00FFFFFFu) / 16777215.0;
}

// 1 when the point is open to the sky, 0 when something in the occlusion map is above it.
float rainOpenSky(vec3 p) {
    if (!occlusionEnabled) return 1.0;
    vec4 c = occlusionMatrix * vec4(p, 1.0);
    vec2 uv = c.xy * 0.5 + 0.5;
    if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0) return 1.0;
    float d = c.z * 0.5 + 0.5;
    float surface = textureLod(occlusionDepth, uv, 0.0).r;
    return d <= surface + occlusionBias ? 1.0 : 0.0;
}

void main()
{
    vKind = 0;
    vPhase = 0.0;
    int instance = gl_InstanceID;
    bool nearLayer = instance < nearCount;
    vec4 layer = nearLayer ? layerNear : layerFar;
    uint seed = uint(nearLayer ? instance : instance - nearCount) * 4u + (nearLayer ? 0u : 0x9E3779B9u);

    float h0 = rainHash(seed);
    float h1 = rainHash(seed + 1u);
    float h2 = rainHash(seed + 2u);
    float h3 = rainHash(seed + 3u);

    float halfExtent = layer.x;
    float columnHeight = layer.y;
    vec3 tileSize = vec3(halfExtent * 2.0, columnHeight, halfExtent * 2.0);

    // Bigger drops fall a little faster; the spread keeps the volume from moving as a sheet.
    float speedScale = 0.82 + 0.36 * h3;
    vec3 velocity = rainVelocity * speedScale;
    vec3 basePos = vec3(h0, h1, h2) * tileSize;
    vec3 pos = basePos + velocity * rainTime;

    // Wrap into the volume around the camera: horizontally centred, vertically biased
    // upward because rain below the eye is mostly hidden by the ground anyway.
    vec3 lower = vec3(halfExtent, columnHeight * 0.3, halfExtent);
    vec3 rel = mod(pos - cameraPos + lower, tileSize) - lower;
    vec3 head = cameraPos + rel;

    float horizontal = max(abs(rel.x), abs(rel.z));
    float edgeFade = 1.0 - smoothstep(halfExtent * 0.7, halfExtent, horizontal);
    if (!nearLayer) {
        // The far layer fills in only where the near layer has faded out.
        float nearHalf = layerNear.x;
        edgeFade *= smoothstep(nearHalf * 0.6, nearHalf * 0.95, horizontal);
    }
    float verticalFade = smoothstep(0.0, columnHeight * 0.12, rel.y + lower.y) *
                         (1.0 - smoothstep(columnHeight * 0.55, columnHeight * 0.7, rel.y));

    // Streak along the drop's motion relative to the camera.
    vec3 relVelocity = velocity - cameraVelocity;
    float speed = max(length(relVelocity), 0.001);
    vec3 axis = relVelocity / speed;
    float streakLength = clamp(speed * layer.z, 0.05, 2.5);
    vec3 tail = head - axis * streakLength;

    vec3 mid = 0.5 * (head + tail);
    vec3 toCamera = cameraPos - mid;
    float dist = length(toCamera);
    vec3 side = cross(axis, toCamera / max(dist, 0.001));
    float sideLen = length(side);
    side = sideLen > 1e-4 ? side / sideLen : vec3(view[0][0], view[1][0], view[2][0]);

    // Thin streaks alias into flicker below a pixel: widen to ~1.3 px and trade the
    // extra width for alpha, which keeps the same amount of "water" on screen.
    float width = layer.w * (0.7 + 0.6 * h2);
    float minWidth = pixelWorld * dist * 1.3;
    float coverage = width / max(width, minWidth);
    width = max(width, minWidth);

    float visible = edgeFade * verticalFade * rainOpenSky(tail);
    // Nothing right at the lens: those would be screen-filling smears.
    visible *= smoothstep(0.35, 1.2, dist);
    // Far streaks are fewer but bigger; dim them a touch so they read as depth, not size.
    float layerAlpha = nearLayer ? 1.0 : 0.7;
    vAlpha = rainAlpha * layerAlpha * coverage * visible * (0.55 + 0.45 * h1);

    int corner = gl_VertexID % 6;
    // Two triangles: (0,1,2) (2,1,3) over corners x in {-1,1}, y in {0,1}.
    int ci = corner == 0 ? 0 : (corner == 1 || corner == 4) ? 1 : (corner == 2 || corner == 3) ? 2 : 3;
    float cx = (ci == 1 || ci == 3) ? 1.0 : -1.0;
    float cy = (ci >= 2) ? 1.0 : 0.0;
    vec3 world = mix(tail, head, cy) + side * (cx * width * 0.5);

    if (vAlpha <= 0.0005) {
        // Collapse culled drops to a degenerate point outside the clip volume.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vUv = vec2(0.0);
        vWorld = head;
        return;
    }
    vUv = vec2(cx, cy);
    vWorld = world;
    gl_Position = projection * view * vec4(world, 1.0);
}
