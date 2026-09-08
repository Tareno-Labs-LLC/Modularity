#pragma once

#include "Common.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

struct ShaderPackAssetData {
    std::string vertexShaderPath;
    std::string fragmentShaderPath;
};

inline std::string MaterialAssetTrim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

inline std::string MaterialAssetLowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline bool IsShaderPackFile(const fs::path& path) {
    return MaterialAssetLowercase(path.extension().string()) == ".modushader";
}

inline bool IsShaderSourceFile(const fs::path& path) {
    const std::string ext = MaterialAssetLowercase(path.extension().string());
    return ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".shader";
}

inline bool IsMaterialShaderAssetFile(const fs::path& path) {
    return IsShaderPackFile(path) || IsShaderSourceFile(path);
}

inline fs::path ResolveShaderPackReferencedPath(const fs::path& packPath, const std::string& rawValue) {
    if (rawValue.empty()) {
        return {};
    }

    fs::path referenced(rawValue);
    if (referenced.is_absolute()) {
        return referenced;
    }

    if (!packPath.empty()) {
        return (packPath.parent_path() / referenced).lexically_normal();
    }

    return referenced.lexically_normal();
}

inline bool ReadShaderPackFile(const std::string& path, ShaderPackAssetData& outData) {
    outData = {};
    if (path.empty()) {
        return false;
    }

    const fs::path packPath(path);
    const std::string ext = MaterialAssetLowercase(packPath.extension().string());
    const std::string stem = MaterialAssetLowercase(packPath.stem().string());
    if (ext == ".vert") {
        outData.vertexShaderPath = path;
        return true;
    }
    if (ext == ".frag") {
        outData.fragmentShaderPath = path;
        return true;
    }
    if (ext == ".glsl") {
        if (stem.find("vert") != std::string::npos ||
            stem.find("vertex") != std::string::npos ||
            (stem.size() >= 2 && stem.compare(stem.size() - 2, 2, "_v") == 0)) {
            outData.vertexShaderPath = path;
        } else {
            outData.fragmentShaderPath = path;
        }
        return true;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = MaterialAssetTrim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }

        const std::string key = MaterialAssetLowercase(MaterialAssetTrim(line.substr(0, eqPos)));
        const std::string value = MaterialAssetTrim(line.substr(eqPos + 1));
        if (key == "vertex" || key == "vert" || key == "vertexshader") {
            outData.vertexShaderPath = ResolveShaderPackReferencedPath(packPath, value).string();
        } else if (key == "fragment" || key == "frag" || key == "fragmentshader") {
            outData.fragmentShaderPath = ResolveShaderPackReferencedPath(packPath, value).string();
        }
    }

    if (ext == ".modushader") {
        return !outData.vertexShaderPath.empty() && !outData.fragmentShaderPath.empty();
    }

    if (!outData.vertexShaderPath.empty() || !outData.fragmentShaderPath.empty()) {
        return true;
    }

    if (ext == ".shader") {
        outData.vertexShaderPath = path;
        outData.fragmentShaderPath = path;
        return true;
    }

    return false;
}
