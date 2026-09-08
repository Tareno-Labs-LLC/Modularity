#version 330 core
out vec4 FragColor;

in vec2 TexCoord;

// Straight copy of an already-post-processed, premultiplied-alpha band layer.
// The caller composites with glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA), so
// nothing here may touch the premultiplication.
uniform sampler2D sourceTex;
uniform float opacity = 1.0;

void main() {
    FragColor = texture(sourceTex, TexCoord) * clamp(opacity, 0.0, 1.0);
}
