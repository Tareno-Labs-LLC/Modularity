#version 330 core

// Pond water (RenderGrass.cpp): one quad per pond, generated from gl_InstanceID / gl_VertexID with no
// vertex data. The quad only bounds the pond; the fragment stage (frag.glsl compiled with MODU_WATER)
// discards every texel where the terrain stands above the water, so the visible shape is the carved basin.

uniform mat4 model;                 // terrain world matrix
uniform mat4 view;
uniform mat4 projection;
uniform vec4 waterPonds[16];        // local x, local z, radius, local water Y
uniform float waterExtent;          // quad half size as a multiple of the radius

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

void main()
{
    vec4 pond = waterPonds[gl_InstanceID];
    int corner = gl_VertexID % 6;
    // Two triangles over corners {-1,1}^2: (0,1,2) (2,1,3).
    int ci = corner == 0 ? 0 : (corner == 1 || corner == 4) ? 1 : (corner == 2 || corner == 3) ? 2 : 3;
    float cx = (ci == 1 || ci == 3) ? 1.0 : -1.0;
    float cz = (ci >= 2) ? 1.0 : -1.0;
    vec3 local = vec3(pond.x + cx * pond.z * waterExtent, pond.w, pond.y + cz * pond.z * waterExtent);
    vec4 world = model * vec4(local, 1.0);
    FragPos = world.xyz;
    Normal = normalize(mat3(model) * vec3(0.0, 1.0, 0.0));
    TexCoord = vec2(cx, cz) * 0.5 + 0.5;
    gl_Position = projection * view * world;
}
