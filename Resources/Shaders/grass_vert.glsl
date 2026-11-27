#version 330 core

// GPU grass (RenderGrass.cpp). There are no vertex attributes and nothing is uploaded per frame: every
// blade is derived from gl_InstanceID (which cell of the ring around the camera) and gl_VertexID (which
// blade of the cell and which corner of it). The cell grid is snapped to the world, so blades stay put
// while the camera moves and the hash of the cell index decides everything about a blade.
//
// Three rings (near / mid / far) are three draws of this program with different cell sizes, blades per
// cell and segment counts (the LOD); a blade's distance decides, with a per-blade random threshold,
// which ring owns it near a boundary, so rings cross-fade by density instead of popping.
//
// The terrain's own data drives it: a height texture (triangulated exactly like the terrain mesh, so
// roots sit on the surface), the splat map (grass weight; none on paths, rock or in ponds) and the
// painted grass-density map. Everything is done in terrain-local space; the terrain object's world
// matrix (`model`) places the blades.
//
// Fragment stage: frag.glsl compiled with MODU_GRASS.

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 viewPos;

uniform sampler2D grassSplat;
uniform sampler2D grassHeightTex;
uniform sampler2D grassDensityTex;

uniform vec4 grassTerrain;         // x size (local m), y height samples per side, z heightOffset, w heightScale
uniform vec3 grassCamera;          // camera in terrain-local space
uniform vec4 grassRing;            // x cell size (m), y blades per cell, z segments per blade, w width multiplier
uniform vec4 grassCells;           // x half cells outside, y half cells of the hole, z unused, w ring index
uniform vec4 grassRect;            // x first cell column, y first cell row, z cells per row of this draw (the visible tile run)
uniform vec4 grassRange;           // x inner boundary radius (0 = none), y outer boundary radius (<=0 = none), z inner fade half width, w outer fade half width
uniform float grassMaxDist;        // blades vanish here
uniform vec4 grassBlade;           // x height, y width, z height variation, w bend
uniform vec4 grassMisc;            // x unused, y dry amount, z wind multiplier, w pixelWorld
uniform float grassKeepScale;      // density: share of a ring's blades that are kept
uniform vec3 grassBaseColor;
uniform vec3 grassTipColor;
uniform vec3 grassDryColor;
uniform vec4 grassPonds[8];        // local x, local z, radius, local water Y
uniform int grassPondCount;

uniform vec4 sceneWind;            // xyz direction, w strength
uniform float sceneWindTurbulence;
uniform float sceneTime;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;                 // x across the blade (-1..1), y along it (0 root .. 1 tip)
out vec4 vGrassColor;              // rgb display colour, a = ambient occlusion
out vec3 vGrassGround;             // terrain normal (world)

