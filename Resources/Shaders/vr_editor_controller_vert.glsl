#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
uniform mat4 model;
uniform mat4 vp;
out vec3 position;
out vec3 normal;
out vec2 uv;
void main() {
    vec4 world=model*vec4(aPos,1.0);
    position=world.xyz;
    normal=transpose(inverse(mat3(model)))*aNormal;
    uv=aUV;
    gl_Position=vp*world;
}
