#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;
// Multi-draw indirect batches: each command's baseInstance selects this object's model
// matrix from an instanced (divisor 1) attribute, so the stage needs no gl_DrawID and
// runs on any GL 4.3 context. Everything below must stay identical to vert.glsl, which
// draws the same objects when the batch path is off - change both together.
layout (location = 5) in mat4 aInstanceModel;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

uniform mat4 view;
uniform mat4 projection;
uniform vec4 uvRect;

void main()
{
    FragPos = vec3(aInstanceModel * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(aInstanceModel))) * aNormal;
    TexCoord = uvRect.xy + aTexCoord * uvRect.zw;

    gl_Position = projection * view * vec4(FragPos, 1.0);
}
