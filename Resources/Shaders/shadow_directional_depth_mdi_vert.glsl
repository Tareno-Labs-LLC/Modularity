#version 330 core
layout (location = 0) in vec3 aPos;
// Multi-draw indirect shadow casters: each command's baseInstance selects the caster's
// model matrix. Must stay identical to shadow_directional_depth_vert.glsl otherwise.
layout (location = 5) in mat4 aInstanceModel;

uniform mat4 lightSpaceMatrix;

void main()
{
    gl_Position = lightSpaceMatrix * aInstanceModel * vec4(aPos, 1.0);
}
