#include "MaterialShaderProfile.h"
#include "EngineMaterialIO.h"
#include "DurableFile.h"
#include "MaterialAssetUtils.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
// Unity's text-serialized .mat shares our extension and opens with "%YAML"
// (optionally behind a UTF-8 BOM). Neither the reader nor the writer may treat
// one as a Modularity material.
bool StartsWithUnityYamlHeader(const std::string& text) {
    size_t start = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        start = 3;
    }
    return text.compare(start, 5, "%YAML") == 0;
}

bool FileStartsWithUnityYamlHeader(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    char head[8] = {};
    in.read(head, sizeof(head));
    return StartsWithUnityYamlHeader(std::string(head, static_cast<size_t>(in.gcount())));
}


} // namespace

// `path` only anchors a relative shaderPack reference; the text comes from `f`.
static bool readMaterialStream(std::istream& f, const std::string& path, MaterialFileData& outData) {
    std::string line;
    bool firstLine = true;
    while (std::getline(f, line)) {
        if (firstLine) {
            firstLine = false;
            // A Unity YAML .mat would otherwise "parse" as an all-defaults material.
            if (StartsWithUnityYamlHeader(line)) return false;
        }
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        // One malformed value (a blank "alpha=" from a hand edit, say) skips that
        // line instead of throwing out of the Inspector's frame.
        try {
        if (ParseMaterialShaderParameter(key, val, outData.props.shaderParameters)) {}
        else if (key == "color") {sscanf(val.c_str(), "%f,%f,%f", &outData.props.color.r, &outData.props.color.g, &outData.props.color.b);}
        else if (key == "opacity" || key == "alpha") {outData.props.alpha = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "ambient")          {outData.props.ambientStrength = std::stof(val);}
        else if (key == "specular")         {outData.props.specularStrength = std::stof(val);}
        else if (key == "shininess")        {outData.props.shininess = std::stof(val);}
        else if (key == "normalIntensity" || key == "normalMapIntensity") {outData.props.normalMapIntensity = std::clamp(std::stof(val), 0.0f, 2.0f);}
        else if (key == "textureMix")       {outData.props.textureMix = std::stof(val);}
        else if (key == "uvTiling")         {sscanf(val.c_str(), "%f,%f", &outData.props.uvTiling.x, &outData.props.uvTiling.y);}
        else if (key == "uvOffset")         {sscanf(val.c_str(), "%f,%f", &outData.props.uvOffset.x, &outData.props.uvOffset.y);}
        else if (key == "scrollSpeed")      {outData.props.scrollSpeed = std::max(0.0f, std::stof(val));}
        else if (key == "scrollDirection")  {sscanf(val.c_str(), "%f,%f", &outData.props.scrollDirection.x, &outData.props.scrollDirection.y);}
        else if (key == "uvScrollEnabled") {outData.props.uvScrollEnabled = (val == "1" || val == "true");}
        else if (key == "cloudColor")       {sscanf(val.c_str(), "%f,%f,%f", &outData.props.cloudColor.r, &outData.props.cloudColor.g, &outData.props.cloudColor.b);}
        else if (key == "cloudSkyColor")    {sscanf(val.c_str(), "%f,%f,%f", &outData.props.cloudSkyColor.r, &outData.props.cloudSkyColor.g, &outData.props.cloudSkyColor.b);}
        else if (key == "cloudScale")       {outData.props.cloudScale = std::max(0.0001f, std::stof(val));}
        else if (key == "cloudCoverage")    {outData.props.cloudCoverage = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "cloudSoftness")    {outData.props.cloudSoftness = std::clamp(std::stof(val), 0.001f, 1.0f);}
        else if (key == "cloudDetail")      {outData.props.cloudDetail = std::clamp(std::stoi(val), 1, 8);}
        else if (key == "cloudSpeed")       {outData.props.cloudSpeed = std::stof(val);}
        else if (key == "cloudWarp")        {outData.props.cloudWarp = std::max(0.0f, std::stof(val));}
        else if (key == "cloudHighlight")   {outData.props.cloudHighlight = std::max(0.0f, std::stof(val));}
        else if (key == "cloudStars")       {outData.props.cloudStars = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "cloudHorizon")     {outData.props.cloudHorizon = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "textureFilter") {
            std::string lower = val;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {return static_cast<char>(std::tolower(c));});
            if (lower == "1" || lower == "point" || lower == "nearest") {outData.props.textureFilter = MaterialProperties::TextureFilter::Point;}
            else                            {outData.props.textureFilter = MaterialProperties::TextureFilter::Bilinear;}}
        else if (key == "pbrEnabled")       {outData.props.pbrEnabled = (val == "1" || val == "true");}
        else if (key == "metallic")         {outData.props.metallic = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "roughness")        {outData.props.roughness = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "occlusionStrength"){outData.props.occlusionStrength = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "emissionColor")    {sscanf(val.c_str(), "%f,%f,%f", &outData.props.emissionColor.r, &outData.props.emissionColor.g, &outData.props.emissionColor.b);}
        else if (key == "emissionStrength") {outData.props.emissionStrength = std::max(0.0f, std::stof(val));}
        else if (key == "metallicRoughness"){outData.metallicRoughness = val;}
        else if (key == "metallicMap"){outData.metallicMap = val;}
        else if (key == "roughnessMap"){outData.roughnessMap = val;}
        else if (key == "opacityMap"){outData.opacityMap = val;}
        else if (key == "alphaCutoff"){outData.props.alphaCutoff = std::max(0.0f, std::min(1.0f, std::stof(val)));}
        else if (key == "emissive")         {outData.emissive = val;}
        else if (key == "heightScale")      {outData.props.heightScale = std::clamp(std::stof(val), 0.0f, 1.0f);}
        else if (key == "parallaxSteps")    {outData.props.parallaxSteps = std::clamp(std::stoi(val), 4, 256);}
        else if (key == "parallaxInvertHeight") {outData.props.parallaxInvertHeight = (val == "1" || val == "true");}
        else if (key == "height")           {outData.height = val;}
        else if (key == "occlusion")        {outData.occlusion = val;}
        else if (key == "albedo")           {outData.albedo = val;}
        else if (key == "overlay")          {outData.overlay = val;}
        else if (key == "normal")           {outData.normal = val;}
        else if (key == "useOverlay")       {outData.useOverlay = std::stoi(val) != 0;}
        else if (key == "shaderPack") {
            outData.shaderPack = val.empty() ? std::string() :
                (Modularity::Platform::GetAssetSource().Exists(val)
                    ? Modularity::Platform::ResolveAssetPath(val)
                    : ResolveShaderPackReferencedPath(fs::path(path), val).string());
        }
        else if (key == "vertexShader")     {outData.vertexShader = val;}
        else if (key == "fragmentShader")   {outData.fragmentShader = val;}
        } catch (const std::exception&) {}
    }
    if (!outData.shaderPack.empty()) {
        if (!outData.props.usesProjectShader())
            SetMaterialShaderProfile(outData.props.shaderParameters, outData.shaderPack);
        ShaderPackAssetData shaderPackData;
        if (ReadShaderPackFile(outData.shaderPack, shaderPackData)) {outData.vertexShader = shaderPackData.vertexShaderPath; outData.fragmentShader = shaderPackData.fragmentShaderPath;}}
    return true;
}
bool readMaterialFile(const std::string& path, MaterialFileData& outData) {
    std::ifstream f(path); if (!f.is_open()) {return false;}
    return readMaterialStream(f, path, outData);
}
bool parseMaterialText(const std::string& text, const std::string& path, MaterialFileData& outData) {
    std::istringstream f(text);
    return readMaterialStream(f, path, outData);
}
bool writeMaterialFile(const MaterialFileData& data, const std::string& path) {
    // Built in memory and written through WriteFileDurably: the Inspector autosaves a material
    // every time an edit settles, and a plain ofstream truncates first, so a crash right after
    // could leave the .mat as zero bytes (see DurableFile.h).
    if (FileStartsWithUnityYamlHeader(path)) {
        std::cerr << "[Material] Refusing to overwrite a Unity YAML .mat: " << path << "\n";
        return false;
    }
    std::ostringstream f;
    f << "# Material\n";
    f << "color=" << data.props.color.r << "," << data.props.color.g << "," << data.props.color.b << "\n";
    f << "opacity=" << data.props.alpha << "\n";
    f << "ambient=" << data.props.ambientStrength << "\n";
    f << "specular=" << data.props.specularStrength << "\n";
    f << "shininess=" << data.props.shininess << "\n";
    f << "normalMapIntensity=" << data.props.normalMapIntensity << "\n";
    f << "textureMix=" << data.props.textureMix << "\n";
    f << "uvTiling=" << data.props.uvTiling.x << "," << data.props.uvTiling.y << "\n";
    f << "uvOffset=" << data.props.uvOffset.x << "," << data.props.uvOffset.y << "\n";
    f << "scrollSpeed=" << data.props.scrollSpeed << "\n";
    f << "scrollDirection=" << data.props.scrollDirection.x << "," << data.props.scrollDirection.y << "\n";
    f << "uvScrollEnabled=" << (data.props.uvScrollEnabled ? 1 : 0) << "\n";
    f << "cloudColor=" << data.props.cloudColor.r << "," << data.props.cloudColor.g << "," << data.props.cloudColor.b << "\n";
    f << "cloudSkyColor=" << data.props.cloudSkyColor.r << "," << data.props.cloudSkyColor.g << "," << data.props.cloudSkyColor.b << "\n";
    f << "cloudScale=" << data.props.cloudScale << "\n";
    f << "cloudCoverage=" << data.props.cloudCoverage << "\n";
    f << "cloudSoftness=" << data.props.cloudSoftness << "\n";
    f << "cloudDetail=" << data.props.cloudDetail << "\n";
    f << "cloudSpeed=" << data.props.cloudSpeed << "\n";
    f << "cloudWarp=" << data.props.cloudWarp << "\n";
    f << "cloudHighlight=" << data.props.cloudHighlight << "\n";
    f << "cloudStars=" << data.props.cloudStars << "\n";
    f << "cloudHorizon=" << data.props.cloudHorizon << "\n";
    f << "textureFilter=" << static_cast<int>(data.props.textureFilter) << "\n";
    f << "pbrEnabled=" << (data.props.pbrEnabled ? 1 : 0) << "\n";
    f << "metallic=" << data.props.metallic << "\n";
    f << "roughness=" << data.props.roughness << "\n";
    f << "occlusionStrength=" << data.props.occlusionStrength << "\n";
    f << "emissionColor=" << data.props.emissionColor.r << "," << data.props.emissionColor.g << "," << data.props.emissionColor.b << "\n";
    f << "emissionStrength=" << data.props.emissionStrength << "\n";
    f << "heightScale=" << data.props.heightScale << "\n";
    f << "parallaxSteps=" << data.props.parallaxSteps << "\n";
    f << "parallaxInvertHeight=" << (data.props.parallaxInvertHeight ? 1 : 0) << "\n";
    WriteMaterialShaderParameters(f, data.props.shaderParameters);
    f << "useOverlay=" << (data.useOverlay ? 1 : 0) << "\n";
    f << "albedo=" << data.albedo << "\n";
    f << "overlay=" << data.overlay << "\n";
    f << "normal=" << data.normal << "\n";
    f << "metallicRoughness=" << data.metallicRoughness << "\n";
    f << "metallicMap=" << data.metallicMap << "\n";
    f << "roughnessMap=" << data.roughnessMap << "\n";
    f << "opacityMap=" << data.opacityMap << "\n";
    f << "alphaCutoff=" << data.props.alphaCutoff << "\n";
    f << "emissive=" << data.emissive << "\n";
    f << "occlusion=" << data.occlusion << "\n";
    f << "height=" << data.height << "\n";
    f << "shaderPack=" << data.shaderPack << "\n";
    f << "vertexShader=" << data.vertexShader << "\n";
    f << "fragmentShader=" << data.fragmentShader << "\n";
    std::string error;
    // Text mode keeps the CRLF line endings the old text-mode ofstream wrote on Windows.
    if (!Modularity::WriteFileDurably(std::filesystem::path(path), f.str(), error,
                                      Modularity::DurableFileMode::Text)) {
        std::cerr << "[Material] " << error << "\n";
        return false;
    }
    return true;}
