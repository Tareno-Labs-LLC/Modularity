#include "../../../include/Shaders/Shader.h"
#include "../../../include/Graphics/OpenGL.h"
#include "../../../include/Platform/AssetSource.h"
#include "../../ThirdParty/glm/glm.hpp"
#include "../../ThirdParty/glm/gtc/matrix_transform.hpp"
#include "../../ThirdParty/glm/gtc/type_ptr.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <utility>

namespace
{
    std::string trimShaderDirective(std::string value)
    {
        const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) {
            return !isSpace(ch);
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) {
            return !isSpace(ch);
        }).base(), value.end());
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    // "path?DEF1,DEF2": a shader path may carry preprocessor defines after a '?'
    // (illegal in Windows file names, so unambiguous). The file is read from the
    // stripped path and each define is inserted as "#define NAME 1" right after
    // the #version line, so one source file can compile into several programs.
    std::string stripShaderPathDefines(const char* path, std::string* definesOut = nullptr)
    {
        if (definesOut) definesOut->clear();
        if (!path) return std::string();
        std::string full(path);
        const size_t q = full.find('?');
        if (q == std::string::npos) return full;
        if (definesOut) *definesOut = full.substr(q + 1);
        return full.substr(0, q);
    }

    std::string injectShaderDefines(const std::string& source, const std::string& defines)
    {
        if (defines.empty()) return source;
        std::string block;
        size_t start = 0;
        while (start <= defines.size())
        {
            size_t end = defines.find(',', start);
            if (end == std::string::npos) end = defines.size();
            std::string name = defines.substr(start, end - start);
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            }), name.end());
            if (!name.empty()) block += "#define " + name + " 1\n";
            start = end + 1;
        }
        if (block.empty()) return source;

        // Directly after the #version line (a #version must be the first
        // directive in the file, so it can't follow our defines).
        size_t pos = 0;
        while (pos < source.size())
        {
            size_t lineEnd = source.find('\n', pos);
            const size_t next = lineEnd == std::string::npos ? source.size() : lineEnd + 1;
            size_t first = pos;
            while (first < next && (source[first] == ' ' || source[first] == '\t')) ++first;
            if (source.compare(first, 8, "#version") == 0)
            {
                std::string result = source;
                if (lineEnd == std::string::npos) result += '\n';
                result.insert(lineEnd == std::string::npos ? result.size() : next, block);
                return result;
            }
            pos = next;
        }
        return block + source;
    }

    bool splitCombinedShaderSource(const std::string& source, std::string& outVertex, std::string& outFragment)
    {
        enum class Stage {
            None,
            Vertex,
            Fragment
        };

        Stage stage = Stage::None;
        std::stringstream stream(source);
        std::string line;
        while (std::getline(stream, line))
        {
            const std::string directive = trimShaderDirective(line);
            if (directive.rfind("#shader", 0) == 0)
            {
                if (directive.find("vertex") != std::string::npos || directive.find("vert") != std::string::npos)
                {
                    stage = Stage::Vertex;
                }
                else if (directive.find("fragment") != std::string::npos || directive.find("frag") != std::string::npos)
                {
                    stage = Stage::Fragment;
                }
                else
                {
                    stage = Stage::None;
                }
                continue;
            }

            if (stage == Stage::Vertex)
            {
                outVertex += line;
                outVertex += '\n';
            }
            else if (stage == Stage::Fragment)
            {
                outFragment += line;
                outFragment += '\n';
            }
        }

        return !outVertex.empty() && !outFragment.empty();
    }

