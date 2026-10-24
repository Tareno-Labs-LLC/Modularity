#pragma once

#include <string>
#include <vector>

// Rewrites an engine shader (GLSL 330 core / ESSL 300 style) into GLSL ES 1.00 for
// GLES 2.0-only GPUs like the PowerVR SGX544 in the original Anbernic RG35XX.
//
// It is a syntax pass, not a compiler: it maps the 3.x spellings onto their 1.00
// equivalents (in/out -> attribute/varying, FragColor -> gl_FragColor, texture() ->
// texture2D/textureCube, transpose/inverse -> helpers) and defines MODU_ES2 so the
// shader itself can drop what ES 2.0 has no answer for (shadow maps, voxel GI,
// skinning with 128 bones). Anything still illegal after that is reported by the
// driver's compiler; validate exports with glslang's ES 100 profile.
namespace Modularity {

enum class Gles2ShaderStage {
    Vertex,
    Fragment
};

struct Gles2AttributeBinding {
    std::string name;
    int location = 0;
};

struct Gles2UniformDefault {
    std::string type;
    std::string name;
    std::string value;  // the GLSL initializer expression, verbatim
};

struct Gles2TranspileResult {
    bool success = false;
    std::string source;
    std::string error;
    // layout(location = N) is ES 3 only, so the loader has to call
    // glBindAttribLocation with these before linking.
    std::vector<Gles2AttributeBinding> attributes;
    // ES 1.00 has no uniform initializers. These are what the shader declared, so the
    // loader can set them once after linking instead of every one reading 0.
    std::vector<Gles2UniformDefault> uniformDefaults;
};

Gles2TranspileResult TranspileShaderToGles2(const std::string& source, Gles2ShaderStage stage);

// Converted shaders ship next to the original as <name>.es2.glsl.
std::string Gles2ShaderSiblingPath(const std::string& shaderPath);

// The converted file carries its attribute bindings and uniform defaults as
// "// @modu-attrib" and "// @modu-default" comment lines, so a loader that only
// has the file can recover them.
void ParseGles2ShaderMetadata(const std::string& convertedSource,
                              std::vector<Gles2AttributeBinding>& attributes,
                              std::vector<Gles2UniformDefault>& uniformDefaults);

} // namespace Modularity
