#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aTexCoord;
out vec2 TexCoord;

uniform mat4 model;
uniform mat4 lightSpaceMatrix;
// Sprite sheet rectangle, as in vert.glsl, so a cut-out sprite casts the same shape
// it shows. Identity unless the renderer sets it for the caster.
uniform vec4 uvRect = vec4(0.0, 0.0, 1.0, 1.0);

out vec3 WorldPos;

void main()
{
    TexCoord = uvRect.xy + aTexCoord * uvRect.zw;
    vec4 world = model * vec4(aPos, 1.0);
    WorldPos = world.xyz;
    gl_Position = lightSpaceMatrix * world;
}