#if MODULARITY_OPENGL_ES
    void replaceAll(std::string& text, const std::string& from, const std::string& to)
    {
        if (from.empty()) return;

        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
#endif

    // Keep the ordinary material contract, including project-owned shaders. The
    // variant changes camera uniforms, never object/instance transforms or wind.
    std::string prepareMultiviewSource(std::string source, bool vertex)
    {
        if (source.find("#define MODU_MULTIVIEW") == std::string::npos) return source;
        const size_t headerEnd = source.find('\n');
        const std::string declarations = vertex
            ? "#extension GL_OVR_multiview2 : require\nlayout(num_views = 2) in;\nflat out highp int moduEyeIndex;\n"
            : "#extension GL_OVR_multiview2 : require\nflat in highp int moduEyeIndex;\n";
        source.insert(headerEnd == std::string::npos ? 0 : headerEnd + 1, declarations);
        const char* index = vertex ? "int(gl_ViewID_OVR)" : "moduEyeIndex";
        for (const auto& uniform : {std::pair{"mat4", "view"}, {"mat4", "projection"}, {"vec3", "viewPos"}}) {
            const std::string name = uniform.second;
            const std::string array = "moduStereo_" + name;
            const std::regex declaration("uniform\\s+(?:highp\\s+|mediump\\s+|lowp\\s+)?" +
                                         std::string(uniform.first) + "\\s+" + name + "\\s*(?:=\\s*[^;]+)?;");
            source = std::regex_replace(source, declaration,
                "uniform " + std::string(uniform.first) + " " + array + "[2];\n#define " + name + " " + array + "[" + index + "]\n");
        }
        if (vertex) {
            const std::regex mainEntry(R"((void\s+main\s*\(\s*(?:void\s*)?\)\s*\{))");
            source = std::regex_replace(source, mainEntry, "$1\nmoduEyeIndex = int(gl_ViewID_OVR);\n");
        }
        return source;
    }

    std::string prepareShaderSourceForBackend(const std::string& source)
    {
#if MODULARITY_OPENGL_ES
        std::string result = source;
        // GLES does not define a default precision for sampler3D. Mali rejects
        // the default material's voxelVolume even when GI is disabled at runtime.
        const std::string esHeader = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler3D;\nprecision highp sampler2DArray;\n";

        if (result.rfind("#version", 0) == 0)
        {
            size_t lineEnd = result.find('\n');
            if (lineEnd == std::string::npos)
            {
                result = esHeader;
            }
            else
            {
                result.replace(0, lineEnd + 1, esHeader);
            }
        }
        else
        {
            result.insert(0, esHeader);
        }

        replaceAll(result, "noperspective ", "");
        const std::regex uniformInitializer(
            R"((uniform\s+(?:bool|int|float|vec2|vec3|vec4|mat3|mat4|sampler2D|samplerCube)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[[^\]]+\])?)\s*=\s*[^;]+;)");
        return std::regex_replace(result, uniformInitializer, "$1;");
#else
        return source;
#endif
    }
}

Shader::Shader(const char* vertexPath, const char* fragmentPath)
{
    const bool sameShaderFile = vertexPath != nullptr && fragmentPath != nullptr &&
                                std::string(vertexPath) == std::string(fragmentPath);
    std::string vertexCode;
    std::string fragmentCode;
    std::string vertexDefines;
    std::string fragmentDefines;
    stripShaderPathDefines(vertexPath, &vertexDefines);
    stripShaderPathDefines(fragmentPath, &fragmentDefines);
    if (sameShaderFile)
    {
        const std::string combinedCode = readShaderFile(vertexPath);
        if (!splitCombinedShaderSource(combinedCode, vertexCode, fragmentCode))
        {
            vertexCode = combinedCode;
            fragmentCode = combinedCode;
        }
    }
    else
    {
        vertexCode = readShaderFile(vertexPath);
        fragmentCode = readShaderFile(fragmentPath);
    }
    vertexCode = injectShaderDefines(vertexCode, vertexDefines);
    fragmentCode = injectShaderDefines(fragmentCode, fragmentDefines);

    compileShaders(vertexCode.c_str(), fragmentCode.c_str());
}

Shader::Shader(const char* vertexPath, const char* geometryPath, const char* fragmentPath)
{
    const bool hasGeometry = geometryPath != nullptr && geometryPath[0] != '\0';
    std::string vertexDefines;
    std::string fragmentDefines;
    std::string geometryDefines;
    stripShaderPathDefines(vertexPath, &vertexDefines);
    stripShaderPathDefines(fragmentPath, &fragmentDefines);
    if (hasGeometry) stripShaderPathDefines(geometryPath, &geometryDefines);
    const std::string vertexCode = injectShaderDefines(readShaderFile(vertexPath), vertexDefines);
    const std::string fragmentCode = injectShaderDefines(readShaderFile(fragmentPath), fragmentDefines);
    const std::string geometryCode = hasGeometry
        ? injectShaderDefines(readShaderFile(geometryPath), geometryDefines) : std::string();
    compileShaders(vertexCode.c_str(), fragmentCode.c_str(),
                   hasGeometry ? geometryCode.c_str() : nullptr);
}

std::string Shader::readShaderFile(const char* filePath)
{
    if (!filePath) return "";
    // Drop any "?DEFINES" suffix: the file on disk is the stripped path.
    const std::string strippedPath = stripShaderPathDefines(filePath);
    filePath = strippedPath.c_str();

    std::vector<uint8_t> bytes = Modularity::Platform::GetAssetSource().ReadAll(filePath);
    if (!bytes.empty()) {
        return std::string(bytes.begin(), bytes.end());
    }

    std::ifstream shaderFile(filePath, std::ios::binary);
    if (shaderFile) {
        std::stringstream shaderStream;
        shaderStream << shaderFile.rdbuf();
        return shaderStream.str();
    }
    std::cerr << "ERROR: Shader file not found: " << filePath << std::endl;
    return "";
}

