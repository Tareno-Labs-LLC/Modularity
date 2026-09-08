#version 430 core
// Voxelization, stage 2 of 3: dominant-axis projection.
//
// A triangle rasterized along an axis it is nearly edge-on to covers almost no
// fragments, and every voxel it should have filled is simply missed - the wall
// ends up with holes in it and the GI leaks through. Projecting along whichever
// axis the triangle faces most directly maximises its rasterized area, which is
// what makes a single pass produce a watertight-enough volume.
layout (triangles) in;
layout (triangle_strip, max_vertices = 3) out;

in vec3 vWorldPos[];
in vec3 vNormal[];
in vec2 vUv[];

out vec3 gWorldPos;
out vec3 gNormal;
out vec2 gUv;

uniform vec3 volumeMin;
uniform float volumeExtent;

void main()
{
    vec3 edge0 = vWorldPos[1] - vWorldPos[0];
    vec3 edge1 = vWorldPos[2] - vWorldPos[0];
    vec3 faceNormal = abs(cross(edge0, edge1));

    // 0 = look down X, 1 = down Y, 2 = down Z.
    int axis = 2;
    if (faceNormal.x >= faceNormal.y && faceNormal.x >= faceNormal.z) axis = 0;
    else if (faceNormal.y >= faceNormal.z) axis = 1;

    for (int i = 0; i < 3; ++i) {
        vec3 unit = (vWorldPos[i] - volumeMin) / max(volumeExtent, 1e-6);
        vec3 ndc = unit * 2.0 - 1.0;

        vec2 xy;
        if (axis == 0)      xy = ndc.zy;
        else if (axis == 1) xy = ndc.xz;
        else                xy = ndc.xy;

        gWorldPos = vWorldPos[i];
        gNormal = vNormal[i];
        gUv = vUv[i];
        // z is pinned: depth is meaningless here, the fragment shader derives
        // its voxel from the world position instead of from the rasterizer.
        gl_Position = vec4(xy, 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
