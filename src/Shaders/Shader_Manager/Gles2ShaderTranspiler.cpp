#include "../../../include/Shaders/Gles2ShaderTranspiler.h"

#include <regex>
#include <set>
#include <sstream>

namespace Modularity {

namespace {

bool contains(const std::string& text, const std::regex& pattern)
{
    return std::regex_search(text, pattern);
}

std::string trim(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// ES 1.00 attributes are float-only. The vertex formats that feed these shaders
// upload bone ids as integers, so a converted shader reads them back with int().
std::string floatAttributeType(const std::string& type)
{
    if (type == "int" || type == "uint") return "float";
    if (type.size() == 5 && (type[0] == 'i' || type[0] == 'u') && type.compare(1, 3, "vec") == 0) {
        return type.substr(1);
    }
    return type;
}

// Only the MODU_ES2 conditionals are evaluated: a block the shader fences off for
// ES 2 has to vanish before the illegal-construct scan, or every shader that
// already handles its own fallback would still be rejected. Every other directive
// is passed through for the driver to evaluate.
bool stripInactiveEs2Blocks(const std::string& source, std::string& out, std::string& error)
{
    struct Frame {
        bool ours = false;       // a MODU_ES2 conditional we evaluate
        bool active = true;      // this branch is kept
        bool parentActive = true;
    };
    std::vector<Frame> stack;
    const std::regex ifdefEs2(R"(^\s*#\s*ifdef\s+MODU_ES2\b)");
    const std::regex ifndefEs2(R"(^\s*#\s*ifndef\s+MODU_ES2\b)");
    const std::regex ifDefinedEs2(R"(^\s*#\s*if\s+defined\s*\(?\s*MODU_ES2\s*\)?\s*$)");
    const std::regex ifNotDefinedEs2(R"(^\s*#\s*if\s+!\s*defined\s*\(?\s*MODU_ES2\s*\)?\s*$)");
    const std::regex anyIf(R"(^\s*#\s*if)");
    const std::regex elseDirective(R"(^\s*#\s*else\b)");
    const std::regex elifDirective(R"(^\s*#\s*elif\b)");
    const std::regex endifDirective(R"(^\s*#\s*endif\b)");

    auto currentActive = [&]() { return stack.empty() || stack.back().active; };

    std::istringstream in(source);
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool parent = currentActive();
        if (std::regex_search(line, ifdefEs2) || std::regex_search(line, ifDefinedEs2)) {
            stack.push_back({true, parent, parent});
            continue;
        }
        if (std::regex_search(line, ifndefEs2) || std::regex_search(line, ifNotDefinedEs2)) {
            stack.push_back({true, false, parent});
            continue;
        }
        if (std::regex_search(line, anyIf)) {
            stack.push_back({false, parent, parent});
            if (parent) out += line + "\n";
            continue;
        }
        if (std::regex_search(line, elifDirective)) {
            if (!stack.empty() && stack.back().ours) {
                error = "line " + std::to_string(lineNumber) + ": #elif after a MODU_ES2 conditional is not supported";
                return false;
            }
            if (parent || (!stack.empty() && stack.back().parentActive)) out += line + "\n";
            continue;
        }
        if (std::regex_search(line, elseDirective)) {
            if (!stack.empty() && stack.back().ours) {
                Frame& f = stack.back();
                f.active = f.parentActive && !f.active;
                continue;
            }
            if (stack.empty() || stack.back().parentActive) out += line + "\n";
            continue;
        }
        if (std::regex_search(line, endifDirective)) {
            if (stack.empty()) {
                error = "line " + std::to_string(lineNumber) + ": #endif without #if";
                return false;
            }
            const Frame f = stack.back();
            stack.pop_back();
            if (!f.ours && f.parentActive) out += line + "\n";
            continue;
        }
        if (parent) out += line + "\n";
    }
    if (!stack.empty()) {
        error = "unterminated #if";
        return false;
    }
    return true;
}

const char* kTextureHelpers2D =
    "vec4 modu_texture(sampler2D s, vec2 uv) { return texture2D(s, uv); }\n";
const char* kTextureHelpersCube =
    "vec4 modu_texture(samplerCube s, vec3 d) { return textureCube(s, d); }\n";
const char* kTextureBiasHelpers =
    "vec4 modu_texture(sampler2D s, vec2 uv, float bias) { return texture2D(s, uv, bias); }\n";
// Without EXT_shader_texture_lod a fragment shader cannot pick a mip, so the
// explicit LOD falls back to the hardware's own selection. The engine only asks
// for LOD 0 on maps that are not mipmapped, which makes that the same answer.
const char* kTextureLodFragHelpers =
    "vec4 modu_textureLod(sampler2D s, vec2 uv, float lod) { return texture2D(s, uv); }\n";
const char* kTextureLodVertHelpers =
    "vec4 modu_textureLod(sampler2D s, vec2 uv, float lod) { return texture2DLod(s, uv, lod); }\n";
const char* kMat3Helper =
    "mat3 modu_mat3(mat4 m) { return mat3(m[0].xyz, m[1].xyz, m[2].xyz); }\n";
const char* kTransposeHelpers =
    "mat3 modu_transpose(mat3 m) {\n"
    "    return mat3(m[0][0], m[1][0], m[2][0], m[0][1], m[1][1], m[2][1], m[0][2], m[1][2], m[2][2]);\n"
    "}\n"
    "mat4 modu_transpose(mat4 m) {\n"
    "    return mat4(m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1],\n"
    "                m[0][2], m[1][2], m[2][2], m[3][2], m[0][3], m[1][3], m[2][3], m[3][3]);\n"
    "}\n";
const char* kInverseHelpers =
    "mat3 modu_inverse(mat3 m) {\n"
    "    vec3 a = m[0]; vec3 b = m[1]; vec3 c = m[2];\n"
    "    vec3 r0 = cross(b, c); vec3 r1 = cross(c, a); vec3 r2 = cross(a, b);\n"
    "    float invDet = 1.0 / dot(r2, c);\n"
    "    return mat3(r0.x, r1.x, r2.x, r0.y, r1.y, r2.y, r0.z, r1.z, r2.z) * invDet;\n"
    "}\n";
// The idiom every lit vertex shader uses. Dropping the translation first keeps it
// a 3x3 inverse, which is the expensive part on a GPU this small.
const char* kNormalMatrixHelper =
    "mat3 modu_normalMatrix(mat4 m) {\n"
    "    mat3 u = mat3(m[0].xyz, m[1].xyz, m[2].xyz);\n"
    "    vec3 r0 = cross(u[1], u[2]); vec3 r1 = cross(u[2], u[0]); vec3 r2 = cross(u[0], u[1]);\n"
    "    float invDet = 1.0 / dot(r2, u[2]);\n"
    "    return mat3(r0, r1, r2) * invDet;\n"
    "}\n";

} // namespace

std::string Gles2ShaderSiblingPath(const std::string& shaderPath)
{
    const size_t slash = shaderPath.find_last_of("/\\");
    const size_t dot = shaderPath.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return shaderPath + ".es2";
    }
    return shaderPath.substr(0, dot) + ".es2" + shaderPath.substr(dot);
}

