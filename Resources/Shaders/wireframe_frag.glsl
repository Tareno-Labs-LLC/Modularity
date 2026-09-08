#version 330 core

out vec4 FragColor;

uniform vec4 wireColor = vec4(0.08, 0.1, 0.14, 1.0);

void main()
{
    FragColor = wireColor;
}