uint hashU(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
float hash01(uint x) { return float(hashU(x) & 0x00FFFFFFu) * (1.0 / 16777215.0); }

float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    uvec2 u = uvec2(ivec2(i));
    float a = hash01(u.x * 0x1B873593u + u.y * 0x85EBCA6Bu);
    float b = hash01((u.x + 1u) * 0x1B873593u + u.y * 0x85EBCA6Bu);
    float c = hash01(u.x * 0x1B873593u + (u.y + 1u) * 0x85EBCA6Bu);
    float d = hash01((u.x + 1u) * 0x1B873593u + (u.y + 1u) * 0x85EBCA6Bu);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fetchHeight(ivec2 i)
{
    int res = int(grassTerrain.y);
    i = clamp(i, ivec2(0), ivec2(res - 1));
    return grassTerrain.z + texelFetch(grassHeightTex, i, 0).r * grassTerrain.w;
}

// Height and normal of the terrain at a local XZ, on the same triangles as the terrain mesh and the
// physics heightfield (diagonal a-d).
float terrainHeight(vec2 l, out vec3 n)
{
    float size = grassTerrain.x;
    float res = grassTerrain.y;
    float cell = size / (res - 1.0);
    vec2 f = clamp((l + 0.5 * size) / cell, vec2(0.0), vec2(res - 1.0));
    ivec2 i0 = min(ivec2(f), ivec2(int(res) - 2));
    vec2 t = f - vec2(i0);
    float ha = fetchHeight(i0);
    float hb = fetchHeight(i0 + ivec2(1, 0));
    float hc = fetchHeight(i0 + ivec2(0, 1));
    float hd = fetchHeight(i0 + ivec2(1, 1));
    bool lower = t.y <= t.x;
    float h = lower ? ha + (hb - ha) * t.x + (hd - hb) * t.y
                    : ha + (hd - hc) * t.x + (hc - ha) * t.y;
    float dx = lower ? (hb - ha) : (hd - hc);
    float dz = lower ? (hd - hb) : (hc - ha);
    n = normalize(vec3(-dx / cell, 1.0, -dz / cell));
    return h;
}

void cull()
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    FragPos = vec3(0.0);
    Normal = vec3(0.0, 1.0, 0.0);
    TexCoord = vec2(0.0);
    vGrassColor = vec4(0.0);
    vGrassGround = vec3(0.0, 1.0, 0.0);
}