Gles2TranspileResult TranspileShaderToGles2(const std::string& source, Gles2ShaderStage stage)
{
    Gles2TranspileResult result;
    const bool vertex = stage == Gles2ShaderStage::Vertex;

    // #version goes first; the header below replaces it.
    std::string withoutVersion;
    {
        std::istringstream in(source);
        std::string line;
        while (std::getline(in, line)) {
            if (std::regex_search(line, std::regex(R"(^\s*#\s*version\b)"))) continue;
            withoutVersion += line + "\n";
        }
    }

    std::string body;
    if (!stripInactiveEs2Blocks(withoutVersion, body, result.error)) {
        return result;
    }

    // Global declarations, one line at a time.
    const std::regex ioDecl(
        R"(^(\s*)(?:layout\s*\(([^)]*)\)\s*)?(?:(?:flat|smooth|noperspective|centroid)\s+)*(in|out)\s+(\w+)\s+(\w+)\s*(\[[^\]]*\])?\s*;(.*)$)");
    const std::regex uniformInit(
        R"(^(\s*)uniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(\[[^\]]+\])?\s*=\s*([^;]+);(.*)$)");
    const std::regex locationValue(R"(location\s*=\s*(\d+))");
    const std::regex mat4Decl(R"(\bmat4\s+(\w+))");

    std::set<std::string> mat4Names;
    std::string fragOutName;
    std::string converted;
    {
        std::istringstream in(body);
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_match(line, m, ioDecl)) {
                const std::string indent = m[1];
                const std::string layout = m[2];
                const std::string qualifier = m[3];
                std::string type = m[4];
                const std::string name = m[5];
                const std::string array = m[6];
                const std::string tail = m[7];
                if (vertex && qualifier == "in") {
                    type = floatAttributeType(type);
                    std::smatch loc;
                    if (std::regex_search(layout, loc, locationValue)) {
                        result.attributes.push_back({name, std::stoi(loc[1])});
                    } else {
                        result.attributes.push_back({name, static_cast<int>(result.attributes.size())});
                    }
                    converted += indent + "attribute " + type + " " + name + array + ";" + tail + "\n";
                } else if (!vertex && qualifier == "out") {
                    if (!fragOutName.empty()) {
                        result.error = "multiple fragment outputs ('" + fragOutName + "', '" + name +
                                       "'): GLES 2.0 has a single gl_FragColor";
                        return result;
                    }
                    fragOutName = name;
                } else {
                    if (type.find("int") != std::string::npos || type.find("uvec") != std::string::npos ||
                        type.find("ivec") != std::string::npos) {
                        result.error = "varying '" + name + "' is an integer type, which GLES 2.0 does not allow";
                        return result;
                    }
                    converted += indent + "varying " + type + " " + name + array + ";" + tail + "\n";
                }
                continue;
            }
            if (std::regex_match(line, m, uniformInit)) {
                result.uniformDefaults.push_back({m[2], m[3], trim(m[5])});
                converted += std::string(m[1]) + "uniform " + std::string(m[2]) + " " + std::string(m[3]) +
                             std::string(m[4]) + ";" + std::string(m[6]) + "\n";
                continue;
            }
            for (std::sregex_iterator it(line.begin(), line.end(), mat4Decl), end; it != end; ++it) {
                mat4Names.insert((*it)[1]);
            }
            converted += line + "\n";
        }
    }

    // Constructs with no ES 2.0 spelling. A shader that needs one fences it off
    // with #ifndef MODU_ES2; reaching here means it did not.
    struct Illegal { const char* pattern; const char* what; };
    const Illegal illegal[] = {
        {R"(\bgl_VertexID\b)", "gl_VertexID"},
        {R"(\bgl_InstanceID\b)", "gl_InstanceID"},
        {R"(\btexelFetch\b)", "texelFetch"},
        {R"(\btextureSize\b)", "textureSize"},
        {R"(\btextureGrad\b)", "textureGrad"},
        {R"(\btextureOffset\b)", "textureOffset"},
        {R"(\bsampler3D\b)", "sampler3D (no 3D textures)"},
        {R"(\bsampler2DShadow\b)", "sampler2DShadow"},
        {R"(\bsampler2DArray\b)", "sampler2DArray"},
        {R"(\b[iu]sampler\w*)", "integer samplers"},
        {R"(\buint\b|\buvec[234]\b)", "unsigned integers"},
        {R"(<<|>>|[^&]&[^&=]|\^|~)", "bitwise operators"},
        {R"(\buniform\s+\w+\s*\{)", "uniform blocks"},
        {R"(\b(?:vec[234]|float|int|mat[234])\s*\[\s*\d*\s*\]\s*\()", "array constructors"},
        {R"(\bswitch\s*\()", "switch statements"},
        {R"(\bdo\s*\{)", "do/while loops"},
        {R"(\bwhile\s*\()", "while loops"},
        {R"(\bgl_FragDepth\b)", "gl_FragDepth"},
    };
    // Comments are stripped for the scan only, so prose mentioning a forbidden name
    // ("no gl_VertexID here") does not fail the shader.
    const std::string scanText = std::regex_replace(converted, std::regex(R"(//[^\n]*)"), "");
    for (const Illegal& rule : illegal) {
        if (contains(scanText, std::regex(rule.pattern))) {
            result.error = std::string("uses ") + rule.what + ", which GLES 2.0 does not support; "
                           "fence it with #ifndef MODU_ES2";
            return result;
        }
    }
    if (contains(scanText, std::regex(R"([^%]%[^%])"))) {
        result.error = "uses the % operator, which is reserved in GLSL ES 1.00 (use mod())";
        return result;
    }

    // Spelling changes.
    converted = std::regex_replace(converted,
        std::regex(R"(\bmat3\s*\(\s*transpose\s*\(\s*inverse\s*\(\s*(\w+)\s*\)\s*\)\s*\))"),
        "modu_normalMatrix($1)");
    const bool usesNormalMatrix = converted.find("modu_normalMatrix(") != std::string::npos;
    // mat3(<a mat4>) is a matrix-from-matrix constructor, which ES 1.00 lacks.
    for (const std::string& name : mat4Names) {
        converted = std::regex_replace(converted,
            std::regex(R"(\bmat3\s*\(\s*)" + name + R"(\s*\))"), "modu_mat3(" + name + ")");
    }
    const bool usesMat3 = converted.find("modu_mat3(") != std::string::npos;
    const bool usesTranspose = contains(converted, std::regex(R"(\btranspose\s*\()"));
    const bool usesInverse = contains(converted, std::regex(R"(\binverse\s*\()"));
    converted = std::regex_replace(converted, std::regex(R"(\btranspose\s*\()"), "modu_transpose(");
    converted = std::regex_replace(converted, std::regex(R"(\binverse\s*\()"), "modu_inverse(");
    const bool usesTextureLod = contains(converted, std::regex(R"(\btextureLod\s*\()"));
    const bool usesTexture = contains(converted, std::regex(R"(\btexture\s*\()"));
    converted = std::regex_replace(converted, std::regex(R"(\btextureLod\s*\()"), "modu_textureLod(");
    converted = std::regex_replace(converted, std::regex(R"(\btexture\s*\()"), "modu_texture(");
    if (!fragOutName.empty()) {
        converted = std::regex_replace(converted, std::regex("\\b" + fragOutName + "\\b"), "gl_FragColor");
    }
    const bool usesCube = contains(converted, std::regex(R"(\bsamplerCube\b)"));
    const bool usesDerivatives = contains(converted, std::regex(R"(\b(?:dFdx|dFdy|fwidth)\s*\()"));

    std::string header = "#version 100\n";
    // Metadata sits straight under #version so ParseGles2ShaderMetadata can stop at
    // the first line that is not one of its comments.
    for (const Gles2AttributeBinding& a : result.attributes) {
        header += "// @modu-attrib " + a.name + " " + std::to_string(a.location) + "\n";
    }
    for (const Gles2UniformDefault& d : result.uniformDefaults) {
        header += "// @modu-default " + d.type + " " + d.name + " = " + d.value + "\n";
    }
    if (usesDerivatives && !vertex) {
        header += "#extension GL_OES_standard_derivatives : enable\n";
    }
    header += "#define MODU_ES2 1\n";
    if (vertex) {
        header += "precision highp float;\n";
    } else {
        header += "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
                  "precision highp float;\n"
                  "#else\n"
                  "precision mediump float;\n"
                  "#endif\n";
    }
    if (usesTexture || usesTextureLod) header += kTextureHelpers2D;
    if (usesTexture && usesCube) header += kTextureHelpersCube;
    if (usesTexture && !vertex) header += kTextureBiasHelpers;
    if (usesTextureLod) header += vertex ? kTextureLodVertHelpers : kTextureLodFragHelpers;
    if (usesMat3) header += kMat3Helper;
    if (usesTranspose) header += kTransposeHelpers;
    if (usesInverse) header += kInverseHelpers;
    if (usesNormalMatrix) header += kNormalMatrixHelper;

    result.source = header + converted;
    result.success = true;
    return result;
}

void ParseGles2ShaderMetadata(const std::string& convertedSource,
                              std::vector<Gles2AttributeBinding>& attributes,
                              std::vector<Gles2UniformDefault>& uniformDefaults)
{
    const std::regex attrib(R"(^//\s*@modu-attrib\s+(\w+)\s+(\d+)\s*$)");
    const std::regex def(R"(^//\s*@modu-default\s+(\w+)\s+(\w+)\s*=\s*(.+?)\s*$)");
    std::istringstream in(convertedSource);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, attrib)) {
            attributes.push_back({m[1], std::stoi(m[2])});
        } else if (std::regex_match(line, m, def)) {
            uniformDefaults.push_back({m[1], m[2], m[3]});
        } else if (line.rfind("//", 0) != 0 && line.rfind("#", 0) != 0 && !line.empty()) {
            // Metadata only lives in the header block.
            break;
        }
    }
}

} // namespace Modularity
