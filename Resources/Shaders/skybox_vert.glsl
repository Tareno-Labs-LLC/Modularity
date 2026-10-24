#version 330 core
layout (location = 0) in vec3 aPos;

out vec3 fragPos;

uniform mat4 projection;
uniform mat4 view;

void main()
{
    vec2 pos;
#ifdef MODU_ES2
    // No gl_VertexID in GLES 2: the ES 2 pipeline feeds the same oversized
    // triangle, (-1,-1) (3,-1) (-1,3), through attribute 0 instead.
    pos = aPos.xy;
#else
    if (gl_VertexID == 0) {
        pos = vec2(-1.0, -1.0);
    } else if (gl_VertexID == 1) {
        pos = vec2(3.0, -1.0);
    } else {
        pos = vec2(-1.0, 3.0);
    }
#endif
    fragPos = vec3(pos, 1.0);
    gl_Position = vec4(pos, 1.0, 1.0);
}
