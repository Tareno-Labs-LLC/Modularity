#version 330 core

// Particle quads arrive already expanded and oriented on the CPU (RenderParticles.cpp), so
// this only projects them and forwards the frame UV and colour.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec4 aColor;

uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoord;
out vec4 Color;
out vec3 WorldPos;

void main()
{
    WorldPos = aPos;
    TexCoord = aTexCoord;
    Color = aColor;
    gl_Position = projection * view * vec4(aPos, 1.0);
}