void main()
{
    int S = int(grassRing.z);
    int vertsPerBlade = 3 * (2 * S - 1);
    int bladesPerCell = int(grassRing.y);
    int blade = gl_VertexID / vertsPerBlade;
    int vtx = gl_VertexID - blade * vertsPerBlade;

    // ---- Cell of the ring -------------------------------------------------------------------
    int n = int(grassCells.x + 0.5);
    int m = int(grassCells.y + 0.5);
    // Each draw covers a rectangle of cells (a run of visible tiles chosen on the CPU); the hole in the
    // middle (cells the inner ring owns) is skipped here.
    int runWidth = int(grassRect.z + 0.5);
    int cx = int(grassRect.x + 0.5) + gl_InstanceID % runWidth;
    int cz = int(grassRect.y + 0.5) + gl_InstanceID / runWidth;
    if (cx >= n - m && cx < n + m && cz >= n - m && cz < n + m) { cull(); return; }
    float cellSize = grassRing.x;
    ivec2 camCell = ivec2(floor(grassCamera.xz / cellSize));
    ivec2 cellId = camCell + ivec2(cx - n, cz - n);
    vec2 cellOrigin = vec2(cellId) * cellSize;

    // ---- Coarse culling on the cell (the whole instance leaves together) ---------------------
    float maxDist = grassMaxDist;
    vec2 cellCenter = cellOrigin + 0.5 * cellSize;
    float cellDist = length(cellCenter - grassCamera.xz);
    float cellRadius = 0.7072 * cellSize;
    if (cellDist - cellRadius > maxDist) { cull(); return; }
    if (grassRange.y > 0.0 && cellDist - cellRadius > grassRange.y + grassRange.w) { cull(); return; }
    if (grassRange.x > 0.0 && cellDist + cellRadius < grassRange.x - grassRange.z) { cull(); return; }
    if (abs(cellCenter.x) > 0.5 * grassTerrain.x || abs(cellCenter.y) > 0.5 * grassTerrain.x) {
        if (abs(cellCenter.x) > 0.5 * grassTerrain.x + cellSize || abs(cellCenter.y) > 0.5 * grassTerrain.x + cellSize) { cull(); return; }
    }

    uint cellSeed = hashU(uint(cellId.x) * 0x1B873593u + hashU(uint(cellId.y) * 0x85EBCA6Bu + uint(grassCells.w + 0.5) * 0x9E3779B9u));

    // One nearest-texel height is plenty for a culling sphere.
    float cellRes = grassTerrain.y;
    vec2 cellF = clamp((cellCenter + 0.5 * grassTerrain.x) / (grassTerrain.x / (cellRes - 1.0)), vec2(0.0), vec2(cellRes - 1.0));
    float cellGround = fetchHeight(ivec2(cellF + 0.5));
    float bladeMax = grassBlade.x * (1.0 + grassBlade.z) * 1.6 + 1.0;
    {
        vec4 c = projection * view * (model * vec4(cellCenter.x, cellGround + 0.5 * bladeMax, cellCenter.y, 1.0));
        float r = (cellRadius + 0.5 * bladeMax) * length(model[0].xyz);
        if (c.w < -r) { cull(); return; }
        if (abs(c.x) > c.w + r * abs(projection[0][0]) * 1.0) { cull(); return; }
        if (abs(c.y) > c.w + r * abs(projection[1][1]) * 1.0) { cull(); return; }
    }

    // ---- Blade -------------------------------------------------------------------------------
    uint bs = hashU(cellSeed + uint(blade) * 0x9E3779B9u);
    float u0 = hash01(bs + 1u);      // position x in the cell
    float u1 = hash01(bs + 2u);      // position z
    float u2 = hash01(bs + 3u);      // keep threshold
    float u3 = hash01(bs + 4u);      // yaw
    float u4 = hash01(bs + 5u);      // height
    float u5 = hash01(bs + 6u);      // colour variation
    float u6 = hash01(bs + 7u);      // bend
    float u7 = hash01(bs + 8u);      // width
    float u8 = hash01(bs + 9u);      // dry

    vec2 lp = cellOrigin + vec2(u0, u1) * cellSize;
    float dist = length(lp - grassCamera.xz);
    if (dist > maxDist) { cull(); return; }

    // Ring ownership: cross-fade by density near the boundaries.
    float keepRing = 1.0;
    if (grassRange.y > 0.0) keepRing *= 1.0 - smoothstep(grassRange.y - grassRange.w, grassRange.y + grassRange.w, dist);
    if (grassRange.x > 0.0) keepRing *= smoothstep(grassRange.x - grassRange.z, grassRange.x + grassRange.z, dist);
    // Far fade: thin out and shrink over the last quarter.
    float farFade = 1.0 - smoothstep(0.62 * maxDist, maxDist, dist);
    float keep = keepRing * mix(1.0, farFade, 0.5);
    if (u2 >= keep * grassKeepScale) { cull(); return; }
    if (bladesPerCell <= 0) { cull(); return; }

    // Terrain data under the root.
    vec3 groundN;
    float ground = terrainHeight(lp, groundN);
    vec2 splatUv = (lp + 0.5 * grassTerrain.x) / grassTerrain.x;
    if (splatUv.x < 0.0 || splatUv.y < 0.0 || splatUv.x > 1.0 || splatUv.y > 1.0) { cull(); return; }
    vec4 sp = texture(grassSplat, splatUv);
    float cover = sp.r + 0.10 * sp.g;
    cover *= 1.0 - smoothstep(0.02, 0.35, sp.b);
    cover *= 1.0 - smoothstep(0.30, 0.70, sp.a);
    cover *= texture(grassDensityTex, splatUv).r;
    // Soft, noisy edge: the threshold wobbles per blade so the field fringes instead of cutting.
    float edge = smoothstep(0.10 + 0.25 * hash01(bs + 12u), 0.60, cover);
    if (edge <= 0.001) { cull(); return; }
    if (hash01(bs + 10u) > edge) { cull(); return; }

    // No grass in ponds or on their shore.
    for (int p = 0; p < 8; ++p) {
        if (p >= grassPondCount) break;
        vec4 pd = grassPonds[p];
        float pr = length(lp - pd.xy);
        if (pr < pd.z * 1.08 + 0.35 || (ground < pd.w + 0.06 && pr < pd.z * 1.8)) { cull(); return; }
    }

    // Large-scale variation: patches of tall / short / dry grass.
    vec2 wp = (model * vec4(lp.x, 0.0, lp.y, 1.0)).xz;
    float patch1 = valueNoise(wp * 0.22 + 17.0);
    float patch2 = valueNoise(wp * 0.9 + 53.0);
    float dryNoise = valueNoise(wp * 0.11 + 101.0);
    float dryMask = smoothstep(1.0 - grassMisc.y - 0.12, 1.0 - grassMisc.y + 0.05, dryNoise * 0.75 + patch2 * 0.25);

    float variation = grassBlade.z;
    float height = grassBlade.x * mix(1.0 - 0.55 * variation, 1.0 + 0.65 * variation, u4) *
                   mix(1.0 - 0.45 * variation, 1.0 + 0.45 * variation, patch1) *
                   mix(1.0, 0.82, dryMask * 0.5);
    // Rare tall stalks stand out above the field.
    height *= 1.0 + 0.5 * step(0.93, hash01(bs + 11u)) * variation;
    height *= smoothstep(0.0, 0.65, farFade);                          // blades sink into the ground at the far edge
    height *= 0.4 + 0.6 * smoothstep(0.0, 0.45, edge);
    height *= length(model[1].xyz);

    float widthScale = grassRing.w;
    float width = grassBlade.y * widthScale * mix(0.75, 1.3, u7) * length(model[0].xyz);
    // Taller blades are a little wider so the proportions hold.
    width *= mix(0.8, 1.15, clamp(height / max(grassBlade.x, 0.01) * 0.5, 0.0, 1.0));

    // ---- Which corner of the blade is this vertex --------------------------------------------
    int tri = vtx / 3;
    int corner = vtx - tri * 3;
    int level;
    float side;
    if (tri < 2 * (S - 1)) {
        int seg = tri / 2;
        if ((tri & 1) == 0) {
            if (corner == 0)      { level = seg;     side = -1.0; }
            else if (corner == 1) { level = seg;     side = 1.0; }
            else                  { level = seg + 1; side = -1.0; }
        } else {
            if (corner == 0)      { level = seg;     side = 1.0; }
            else if (corner == 1) { level = seg + 1; side = 1.0; }
            else                  { level = seg + 1; side = -1.0; }
        }
    } else {
        if (corner == 0)      { level = S - 1; side = -1.0; }
        else if (corner == 1) { level = S - 1; side = 1.0; }
        else                  { level = S;     side = 0.0; }
    }
    float t = float(level) / float(S);

    // ---- Shape -------------------------------------------------------------------------------
    float yaw = u3 * 6.2831853;
    // Neighbouring blades lean the same way within a patch (the field looks combed by the wind and gravity).
    float patchYaw = valueNoise(wp * 0.35 + 7.0) * 6.2831853;
    yaw = mix(yaw, patchYaw, 0.35);
    vec2 facing2 = vec2(cos(yaw), sin(yaw));
    vec3 facing = vec3(facing2.x, 0.0, facing2.y);
    vec3 across = vec3(-facing2.y, 0.0, facing2.x);

    float bend = grassBlade.w * mix(0.35, 1.25, u6);
    float lean = 0.08 + 0.30 * bend;                 // radians-ish, how far the blade tilts from vertical
    vec3 up = normalize(mix(vec3(0.0, 1.0, 0.0), groundN, 0.35));

    // Wind (world space).
    vec2 windDir2 = sceneWind.xz;
    float windLen = length(windDir2);
    windDir2 = windLen > 1e-4 ? windDir2 / windLen : vec2(0.0, 1.0);
    float windStrength = sceneWind.w * grassMisc.z;
    float travel = dot(wp, windDir2);
    float gust = valueNoise(wp * 0.07 - windDir2 * sceneTime * 0.9) * 0.6 +
                 valueNoise(wp * 0.23 - windDir2 * sceneTime * 1.7 + 31.0) * 0.4;
    float wave = sin(travel * 0.55 - sceneTime * 2.1 + u5 * 1.5) * 0.5 + 0.5;
    float sway = (0.12 + 0.55 * gust) * (0.45 + 0.55 * wave) * windStrength;
    float flutter = sin(sceneTime * (3.5 + 3.0 * u7) + u8 * 40.0 + t * 3.0) * sceneWindTurbulence * 0.045;

    // Centreline of the blade: lean + arch in `facing`, wind pushes along the wind direction.
    float tt = t * t;
    vec3 windVec = vec3(windDir2.x, 0.0, windDir2.y);
    vec3 offset = facing * (height * lean * tt * 1.6) + windVec * (height * sway * tt * 0.55) +
                  across * (height * flutter * tt);
    float rise = t * (1.0 - 0.28 * bend * t - 0.18 * sway * t);
    vec3 centre = up * (height * rise) + offset;

    // Tangent for the normal (derivative of the centreline).
    vec3 tangent = normalize(up * (1.0 - 0.56 * bend * t - 0.36 * sway * t) +
                             facing * (lean * t * 3.2) + windVec * (sway * t * 1.1));
    float profile = pow(max(1.0 - t, 0.0), 0.65);
    // Keep distant blades from dropping below a pixel.
    float dview = max(length((model * vec4(lp.x, ground, lp.y, 1.0)).xyz - viewPos), 0.1);
    float minWidth = grassMisc.w * dview * 1.1;
    float halfWidth = 0.5 * max(width * profile, side == 0.0 ? 0.0 : minWidth * mix(0.0, 1.0, profile));
    vec3 localPos = vec3(lp.x, ground - 0.03, lp.y) + centre + across * (side * halfWidth);
    // The root is slightly sunk; scale the sink away with height so tips are not pulled down.
    localPos.y += 0.03 * t;

    vec3 world = (model * vec4(localPos, 1.0)).xyz;
    vec3 faceN = normalize(cross(across, tangent));
    if (dot(faceN, facing) < 0.0) faceN = -faceN;
    // Round the ribbon: the outer edges bend away from the middle.
    vec3 shadeN = normalize(faceN + across * side * 0.55);
    FragPos = world;
    Normal = mat3(model) * shadeN;
    TexCoord = vec2(side, t);
    vGrassGround = mat3(model) * groundN;

    // ---- Colour (display space; the fragment stage linearises it) ----------------------------
    vec3 baseCol = grassBaseColor * mix(0.75, 1.3, u5);
    vec3 tipCol = grassTipColor * mix(vec3(0.82, 0.95, 0.78), vec3(1.18, 1.06, 0.72), u5);
    vec3 dryCol = grassDryColor * mix(0.8, 1.15, u7);
    float dryBlade = dryMask * smoothstep(0.1, 0.65, u8 + 0.35 * dryMask);
    tipCol = mix(tipCol, dryCol, dryBlade);
    baseCol = mix(baseCol, dryCol * 0.38, dryBlade * 0.8);
    float g = smoothstep(0.0, 0.9, t);
    vec3 col = mix(baseCol, tipCol, pow(g, 0.85));
    // A darker, richer green in the thick of the field, paler where it is short and sparse.
    col *= mix(0.74, 1.2, patch1);
    float ao = mix(0.36, 1.0, smoothstep(0.0, 0.5, t));
    // With distance the dark roots (which merge into a texture anyway) wash out and the tone settles on
    // the mid colour, so the far rings read as the same field instead of as dark spikes.
    float farTone = smoothstep(0.30 * maxDist, 0.9 * maxDist, dist);
    ao = mix(ao, 0.8, farTone * 0.75);
    col = mix(col, mix(baseCol, tipCol, 0.55), farTone * 0.55);
    // Edges of a patch fade toward the ground tone.
    ao *= mix(0.7, 1.0, smoothstep(0.1, 0.7, edge));
    vGrassColor = vec4(col, ao);

    gl_Position = projection * view * vec4(world, 1.0);
}
