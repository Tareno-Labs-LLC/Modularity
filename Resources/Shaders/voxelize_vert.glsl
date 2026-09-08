#version 430 core
// Voxelization, stage 1 of 3. Emits world space and lets the geometry shader do
// the projection, because which axis a triangle should be rasterized along is a
// per-triangle decision this stage cannot make.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;

uniform mat4 model;
uniform vec4 uvRect = vec4(0.0, 0.0, 1.0, 1.0);
uniform vec4 uvTransform = vec4(1.0, 1.0, 0.0, 0.0);

void main()
{
    vWorldPos = vec3(model * vec4(aPos, 1.0));
    vNormal = mat3(transpose(inverse(model))) * aNormal;
    vec2 uv = uvRect.xy + aTexCoord * uvRect.zw;
    vUv = uv * uvTransform.xy + uvTransform.zw;
    gl_Position = vec4(vWorldPos, 1.0);
}
