#ifndef SHADER_H
#define SHADER_H

#include <string>
#include <unordered_map>
#include "../../ThirdParty/glm/glm.hpp"

class Shader
{
public:
    unsigned int ID;

    Shader(const char* vertexPath, const char* fragmentPath);
    // With a geometry stage. Added for voxelization, where the axis a triangle
    // should be rasterized along is a per-triangle decision no other stage can
    // make. A null or empty geometryPath behaves exactly like the two-argument
    // form, so this is safe to route existing callers through.
    Shader(const char* vertexPath, const char* geometryPath, const char* fragmentPath);

    void use();

    void setBool(const std::string &name, bool value) const;
    void setInt(const std::string &name, int value) const;
    void setFloat(const std::string &name, float value) const;
    void setVec2(const std::string &name, const glm::vec2 &value) const;
    void setVec3(const std::string &name, const glm::vec3 &value) const;
    void setVec4(const std::string &name, const glm::vec4 &value) const;
    void setMat3(const std::string &name, const glm::mat3 &mat) const;
    void setMat4(const std::string &name, const glm::mat4 &mat) const;
    void setMat4Array(const std::string &name, const glm::mat4 *data, int count) const;

private:
    int getUniformLocation(const std::string& name) const;
    std::string readShaderFile(const char* filePath);
    void compileShaders(const char* vertexSource, const char* fragmentSource,
                        const char* geometrySource = nullptr);
    void checkCompileErrors(unsigned int shader, std::string type);
    mutable std::unordered_map<std::string, int> uniformLocationCache;
};

#endif