void Shader::compileShaders(const char* vertexSource, const char* fragmentSource,
                            const char* geometrySource)
{
    std::string preparedVertexSource = prepareShaderSourceForBackend(vertexSource ? vertexSource : "");
    std::string preparedFragmentSource = prepareShaderSourceForBackend(fragmentSource ? fragmentSource : "");
    preparedVertexSource = prepareMultiviewSource(std::move(preparedVertexSource), true);
    preparedFragmentSource = prepareMultiviewSource(std::move(preparedFragmentSource), false);
    const char* preparedVertexCStr = preparedVertexSource.c_str();
    const char* preparedFragmentCStr = preparedFragmentSource.c_str();

#if MODULARITY_OPENGL_ES
    if (geometrySource != nullptr && geometrySource[0] != '\0') {
        std::cerr << "Geometry shaders are not available on the OpenGL ES backend\n";
        ID = 0;
        return;
    }
#endif

    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &preparedVertexCStr, NULL);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &preparedFragmentCStr, NULL);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

#if !MODULARITY_OPENGL_ES
    unsigned int geometry = 0;
    std::string preparedGeometrySource;
    if (geometrySource != nullptr && geometrySource[0] != '\0') {
        preparedGeometrySource = prepareShaderSourceForBackend(geometrySource);
        const char* preparedGeometryCStr = preparedGeometrySource.c_str();
        geometry = glCreateShader(GL_GEOMETRY_SHADER);
        glShaderSource(geometry, 1, &preparedGeometryCStr, NULL);
        glCompileShader(geometry);
        checkCompileErrors(geometry, "GEOMETRY");
    }
#endif

    auto releaseStages = [&]() {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
#if !MODULARITY_OPENGL_ES
        if (geometry != 0) glDeleteShader(geometry);
#endif
    };

    int success = 0;
    glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
    if (!success) {
        releaseStages();
        ID = 0;
        return;
    }
    glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
    if (!success) {
        releaseStages();
        ID = 0;
        return;
    }
#if !MODULARITY_OPENGL_ES
    if (geometry != 0) {
        glGetShaderiv(geometry, GL_COMPILE_STATUS, &success);
        if (!success) {
            releaseStages();
            ID = 0;
            return;
        }
    }
#endif

    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, fragment);
#if !MODULARITY_OPENGL_ES
    if (geometry != 0) glAttachShader(ID, geometry);
#endif
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glGetProgramiv(ID, GL_LINK_STATUS, &success);
    if (!success) {
        glDeleteProgram(ID);
        ID = 0;
    }

    releaseStages();
}

void Shader::use()
{
    glUseProgram(ID);
}

int Shader::getUniformLocation(const std::string& name) const
{
    auto it = uniformLocationCache.find(name);
    if (it != uniformLocationCache.end()) {
        return it->second;
    }
    int location = glGetUniformLocation(ID, name.c_str());
    uniformLocationCache.emplace(name, location);
    return location;
}

void Shader::checkCompileErrors(unsigned int shader, std::string type)
{
    int success;
    char infoLog[1024];

    if (type != "PROGRAM")
    {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(shader, 1024, NULL, infoLog);
            std::cerr << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n" << infoLog << std::endl;
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shader, 1024, NULL, infoLog);
            std::cerr << "ERROR::PROGRAM_LINKING_ERROR of type: " << type << "\n" << infoLog << std::endl;
        }
    }
}

void Shader::setBool(const std::string &name, bool value) const
{
    glUniform1i(getUniformLocation(name), (int)value);
}

void Shader::setInt(const std::string &name, int value) const
{
    glUniform1i(getUniformLocation(name), value);
}

void Shader::setFloat(const std::string &name, float value) const
{
    glUniform1f(getUniformLocation(name), value);
}

void Shader::setVec2(const std::string &name, const glm::vec2 &value) const
{
    glUniform2fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec3(const std::string &name, const glm::vec3 &value) const
{
    glUniform3fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec4(const std::string &name, const glm::vec4 &value) const
{
    glUniform4fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setMat3(const std::string &name, const glm::mat3 &mat) const
{
    glUniformMatrix3fv(getUniformLocation(name), 1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::setMat4(const std::string &name, const glm::mat4 &mat) const
{
    glUniformMatrix4fv(getUniformLocation(name), 1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::setMat4Array(const std::string &name, const glm::mat4 *data, int count) const
{
    if (count <= 0 || !data) return;
    glUniformMatrix4fv(getUniformLocation(name), count, GL_FALSE, glm::value_ptr(data[0]));
}
