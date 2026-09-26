#version 430 core
// GPU-driven culling for multi-draw indirect batches (Project Settings > Optimization).
// One invocation per batched object. Writes that object's DrawArraysIndirectCommand with
// instanceCount 1 or 0, so every command keeps a fixed slot and nothing is read back:
// the draws that follow consume this buffer directly.
layout(local_size_x = 64) in;

// Per slot: [2i] = world AABB centre (xyz) + bounding-sphere radius (w),
//           [2i+1] = world AABB half extents (xyz).
layout(std430, binding = 0) readonly buffer Bounds { vec4 bounds[]; };
// Per slot: vertex count, first vertex in the mesh arena, layer bit, unused.
layout(std430, binding = 1) readonly buffer Templates { uvec4 templates[]; };
// DrawArraysIndirectCommand: count, instanceCount, first, baseInstance.
layout(std430, binding = 2) writeonly buffer Commands { uvec4 commands[]; };
layout(std430, binding = 3) buffer Counters { uint visibleCount; };

uniform uint slotCount;
uniform uint commandOffset;
uniform uint cullingMask;
uniform vec4 frustumPlanes[6];

uniform bool lodEnabled;
uniform bool lodPerspective;
uniform float lodMinScreenPx;
uniform float lodPxPerUnit;   // pixels per world unit at view depth 1 (or flat, orthographic)
uniform vec4 viewDepthRow;    // dot(viewDepthRow, vec4(p, 1)) = view-space depth of p

// Experimental Hi-Z occlusion: last frame's depth, max-reduced. Level 0 is half the
// depth buffer's resolution; hizDepthSize is the depth buffer's own size in pixels.
uniform bool hizEnabled;
uniform sampler2D hizTex;
uniform mat4 hizViewProj;
uniform vec2 hizDepthSize;
uniform float hizMaxLevel;

bool insideFrustum(vec3 center, vec3 extents)
{
    for (int p = 0; p < 6; ++p) {
        vec4 plane = frustumPlanes[p];
        float r = dot(extents, abs(plane.xyz));
        if (dot(plane.xyz, center) + plane.w + r < 0.0) {
            return false;
        }
    }
    return true;
}

bool occludedByHiZ(vec3 center, vec3 extents)
{
    vec2 uvMin = vec2(1.0);
    vec2 uvMax = vec2(0.0);
    float nearestDepth = 1.0;
    for (int k = 0; k < 8; ++k) {
        vec3 corner = center + extents * vec3((k & 1) != 0 ? 1.0 : -1.0,
                                              (k & 2) != 0 ? 1.0 : -1.0,
                                              (k & 4) != 0 ? 1.0 : -1.0);
        vec4 clip = hizViewProj * vec4(corner, 1.0);
        if (clip.w <= 1e-5) {
            return false; // crosses the near plane: cannot be tested, keep it
        }
        vec3 ndc = clip.xyz / clip.w;
        vec2 uv = ndc.xy * 0.5 + 0.5;
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        nearestDepth = min(nearestDepth, ndc.z * 0.5 + 0.5);
    }
    uvMin = clamp(uvMin, vec2(0.0), vec2(1.0));
    uvMax = clamp(uvMax, vec2(0.0), vec2(1.0));
    if (uvMax.x <= uvMin.x || uvMax.y <= uvMin.y) {
        return false; // off screen last frame; the frustum test owns this case
    }
    vec2 sizePx = (uvMax - uvMin) * hizDepthSize;
    float level = ceil(log2(max(max(sizePx.x, sizePx.y), 1.0))) - 1.0;
    level = clamp(level, 0.0, hizMaxLevel);
    float d0 = textureLod(hizTex, uvMin, level).r;
    float d1 = textureLod(hizTex, vec2(uvMax.x, uvMin.y), level).r;
    float d2 = textureLod(hizTex, vec2(uvMin.x, uvMax.y), level).r;
    float d3 = textureLod(hizTex, uvMax, level).r;
    float farthest = max(max(d0, d1), max(d2, d3));
    return nearestDepth > farthest;
}

void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= slotCount) {
        return;
    }
    uvec4 t = templates[i];
    vec4 centerRadius = bounds[2u * i];
    vec3 center = centerRadius.xyz;
    vec3 extents = bounds[2u * i + 1u].xyz;

    bool visible = (t.z & cullingMask) != 0u && insideFrustum(center, extents);
    if (visible && lodEnabled) {
        float depth = dot(viewDepthRow, vec4(center, 1.0));
        float radius = centerRadius.w;
        if (!lodPerspective) {
            visible = 2.0 * radius * lodPxPerUnit >= lodMinScreenPx;
        } else if (depth > radius) {
            visible = 2.0 * radius * lodPxPerUnit / depth >= lodMinScreenPx;
        }
    }
    if (visible && hizEnabled) {
        visible = !occludedByHiZ(center, extents);
    }
    if (visible) {
        atomicAdd(visibleCount, 1u);
    }
    commands[commandOffset + i] = uvec4(t.x, visible ? 1u : 0u, t.y, i);
}
