#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

// These match the regular material shader contract. The UI renderer supplies
// identity transforms because its temporary surface already is the image quad.
uniform mat4 model = mat4(1.0);
uniform mat4 view = mat4(1.0);
uniform mat4 projection = mat4(1.0);

void main()
{
    vec4 worldPos = model * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    Normal = mat3(model) * aNormal;
    TexCoord = aTexCoord;
    gl_Position = projection * view * worldPos;
}
