#version 430 core
// Builds one level of the Hi-Z depth pyramid (experimental occlusion culling).
// Every texel holds the FARTHEST depth of the source texels under it, so an object whose
// nearest point lies beyond that value is hidden. Level 0 reduces the scene depth buffer
// to half size; later levels reduce the previous level.
//
// Levels have floor-halved sizes, so at a non-power-of-two resolution a texel does not
// sit over exactly 2x2 source texels. The culling shader looks texels up by UV, so each
// texel here takes every source texel its UV footprint overlaps (two or three per axis).
// A plain 2x2 reduction would leave gaps (at 333 pixels wide, depth pixel 200 maps to
// level-0 texel 99, which a 2x2 reduction builds from pixels 198-199 only), and an
// object could be hidden by depth that does not cover it.
layout(local_size_x = 8, local_size_y = 8) in;

uniform bool fromDepth;
uniform sampler2D depthTex;                                    // fromDepth: the scene depth
layout(r32f, binding = 0) readonly uniform image2D srcLevel;   // !fromDepth
layout(r32f, binding = 1) writeonly uniform image2D dstLevel;
uniform ivec2 srcSize;
uniform ivec2 dstSize;

float fetchSource(ivec2 p)
{
    return fromDepth ? texelFetch(depthTex, p, 0).r : imageLoad(srcLevel, p).r;
}

void main()
{
    ivec2 dst = ivec2(gl_GlobalInvocationID.xy);
    if (dst.x >= dstSize.x || dst.y >= dstSize.y) {
        return;
    }
    // Source texels overlapping [dst, dst + 1) / dstSize in UV: floor to ceil.
    ivec2 first = (dst * srcSize) / dstSize;
    ivec2 last = min(((dst + 1) * srcSize + dstSize - 1) / dstSize - 1, srcSize - 1);
    float farthest = 0.0;
    for (int y = first.y; y <= last.y; ++y) {
        for (int x = first.x; x <= last.x; ++x) {
            farthest = max(farthest, fetchSource(ivec2(x, y)));
        }
    }
    imageStore(dstLevel, dst, vec4(farthest));
}
