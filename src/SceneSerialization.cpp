#include "SceneSerializationInternal.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace {

using FieldMap = std::map<std::string, std::string>;

struct FlatSceneObjectData {
    FieldMap fields;
};

struct FlatSceneDocument {
    FieldMap settings;
    std::vector<FlatSceneObjectData> objects;
};

struct BlockHeader {
    std::string identifier;
    FieldMap primaryParams;
    FieldMap secondaryParams;
};

struct LineCursor {
    const std::vector<std::string>& lines;
    size_t index = 0;
};

struct ComponentSchema {
    std::string nodeName;
    std::string presenceFlagKey;
    std::string enabledLegacyKey;
    std::vector<std::string> explicitLegacyKeys;
    std::vector<std::string> legacyPrefixes;
    std::vector<std::string> excludedPrefixes;
    std::map<std::string, std::string> legacyToNode;
    std::map<std::string, std::string> nodeToLegacy;

    bool ownsLegacyKey(const std::string& key) const {
        if (!presenceFlagKey.empty() && key == presenceFlagKey) return false;
        if (!enabledLegacyKey.empty() && key == enabledLegacyKey) return false;
        if (std::find(explicitLegacyKeys.begin(), explicitLegacyKeys.end(), key) != explicitLegacyKeys.end()) {
            return true;
        }
        for (const std::string& excluded : excludedPrefixes) {
            if (!excluded.empty() && key.rfind(excluded, 0) == 0) {
                return false;
            }
        }
        for (const std::string& prefix : legacyPrefixes) {
            if (!prefix.empty() && key.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    }

    std::string nodeKeyForLegacy(const std::string& legacyKey) const {
        auto it = legacyToNode.find(legacyKey);
        if (it != legacyToNode.end()) {
            return it->second;
        }
        return legacyKey;
    }

    std::string legacyKeyForNode(const std::string& nodeKey) const {
        auto it = nodeToLegacy.find(nodeKey);
        if (it != nodeToLegacy.end()) {
            return it->second;
        }
        return nodeKey;
    }
};

std::string TrimCopy(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string StripLineComment(const std::string& line) {
    bool inQuotes = false;
    bool escaped = false;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes && c == '/' && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

bool IsCommentLine(const std::string& line) {
    const std::string trimmed = TrimCopy(line);
    return trimmed.empty() || trimmed[0] == '#';
}

std::vector<std::string> LoadMeaningfulLines(std::istream& in) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        const std::string withoutComments = TrimCopy(StripLineComment(line));
        if (IsCommentLine(withoutComments)) continue;
        lines.push_back(withoutComments);
    }
    return lines;
}

size_t FindOutsideQuotes(const std::string& text, char needle) {
    bool inQuotes = false;
    bool escaped = false;
    int parenDepth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes) {
            if (c == '(') ++parenDepth;
            else if (c == ')' && parenDepth > 0) --parenDepth;
            else if (c == needle && parenDepth == 0) return i;
        }
    }
    return std::string::npos;
}

std::vector<std::string> SplitOutsideQuotes(const std::string& text, char separator) {
    std::vector<std::string> parts;
    bool inQuotes = false;
    bool escaped = false;
    int parenDepth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes) {
            if (c == '(') ++parenDepth;
            else if (c == ')' && parenDepth > 0) --parenDepth;
            else if (c == separator && parenDepth == 0) {
                parts.push_back(TrimCopy(text.substr(start, i - start)));
                start = i + 1;
            }
        }
    }
    parts.push_back(TrimCopy(text.substr(start)));
    return parts;
}

std::string UnquoteIfWrapped(const std::string& value) {
    const std::string trimmed = TrimCopy(value);
    if (trimmed.size() < 2 || trimmed.front() != '"' || trimmed.back() != '"') {
        return trimmed;
    }

    std::string result;
    result.reserve(trimmed.size() - 2);
    bool escaped = false;
    for (size_t i = 1; i + 1 < trimmed.size(); ++i) {
        const char c = trimmed[i];
        if (escaped) {
            result.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else {
            result.push_back(c);
        }
    }
    return result;
}

std::string QuoteString(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (char c : value) {
        if (c == '\\' || c == '"') {
            quoted.push_back('\\');
        }
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

bool LooksNumericish(const std::string& value) {
    const std::string trimmed = TrimCopy(value);
    if (trimmed.empty()) return false;
    for (char c : trimmed) {
        if (std::isdigit(static_cast<unsigned char>(c))) continue;
        switch (c) {
            case '-':
            case '+':
            case '.':
            case ',':
            case ';':
            case ' ':
                continue;
            default:
                return false;
        }
    }
    return true;
}

std::string FormatValue(const std::string& value, bool forceQuote = false) {
    const std::string trimmed = TrimCopy(value);
    if (trimmed.empty()) {
        return forceQuote ? "\"\"" : "";
    }
    if (!forceQuote && trimmed.front() == '"' && trimmed.back() == '"') {
        return trimmed;
    }
    if (!forceQuote && LooksNumericish(trimmed)) {
        return trimmed;
    }
    return QuoteString(trimmed);
}

FieldMap ParseParamList(std::string text) {
    FieldMap params;
    text = TrimCopy(text);
    if (text.empty()) return params;
    if (text.front() == '(' && text.back() == ')') {
        text = TrimCopy(text.substr(1, text.size() - 2));
    }
    for (const std::string& part : SplitOutsideQuotes(text, ',')) {
        if (part.empty()) continue;
        const size_t eq = FindOutsideQuotes(part, '=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimCopy(part.substr(0, eq));
        const std::string value = UnquoteIfWrapped(part.substr(eq + 1));
        params[key] = value;
    }
    return params;
}

bool ParseBlockHeader(const std::string& line, BlockHeader& out) {
    std::string trimmed = TrimCopy(line);
    if (trimmed.empty()) return false;
    if (trimmed.back() != '{') return false;
    trimmed = TrimCopy(trimmed.substr(0, trimmed.size() - 1));

    const size_t eq = FindOutsideQuotes(trimmed, '=');
    std::string left = eq == std::string::npos ? trimmed : TrimCopy(trimmed.substr(0, eq));
    std::string right = eq == std::string::npos ? "" : TrimCopy(trimmed.substr(eq + 1));

    const size_t openParen = left.find('(');
    if (openParen != std::string::npos) {
        out.identifier = TrimCopy(left.substr(0, openParen));
        out.primaryParams = ParseParamList(left.substr(openParen));
    } else {
        out.identifier = TrimCopy(left);
        out.primaryParams.clear();
    }

    if (!right.empty()) {
        out.secondaryParams = ParseParamList(right);
    } else {
        out.secondaryParams.clear();
    }

    return !out.identifier.empty();
}

bool IsBlockClose(const std::string& line) {
    const std::string trimmed = TrimCopy(line);
    return trimmed == "}" || trimmed == "};";
}

bool ParseAssignment(const std::string& line, std::string& outKey, std::string& outValue) {
    std::string trimmed = TrimCopy(line);
    if (trimmed.empty()) return false;
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed.pop_back();
        trimmed = TrimCopy(trimmed);
    }
    const size_t eq = FindOutsideQuotes(trimmed, '=');
    if (eq == std::string::npos) return false;
    outKey = TrimCopy(trimmed.substr(0, eq));
    outValue = UnquoteIfWrapped(trimmed.substr(eq + 1));
    return !outKey.empty();
}

bool ParseLegacyFlatDocument(std::istream& in, FlatSceneDocument& outDoc) {
    outDoc.settings.clear();
    outDoc.objects.clear();

    std::string line;
    FlatSceneObjectData* currentObject = nullptr;
    while (std::getline(in, line)) {
        line = TrimCopy(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "[Object]") {
            outDoc.objects.push_back({});
            currentObject = &outDoc.objects.back();
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimCopy(line.substr(0, eq));
        const std::string value = line.substr(eq + 1);
        if (currentObject) {
            currentObject->fields[key] = value;
        } else {
            outDoc.settings[key] = value;
        }
    }
    return true;
}

bool DetectModularFormat(std::istream& in) {
    const std::streampos start = in.tellg();
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = TrimCopy(StripLineComment(line));
        if (IsCommentLine(trimmed)) continue;
        in.clear();
        in.seekg(start);
        return trimmed.rfind("MODU_SCENESETTINGS", 0) == 0 ||
               trimmed.rfind("MODU_GAMEOBJECT", 0) == 0 ||
               trimmed.rfind("MODU_SKYBOX", 0) == 0;
    }
    in.clear();
    in.seekg(start);
    return false;
}

bool IsTruthy(const FieldMap& fields, const std::string& key) {
    const auto it = fields.find(key);
    if (it == fields.end()) return false;
    const std::string value = TrimCopy(it->second);
    return value == "1" || value == "true" || value == "True";
}

std::vector<std::string> OrderedFieldKeys(const FieldMap& fields, const ComponentSchema& schema) {
    std::vector<std::string> ordered;
    std::set<std::string> seen;
    for (const std::string& key : schema.explicitLegacyKeys) {
        if (fields.count(key) > 0 && seen.insert(key).second) {
            ordered.push_back(key);
        }
    }
    std::vector<std::string> remaining;
    remaining.reserve(fields.size());
    for (const auto& [key, value] : fields) {
        (void)value;
        if (!schema.ownsLegacyKey(key)) continue;
        if (!seen.insert(key).second) continue;
        remaining.push_back(key);
    }
    std::sort(remaining.begin(), remaining.end());
    ordered.insert(ordered.end(), remaining.begin(), remaining.end());
    return ordered;
}

std::string ScriptDisplayName(const FieldMap& fields, int scriptIndex) {
    const std::string pathKey = "script" + std::to_string(scriptIndex) + "_path";
    auto pathIt = fields.find(pathKey);
    if (pathIt != fields.end() && !pathIt->second.empty()) {
        return fs::path(pathIt->second).stem().string();
    }
    const std::string typeKey = "script" + std::to_string(scriptIndex) + "_type";
    auto typeIt = fields.find(typeKey);
    if (typeIt != fields.end() && !typeIt->second.empty()) {
        return typeIt->second;
    }
    return "Script";
}

const std::vector<ComponentSchema>& GetComponentSchemas() {
    static const std::vector<ComponentSchema> schemas = {
        {
            "Renderer",
            "hasRenderer",
            "",
            {"renderType", "faceCamera", "meshPath", "meshSourceIndex"},
            {},
            {},
            {
                {"renderType", "type"},
                {"faceCamera", "faceCamera"},
                {"meshPath", "meshPath"},
                {"meshSourceIndex", "meshSourceIndex"},
            },
            {
                {"type", "renderType"},
                {"faceCamera", "faceCamera"},
                {"meshPath", "meshPath"},
                {"meshSourceIndex", "meshSourceIndex"},
            }
        },
        {
            "Material",
            "",
            "",
            {
                "materialColor", "materialAlpha", "materialAmbient", "materialSpecular",
                "materialShininess", "materialNormalMapIntensity", "materialTextureMix", "materialUvTiling", "materialUvOffset",
                "materialScrollSpeed", "materialScrollDirection", "materialUvScrollEnabled", "materialTextureFilter",
                "materialCloudColor", "materialCloudSkyColor", "materialCloudScale", "materialCloudCoverage",
                "materialCloudSoftness", "materialCloudDetail", "materialCloudSpeed", "materialCloudWarp",
                "materialCloudHighlight", "materialCloudStars", "materialCloudHorizon",
                "materialPbrEnabled", "materialMetallic", "materialRoughness",
                "materialOcclusionStrength", "materialEmissionColor", "materialEmissionStrength",
                "materialAlphaCutoff",
                "materialHeightScale", "materialParallaxSteps", "materialParallaxInvertHeight",
                "materialPath", "albedoTex", "overlayTex", "normalMap",
                "metallicRoughnessMap", "metallicMap", "roughnessMap", "opacityMap",
                "emissiveMap", "occlusionMap", "heightMap",
                "shaderPack", "vertexShader", "fragmentShader", "useOverlay", "additionalMaterialCount"
            },
            {"additionalMaterial"},
            {},
            {
                {"materialColor", "color"},
                {"materialAlpha", "alpha"},
                {"materialAlphaCutoff", "alphaCutoff"},
                {"materialAmbient", "ambient"},
                {"materialSpecular", "specular"},
                {"materialShininess", "shininess"},
                {"materialNormalMapIntensity", "normalMapIntensity"},
                {"materialTextureMix", "textureMix"},
                {"materialUvTiling", "uvTiling"},
                {"materialUvOffset", "uvOffset"},
                {"materialScrollSpeed", "scrollSpeed"},
                {"materialScrollDirection", "scrollDirection"},
                {"materialUvScrollEnabled", "uvScrollEnabled"},
                {"materialCloudColor", "cloudColor"},
                {"materialCloudSkyColor", "cloudSkyColor"},
                {"materialCloudScale", "cloudScale"},
                {"materialCloudCoverage", "cloudCoverage"},
                {"materialCloudSoftness", "cloudSoftness"},
                {"materialCloudDetail", "cloudDetail"},
                {"materialCloudSpeed", "cloudSpeed"},
                {"materialCloudWarp", "cloudWarp"},
                {"materialCloudHighlight", "cloudHighlight"},
                {"materialCloudStars", "cloudStars"},
                {"materialCloudHorizon", "cloudHorizon"},
                {"materialTextureFilter", "textureFilter"},
                {"materialPbrEnabled", "pbrEnabled"},
                {"materialMetallic", "metallic"},
                {"materialRoughness", "roughness"},
                {"materialOcclusionStrength", "occlusionStrength"},
                {"materialEmissionColor", "emissionColor"},
                {"materialEmissionStrength", "emissionStrength"},
                {"materialHeightScale", "heightScale"},
                {"materialParallaxSteps", "parallaxSteps"},
                {"materialParallaxInvertHeight", "parallaxInvertHeight"},
                {"materialPath", "materialPath"},
                {"albedoTex", "albedoTexture"},
                {"overlayTex", "overlayTexture"},
                {"normalMap", "normalMap"},
                {"metallicRoughnessMap", "metallicRoughnessMap"},
                {"metallicMap", "metallicMap"},
                {"roughnessMap", "roughnessMap"},
                {"opacityMap", "opacityMap"},
                {"emissiveMap", "emissiveMap"},
                {"occlusionMap", "occlusionMap"},
                {"heightMap", "heightMap"},
                {"shaderPack", "shaderPack"},
                {"vertexShader", "vertexShader"},
                {"fragmentShader", "fragmentShader"},
                {"useOverlay", "useOverlay"},
            },
            {
                {"color", "materialColor"},
                {"alpha", "materialAlpha"},
                {"alphaCutoff", "materialAlphaCutoff"},
                {"ambient", "materialAmbient"},
                {"specular", "materialSpecular"},
                {"shininess", "materialShininess"},
                {"normalMapIntensity", "materialNormalMapIntensity"},
                {"textureMix", "materialTextureMix"},
                {"uvTiling", "materialUvTiling"},
                {"uvOffset", "materialUvOffset"},
                {"scrollSpeed", "materialScrollSpeed"},
                {"scrollDirection", "materialScrollDirection"},
                {"uvScrollEnabled", "materialUvScrollEnabled"},
                {"cloudColor", "materialCloudColor"},
                {"cloudSkyColor", "materialCloudSkyColor"},
                {"cloudScale", "materialCloudScale"},
                {"cloudCoverage", "materialCloudCoverage"},
                {"cloudSoftness", "materialCloudSoftness"},
                {"cloudDetail", "materialCloudDetail"},
                {"cloudSpeed", "materialCloudSpeed"},
                {"cloudWarp", "materialCloudWarp"},
                {"cloudHighlight", "materialCloudHighlight"},
                {"cloudStars", "materialCloudStars"},
                {"cloudHorizon", "materialCloudHorizon"},
                {"textureFilter", "materialTextureFilter"},
                {"pbrEnabled", "materialPbrEnabled"},
                {"metallic", "materialMetallic"},
                {"roughness", "materialRoughness"},
                {"occlusionStrength", "materialOcclusionStrength"},
                {"emissionColor", "materialEmissionColor"},
                {"emissionStrength", "materialEmissionStrength"},
                {"heightScale", "materialHeightScale"},
                {"parallaxSteps", "materialParallaxSteps"},
                {"parallaxInvertHeight", "materialParallaxInvertHeight"},
                {"materialPath", "materialPath"},
                {"albedoTexture", "albedoTex"},
                {"overlayTexture", "overlayTex"},
                {"normalMap", "normalMap"},
                {"metallicRoughnessMap", "metallicRoughnessMap"},
                {"metallicMap", "metallicMap"},
                {"roughnessMap", "roughnessMap"},
                {"opacityMap", "opacityMap"},
                {"emissiveMap", "emissiveMap"},
                {"occlusionMap", "occlusionMap"},
                {"heightMap", "heightMap"},
                {"shaderPack", "shaderPack"},
                {"vertexShader", "vertexShader"},
                {"fragmentShader", "fragmentShader"},
                {"useOverlay", "useOverlay"},
            }
        },
        {
            "Camera",
            "hasCamera",
            "",
            {"cameraType", "cameraFov", "cameraNear", "cameraFar", "cameraPostFX", "cameraUse2D", "cameraPixelsPerUnit",
             "cameraProjection", "cameraFovAxis", "cameraOrthoSize", "cameraRenderShadows", "cameraBackground",
             "cameraBackgroundColor", "cameraCullingMask"},
            {},
            {},
            {
                {"cameraType", "type"},
                {"cameraFov", "fov"},
                {"cameraNear", "near"},
                {"cameraFar", "far"},
                {"cameraPostFX", "postFX"},
                {"cameraUse2D", "use2D"},
                {"cameraPixelsPerUnit", "pixelsPerUnit"},
                {"cameraProjection", "projection"},
                {"cameraFovAxis", "fovAxis"},
                {"cameraOrthoSize", "orthoSize"},
                {"cameraRenderShadows", "renderShadows"},
                {"cameraBackground", "background"},
                {"cameraBackgroundColor", "backgroundColor"},
                {"cameraCullingMask", "cullingMask"},
            },
            {
                {"type", "cameraType"},
                {"fov", "cameraFov"},
                {"near", "cameraNear"},
                {"far", "cameraFar"},
                {"postFX", "cameraPostFX"},
                {"use2D", "cameraUse2D"},
                {"pixelsPerUnit", "cameraPixelsPerUnit"},
                {"projection", "cameraProjection"},
                {"fovAxis", "cameraFovAxis"},
                {"orthoSize", "cameraOrthoSize"},
                {"renderShadows", "cameraRenderShadows"},
                {"background", "cameraBackground"},
                {"backgroundColor", "cameraBackgroundColor"},
                {"cullingMask", "cameraCullingMask"},
            }
        },
        {
            "CameraFollow2D",
            "hasCameraFollow2D",
            "cameraFollow2dEnabled",
            {"cameraFollow2dTarget", "cameraFollow2dOffset", "cameraFollow2dSmoothTime"},
            {},
            {},
            {
                {"cameraFollow2dTarget", "target"},
                {"cameraFollow2dOffset", "offset"},
                {"cameraFollow2dSmoothTime", "smoothTime"},
            },
            {
                {"target", "cameraFollow2dTarget"},
                {"offset", "cameraFollow2dOffset"},
                {"smoothTime", "cameraFollow2dSmoothTime"},
            }
        },
        {
            "Rigidbody",
            "hasRigidbody",
            "rbEnabled",
            {},
            {"rb"},
            {},
            {},
            {}
        },
        {
            "Rigidbody2D",
            "hasRigidbody2D",
            "rb2dEnabled",
            {},
            {"rb2d"},
            {},
            {},
            {}
        },
        {
            "Collider2D",
            "hasCollider2D",
            "collider2dEnabled",
            {},
            {"collider2d"},
            {},
            {},
            {}
        },
        {
            "ParallaxLayer2D",
            "hasParallaxLayer2D",
            "parallax2dEnabled",
            {},
            {"parallax2d"},
            {},
            {},
            {}
        },
        {
            "AssemblageLayer",
            "assemblageLayer",
            "assemblageLayerEnabled",
            {},
            {"assemblageLayer"},
            {},
            {},
            {}
        },
        {
            "Assemblage",
            "assemblage",
            "assemblageEnabled",
            {},
            {"assemblage"},
            {"assemblageLayer"},
            {},
            {}
        },
        {
            "Collider",
            "hasCollider",
            "colliderEnabled",
            {},
            {"collider"},
            {"collider2d"},
            {},
            {}
        },
        {
            "PlayerController",
            "hasPlayerController",
            "pcEnabled",
            {},
            {"pc"},
            {},
            {},
            {}
        },
        {
            "AudioSource",
            "hasAudioSource",
            "audioEnabled",
            {},
            {"audio"},
            {},
            {},
            {}
        },
        {
            "AudioFX",
            "hasAudioFX",
            "afxEnabled",
            {},
            {"afx"},
            {},
            {},
            {}
        },
        {
            "VideoPlayer",
            "hasVideoPlayer",
            "videoEnabled",
            {"videoPath", "videoPlayOnAwake", "videoLoop", "videoPlaybackSpeed", "videoPlayAudioFromVideo",
             "videoRouteAudioToSource", "videoOutputAudioSourceObjectId", "videoAudioVolume", "videoAudioMuted",
             "videoSyncAudioToVideo", "videoAudioSyncTolerance", "videoFlipX", "videoFlipY"},
            {},
            {},
            {
                {"videoPath", "path"},
                {"videoPlayOnAwake", "playOnAwake"},
                {"videoLoop", "loop"},
                {"videoFlipX", "flipX"},
                {"videoFlipY", "flipY"},
                {"videoPlaybackSpeed", "playbackSpeed"},
                {"videoPlayAudioFromVideo", "playAudioFromVideo"},
                {"videoRouteAudioToSource", "routeAudioToSource"},
                {"videoOutputAudioSourceObjectId", "outputAudioSourceObjectId"},
                {"videoAudioVolume", "videoAudioVolume"},
                {"videoAudioMuted", "videoAudioMuted"},
                {"videoSyncAudioToVideo", "syncAudioToVideo"},
                {"videoAudioSyncTolerance", "audioSyncTolerance"},
            },
            {
                {"path", "videoPath"},
                {"playOnAwake", "videoPlayOnAwake"},
                {"loop", "videoLoop"},
                {"flipX", "videoFlipX"},
                {"flipY", "videoFlipY"},
                {"playbackSpeed", "videoPlaybackSpeed"},
                {"playAudioFromVideo", "videoPlayAudioFromVideo"},
                {"routeAudioToSource", "videoRouteAudioToSource"},
                {"outputAudioSourceObjectId", "videoOutputAudioSourceObjectId"},
                {"videoAudioVolume", "videoAudioVolume"},
                {"videoAudioMuted", "videoAudioMuted"},
                {"syncAudioToVideo", "videoSyncAudioToVideo"},
                {"audioSyncTolerance", "videoAudioSyncTolerance"},
            }
        },
        {
            "ParticleSystem2D",
            "hasParticleSystem2D",
            "ps2dEnabled",
            {},
            {"ps2d"},
            {},
            {},
            {}
        },
        {
            "ReverbZone",
            "hasReverbZone",
            "reverbEnabled",
            {},
            {"reverb"},
            {},
            {},
            {}
        },
        {
            "XROrigin",
            "hasXROrigin",
            "xrOriginEnabled",
            {},
            {"xrOrigin"},
            {},
            {},
            {}
        },
        {
            "XRCamera",
            "hasXRCamera",
            "xrCameraEnabled",
            {},
            {"xrCamera"},
            {},
            {},
            {}
        },
        {
            "XRController",
            "hasXRController",
            "xrControllerEnabled",
            {},
            {"xrController"},
            {},
            {},
            {}
        },
        {
            "XRActionBasedController",
            "hasXRActionBasedController",
            "xrActionControllerEnabled",
            {},
            {"xrActionController"},
            {},
            {},
            {}
        },
        {
            "XRRayInteractor",
            "hasXRRayInteractor",
            "xrRayEnabled",
            {},
            {"xrRay"},
            {},
            {},
            {}
        },
        {
            "XRDirectInteractor",
            "hasXRDirectInteractor",
            "xrDirectEnabled",
            {},
            {"xrDirect"},
            {},
            {},
            {}
        },
        {
            "XRGrabInteractable",
            "hasXRGrabInteractable",
            "xrGrabEnabled",
            {},
            {"xrGrab"},
            {},
            {},
            {}
        },
        {
            "GroundBakedType",
            "hasGroundBakedType",
            "groundBakedEnabled",
            {},
            {"groundBaked"},
            {},
            {},
            {}
        },
        {
            "ObsticleObject",
            "hasObsticleObject",
            "obsticleEnabled",
            {},
            {"obsticle"},
            {},
            {},
            {}
        },
        {
            "MapRoot",
            "hasMapRoot",
            "mapRootEnabled",
            {},
            {"mapRoot"},
            {},
            {},
            {}
        },
        {
            "MapSector",
            "hasMapSector",
            "mapSectorEnabled",
            {},
            {"mapSector"},
            {},
            {},
            {}
        },
        {
            "MapTransition",
            "hasMapTransition",
            "mapTransitionEnabled",
            {},
            {"mapTransition"},
            {},
            {},
            {}
        },
        {
            "MapPortal",
            "hasMapPortal",
            "mapPortalEnabled",
            {},
            {"mapPortal"},
            {},
            {},
            {}
        },
        {
            "MapMesh",
            "hasMapMesh",
            "mapMeshEnabled",
            {},
            {"mapMesh"},
            {},
            {},
            {}
        },
        {
            "AIAgent",
            "hasAIAgent",
            "aiAgentEnabled",
            {},
            {"aiAgent"},
            {},
            {},
            {}
        },
        {
            "OffMeshLink",
            "hasOffMeshLink",
            "offMeshLinkEnabled",
            {},
            {"offMeshLink"},
            {},
            {},
            {}
        },
        {
            "Rig25DRoot",
            "hasRig25DRoot",
            "rig25dRootEnabled",
            {},
            {"rig25dRoot"},
            {},
            {},
            {}
        },
        {
            "Rig25DNode",
            "hasRig25DNode",
            "rig25dNodeEnabled",
            {},
            {"rig25dNode"},
            {},
            {},
            {}
        },
        {
            "Animation",
            "hasAnimation",
            "animEnabled",
            {},
            {"anim"},
            {},
            {},
            {}
        },
        {
            "SkeletalAnimation",
            "hasSkeletalAnimation",
            "skelEnabled",
            {},
            {"skel"},
            {},
            {},
            {}
        },
        {
            "UI",
            "hasUI",
            "",
            {"uiType"},
            {"ui"},
            {},
            {
                {"uiType", "type"},
            },
            {
                {"type", "uiType"},
            }
        },
        {
            "PostFX",
            "hasPostFX",
            "postEnabled",
            {},
            {"post"},
            {},
            {},
            {}
        },
        {
            "Light2D",
            "hasLight2D",
            "light2dEnabled",
            {},
            {"light2d"},
            {},
            {
                {"light2dType", "type"},
            },
            {
                {"type", "light2dType"},
            }
        },
        {
            "Light",
            "hasLight",
            "lightEnabled",
            {"lightType"},
            {"light"},
            {"light2d"},
            {
                {"lightType", "type"},
            },
            {
                {"type", "lightType"},
            }
        },
        {
            "ReflectionCast",
            "hasReflectionCast",
            "reflectionCastEnabled",
            {"reflectionCastUpdateMode", "reflectionCastBox", "reflectionCastBlend", "reflectionCastIntensity", "reflectionCastResolution"},
            {},
            {},
            {
                {"reflectionCastUpdateMode", "updateMode"},
                {"reflectionCastBox", "box"},
                {"reflectionCastBlend", "blend"},
                {"reflectionCastIntensity", "intensity"},
                {"reflectionCastResolution", "resolution"},
            },
            {
                {"updateMode", "reflectionCastUpdateMode"},
                {"box", "reflectionCastBox"},
                {"blend", "reflectionCastBlend"},
                {"intensity", "reflectionCastIntensity"},
                {"resolution", "reflectionCastResolution"},
            }
        },
        {
            "ShadowCaster2D",
            "hasShadowCaster2D",
            "shadowCaster2dEnabled",
            {},
            {"shadowCaster2d"},
            {},
            {},
            {}
        },
        {
            "ModuObjInstance",
            "moduObjInstance",
            "",
            {},
            {"moduObj"},
            {},
            {},
            {}
        },
        {
            "NetworkIdentity",
            "netIdentity",
            "netIdentityEnabled",
            {},
            {"netIdentity"},
            {},
            {},
            {}
        },
        {
            "NetworkManager",
            "netManager",
            "netManagerEnabled",
            {},
            {"netManager"},
            {},
            {},
            {}
        },
    };
    return schemas;
}

const ComponentSchema* FindSchemaByNodeName(const std::string& nodeName) {
    const auto& schemas = GetComponentSchemas();
    for (const ComponentSchema& schema : schemas) {
        if (schema.nodeName == nodeName) {
            return &schema;
        }
    }
    return nullptr;
}

bool ShouldEmitMaterialNode(const FieldMap& fields) {
    if (IsTruthy(fields, "hasRenderer") || IsTruthy(fields, "hasUI")) {
        return true;
    }
    const auto& schemas = GetComponentSchemas();
    const auto it = std::find_if(schemas.begin(), schemas.end(), [](const ComponentSchema& schema) {
        return schema.nodeName == "Material";
    });
    if (it == schemas.end()) return false;
    for (const auto& [key, value] : fields) {
        if (!TrimCopy(value).empty() && it->ownsLegacyKey(key)) {
            return true;
        }
    }
    return false;
}

void WriteIndent(std::ostream& out, int indent) {
    for (int i = 0; i < indent; ++i) {
        out << ' ';
    }
}

void WriteField(std::ostream& out, int indent, const std::string& key, const std::string& value, bool forceQuote = false) {
    WriteIndent(out, indent);
    out << key << "=" << FormatValue(value, forceQuote) << ";\n";
}

int ResolveScriptCount(const FieldMap& fields) {
    const auto scriptCountIt = fields.find("scriptCount");
    if (scriptCountIt != fields.end()) {
        try {
            return std::max(0, std::stoi(scriptCountIt->second));
        } catch (...) {}
    }
    const auto scriptsIt = fields.find("scripts");
    if (scriptsIt != fields.end()) {
        try {
            return std::max(0, std::stoi(scriptsIt->second));
        } catch (...) {}
    }
    return 0;
}

void WriteScriptNode(std::ostream& out, const FieldMap& fields, int scriptIndex, int indent) {
    const std::string prefix = "script" + std::to_string(scriptIndex) + "_";
    const auto enabledIt = fields.find(prefix + "enabled");
    const auto idIt = fields.find(prefix + "id");
    const auto langIt = fields.find(prefix + "lang");
    const auto typeIt = fields.find(prefix + "type");
    const auto pathIt = fields.find(prefix + "path");

    WriteIndent(out, indent);
    out << "NODE_Script";
    if (enabledIt != fields.end()) {
        out << "(enabled=" << TrimCopy(enabledIt->second) << ")";
    }
    out << " = (scriptid=" << (idIt != fields.end() ? TrimCopy(idIt->second) : "0")
        << ", name=" << QuoteString(ScriptDisplayName(fields, scriptIndex))
        << ", lang=" << (langIt != fields.end() ? TrimCopy(langIt->second) : "0")
        << ") {\n";

    if (pathIt != fields.end()) {
        WriteField(out, indent + 4, "path", pathIt->second, true);
    }
    if (typeIt != fields.end() && !TrimCopy(typeIt->second).empty()) {
        WriteField(out, indent + 4, "managedType", typeIt->second, true);
    }

    int settingCount = 0;
    auto settingCountIt = fields.find(prefix + "settingCount");
    if (settingCountIt == fields.end()) {
        settingCountIt = fields.find(prefix + "settings");
    }
    if (settingCountIt != fields.end()) {
        try {
            settingCount = std::max(0, std::stoi(settingCountIt->second));
        } catch (...) {
            settingCount = 0;
        }
    }

    for (int settingIndex = 0; settingIndex < settingCount; ++settingIndex) {
        const auto settingIt = fields.find(prefix + "setting" + std::to_string(settingIndex));
        if (settingIt == fields.end()) continue;
        std::string raw = settingIt->second;
        std::string settingKey;
        std::string settingValue;
        const size_t sep = raw.find(':');
        if (sep == std::string::npos) {
            settingValue = raw;
        } else {
            settingKey = raw.substr(0, sep);
            settingValue = raw.substr(sep + 1);
        }
        WriteIndent(out, indent + 4);
        out << "SETTING = (key=" << QuoteString(settingKey) << ") {\n";
        WriteIndent(out, indent + 8);
        out << "value=" << TrimCopy(settingValue) << ";\n";
        WriteIndent(out, indent + 4);
        out << "}\n";
    }

    WriteIndent(out, indent);
    out << "}\n";
}

void WriteComponentNode(std::ostream& out, const FieldMap& fields, const ComponentSchema& schema, int indent) {
    WriteIndent(out, indent);
    out << "NODE_" << schema.nodeName;
    if (!schema.enabledLegacyKey.empty()) {
        const auto enabledIt = fields.find(schema.enabledLegacyKey);
        if (enabledIt != fields.end()) {
            out << "(enabled=" << TrimCopy(enabledIt->second) << ")";
        }
    }
    out << " {\n";

    const std::vector<std::string> orderedKeys = OrderedFieldKeys(fields, schema);
    for (const std::string& legacyKey : orderedKeys) {
        const auto it = fields.find(legacyKey);
        if (it == fields.end()) continue;
        const std::string nodeKey = schema.nodeKeyForLegacy(legacyKey);
        const bool forceQuote = !LooksNumericish(it->second);
        WriteField(out, indent + 4, nodeKey, it->second, forceQuote);
    }

    WriteIndent(out, indent);
    out << "}\n";
}

bool WriteModularScene(std::ostream& out,
                       const std::vector<SceneObject>& objects,
                       int nextId,
                       float timeOfDay,
                       const SkyboxSettings& skyboxSettings) {
    std::stringstream legacyText;
    if (!SceneSerializationInternal::WriteLegacySceneStream(legacyText, objects, nextId, timeOfDay, skyboxSettings)) {
        return false;
    }
    legacyText.str(SceneSerializationInternal::MakeSceneTextPathsPortable(legacyText.str()));

    FlatSceneDocument doc;
    legacyText.seekg(0);
    if (!ParseLegacyFlatDocument(legacyText, doc)) {
        return false;
    }

    out << "MODU_SCENESETTINGS{\n";
    WriteField(out, 4, "version", std::to_string(SceneSerializationInternal::kModularSceneFormatVersion));
    WriteField(out, 4, "nextId", doc.settings.count("nextId") ? doc.settings["nextId"] : std::to_string(nextId));
    WriteField(out, 4, "objectCount", std::to_string(objects.size()));
    out << "}\n\n";

    out << "MODU_SKYBOX {\n";
    WriteField(out, 4, "timeOfDay", doc.settings.count("timeOfDay") ? doc.settings["timeOfDay"] : "0");
    WriteField(out, 4, "skyboxMode", doc.settings.count("skyboxMode") ? doc.settings["skyboxMode"] : "0");
    WriteField(out, 4, "skyboxCachedSky", doc.settings.count("skyboxCachedSky") ? doc.settings["skyboxCachedSky"] : "1");
    WriteField(out, 4, "skyboxSunFollowsLight", doc.settings.count("skyboxSunFollowsLight") ? doc.settings["skyboxSunFollowsLight"] : "0");
    WriteField(out, 4, "skyboxVolumetric", doc.settings.count("skyboxVolumetric") ? doc.settings["skyboxVolumetric"] : "0");
    WriteField(out, 4, "skyboxVolumetricIntensity", doc.settings.count("skyboxVolumetricIntensity") ? doc.settings["skyboxVolumetricIntensity"] : "1");
    WriteField(out, 4, "skyboxVolumetricSteps", doc.settings.count("skyboxVolumetricSteps") ? doc.settings["skyboxVolumetricSteps"] : "32");
    WriteField(out, 4, "skyboxVolumetricScattering", doc.settings.count("skyboxVolumetricScattering") ? doc.settings["skyboxVolumetricScattering"] : "0.6");
    WriteField(out, 4, "skyboxVolumetricDensity", doc.settings.count("skyboxVolumetricDensity") ? doc.settings["skyboxVolumetricDensity"] : "0.05");
    WriteField(out, 4, "skyboxVolumetricMaxDistance", doc.settings.count("skyboxVolumetricMaxDistance") ? doc.settings["skyboxVolumetricMaxDistance"] : "60");
    WriteField(out, 4, "skyboxVolumetricHalfRes", doc.settings.count("skyboxVolumetricHalfRes") ? doc.settings["skyboxVolumetricHalfRes"] : "1");
    WriteField(out, 4, "skyboxWeatherEnabled", doc.settings.count("skyboxWeatherEnabled") ? doc.settings["skyboxWeatherEnabled"] : "0");
    WriteField(out, 4, "skyboxWeatherMode", doc.settings.count("skyboxWeatherMode") ? doc.settings["skyboxWeatherMode"] : "0");
    WriteField(out, 4, "skyboxWeatherIntensity", doc.settings.count("skyboxWeatherIntensity") ? doc.settings["skyboxWeatherIntensity"] : "1");
    WriteField(out, 4, "skyboxWeatherWindSpeed", doc.settings.count("skyboxWeatherWindSpeed") ? doc.settings["skyboxWeatherWindSpeed"] : "1");
    WriteField(out, 4, "skyboxWeatherRainOpacity", doc.settings.count("skyboxWeatherRainOpacity") ? doc.settings["skyboxWeatherRainOpacity"] : "0.55");
    WriteField(out, 4, "skyboxWeatherRainSpeed", doc.settings.count("skyboxWeatherRainSpeed") ? doc.settings["skyboxWeatherRainSpeed"] : "1.6");
    WriteField(out, 4, "skyboxWeatherRainScale", doc.settings.count("skyboxWeatherRainScale") ? doc.settings["skyboxWeatherRainScale"] : "1");
    WriteField(out, 4, "skyboxWeatherLightningInterval", doc.settings.count("skyboxWeatherLightningInterval") ? doc.settings["skyboxWeatherLightningInterval"] : "11");
    WriteField(out, 4, "skyboxWeatherLightningIntensity", doc.settings.count("skyboxWeatherLightningIntensity") ? doc.settings["skyboxWeatherLightningIntensity"] : "2.5");
    WriteField(out, 4, "skyboxWeatherAmbienceVolume", doc.settings.count("skyboxWeatherAmbienceVolume") ? doc.settings["skyboxWeatherAmbienceVolume"] : "0.7");
    WriteField(out, 4, "skyboxWeatherHeavyThreshold", doc.settings.count("skyboxWeatherHeavyThreshold") ? doc.settings["skyboxWeatherHeavyThreshold"] : "0.65");
    WriteField(out, 4, "skyboxWeatherClearSound", doc.settings.count("skyboxWeatherClearSound") ? doc.settings["skyboxWeatherClearSound"] : "", true);
    WriteField(out, 4, "skyboxWeatherOvercastSound", doc.settings.count("skyboxWeatherOvercastSound") ? doc.settings["skyboxWeatherOvercastSound"] : "", true);
    WriteField(out, 4, "skyboxWeatherRainSound", doc.settings.count("skyboxWeatherRainSound") ? doc.settings["skyboxWeatherRainSound"] : "", true);
    WriteField(out, 4, "skyboxWeatherHeavyRainSound", doc.settings.count("skyboxWeatherHeavyRainSound") ? doc.settings["skyboxWeatherHeavyRainSound"] : "", true);
    WriteField(out, 4, "skyboxWeatherThunderSound", doc.settings.count("skyboxWeatherThunderSound") ? doc.settings["skyboxWeatherThunderSound"] : "", true);
    WriteField(out, 4, "skyboxCloudHorizonFade", doc.settings.count("skyboxCloudHorizonFade") ? doc.settings["skyboxCloudHorizonFade"] : "0.16");
    WriteField(out, 4, "skyboxSkyResolution", doc.settings.count("skyboxSkyResolution") ? doc.settings["skyboxSkyResolution"] : "256");
    WriteField(out, 4, "skyboxSkyRefreshInterval", doc.settings.count("skyboxSkyRefreshInterval") ? doc.settings["skyboxSkyRefreshInterval"] : "0.1");
    WriteField(out, 4, "skyboxCloudMode", doc.settings.count("skyboxCloudMode") ? doc.settings["skyboxCloudMode"] : "0");
    WriteField(out, 4, "skyboxCloudCoverage", doc.settings.count("skyboxCloudCoverage") ? doc.settings["skyboxCloudCoverage"] : "0.45");
    WriteField(out, 4, "skyboxCloudDensity", doc.settings.count("skyboxCloudDensity") ? doc.settings["skyboxCloudDensity"] : "1");
    WriteField(out, 4, "skyboxCloudSteps", doc.settings.count("skyboxCloudSteps") ? doc.settings["skyboxCloudSteps"] : "24");
    WriteField(out, 4, "skyboxCloudLightSteps", doc.settings.count("skyboxCloudLightSteps") ? doc.settings["skyboxCloudLightSteps"] : "4");
    WriteField(out, 4, "skyboxCloudBottom", doc.settings.count("skyboxCloudBottom") ? doc.settings["skyboxCloudBottom"] : "900");
    WriteField(out, 4, "skyboxCloudTop", doc.settings.count("skyboxCloudTop") ? doc.settings["skyboxCloudTop"] : "2400");
    WriteField(out, 4, "skyboxCloudWind", doc.settings.count("skyboxCloudWind") ? doc.settings["skyboxCloudWind"] : "1");
    WriteField(out, 4, "skyboxCloudDarkness", doc.settings.count("skyboxCloudDarkness") ? doc.settings["skyboxCloudDarkness"] : "0");
    WriteField(out, 4, "skyboxVolumetricMaxIntensity", doc.settings.count("skyboxVolumetricMaxIntensity") ? doc.settings["skyboxVolumetricMaxIntensity"] : "2");
    WriteField(out, 4, "skyboxVolumetricDenoise", doc.settings.count("skyboxVolumetricDenoise") ? doc.settings["skyboxVolumetricDenoise"] : "1");
    WriteField(out, 4, "skyboxVolumetricDenoiseRadius", doc.settings.count("skyboxVolumetricDenoiseRadius") ? doc.settings["skyboxVolumetricDenoiseRadius"] : "3");
    WriteField(out, 4, "skyboxSsr", doc.settings.count("skyboxSsr") ? doc.settings["skyboxSsr"] : "0");
    WriteField(out, 4, "skyboxSsrIntensity", doc.settings.count("skyboxSsrIntensity") ? doc.settings["skyboxSsrIntensity"] : "1");
    WriteField(out, 4, "skyboxSsrSteps", doc.settings.count("skyboxSsrSteps") ? doc.settings["skyboxSsrSteps"] : "48");
    WriteField(out, 4, "skyboxSsrRefineSteps", doc.settings.count("skyboxSsrRefineSteps") ? doc.settings["skyboxSsrRefineSteps"] : "6");
    WriteField(out, 4, "skyboxSsrMaxDistance", doc.settings.count("skyboxSsrMaxDistance") ? doc.settings["skyboxSsrMaxDistance"] : "40");
    WriteField(out, 4, "skyboxSsrThickness", doc.settings.count("skyboxSsrThickness") ? doc.settings["skyboxSsrThickness"] : "0.5");
    WriteField(out, 4, "skyboxSsrMaxRoughness", doc.settings.count("skyboxSsrMaxRoughness") ? doc.settings["skyboxSsrMaxRoughness"] : "0.6");
    WriteField(out, 4, "skyboxSsrHalfRes", doc.settings.count("skyboxSsrHalfRes") ? doc.settings["skyboxSsrHalfRes"] : "1");
    WriteField(out, 4, "skyboxGi", doc.settings.count("skyboxGi") ? doc.settings["skyboxGi"] : "0");
    WriteField(out, 4, "skyboxGiIntensity", doc.settings.count("skyboxGiIntensity") ? doc.settings["skyboxGiIntensity"] : "1");
    WriteField(out, 4, "skyboxGiResolution", doc.settings.count("skyboxGiResolution") ? doc.settings["skyboxGiResolution"] : "128");
    WriteField(out, 4, "skyboxGiMaxExtent", doc.settings.count("skyboxGiMaxExtent") ? doc.settings["skyboxGiMaxExtent"] : "60");
    WriteField(out, 4, "skyboxGiConeSteps", doc.settings.count("skyboxGiConeSteps") ? doc.settings["skyboxGiConeSteps"] : "24");
    WriteField(out, 4, "skyboxGiBounces", doc.settings.count("skyboxGiBounces") ? doc.settings["skyboxGiBounces"] : "2");
    WriteField(out, 4, "skyboxSunTexture", doc.settings.count("skyboxSunTexture") ? doc.settings["skyboxSunTexture"] : "", true);
    WriteField(out, 4, "skyboxMoonTexture", doc.settings.count("skyboxMoonTexture") ? doc.settings["skyboxMoonTexture"] : "", true);
    WriteField(out, 4, "skyboxScrollTexture", doc.settings.count("skyboxScrollTexture") ? doc.settings["skyboxScrollTexture"] : "", true);
    WriteField(out, 4, "skyboxScrollRepeat", doc.settings.count("skyboxScrollRepeat") ? doc.settings["skyboxScrollRepeat"] : "1,1");
    WriteField(out, 4, "skyboxScrollLookSensitivity", doc.settings.count("skyboxScrollLookSensitivity") ? doc.settings["skyboxScrollLookSensitivity"] : "0");
    WriteField(out, 4, "skyboxScrollVerticalInfluence", doc.settings.count("skyboxScrollVerticalInfluence") ? doc.settings["skyboxScrollVerticalInfluence"] : "0");
    WriteField(out, 4, "skyboxEnvironmentReflections", doc.settings.count("skyboxEnvironmentReflections") ? doc.settings["skyboxEnvironmentReflections"] : "0");
    WriteField(out, 4, "skyboxEnvironmentReflectionIntensity", doc.settings.count("skyboxEnvironmentReflectionIntensity") ? doc.settings["skyboxEnvironmentReflectionIntensity"] : "0.5");
    WriteField(out, 4, "skyboxReflectionDistanceFadeStart", doc.settings.count("skyboxReflectionDistanceFadeStart") ? doc.settings["skyboxReflectionDistanceFadeStart"] : "4");
    WriteField(out, 4, "skyboxReflectionDistanceFadeEnd", doc.settings.count("skyboxReflectionDistanceFadeEnd") ? doc.settings["skyboxReflectionDistanceFadeEnd"] : "24");
    WriteField(out, 4, "fogEnabled", doc.settings.count("fogEnabled") ? doc.settings["fogEnabled"] : "0");
    WriteField(out, 4, "fogMode", doc.settings.count("fogMode") ? doc.settings["fogMode"] : "0");
    WriteField(out, 4, "fogColor", doc.settings.count("fogColor") ? doc.settings["fogColor"] : "0.65,0.72,0.78");
    WriteField(out, 4, "fogStart", doc.settings.count("fogStart") ? doc.settings["fogStart"] : "20");
    WriteField(out, 4, "fogEnd", doc.settings.count("fogEnd") ? doc.settings["fogEnd"] : "120");
    WriteField(out, 4, "fogDensity", doc.settings.count("fogDensity") ? doc.settings["fogDensity"] : "0.015");
    WriteField(out, 4, "fogHeight", doc.settings.count("fogHeight") ? doc.settings["fogHeight"] : "0");
    WriteField(out, 4, "fogHeightFalloff", doc.settings.count("fogHeightFalloff") ? doc.settings["fogHeightFalloff"] : "0");
    out << "}\n\n";

    const auto& schemas = GetComponentSchemas();
    for (size_t objectIndex = 0; objectIndex < doc.objects.size(); ++objectIndex) {
        const FieldMap& fields = doc.objects[objectIndex].fields;
        const auto findOr = [&](const char* key, const std::string& fallback) -> std::string {
            auto it = fields.find(key);
            return it != fields.end() ? it->second : fallback;
        };

        out << "MODU_GAMEOBJECT = (id=" << findOr("id", "0")
            << ", parentId=" << findOr("parentId", "-1")
            << ", type=" << findOr("type", "21")
            << ", name=" << QuoteString(findOr("name", "GameObject"))
            << ", enabled=" << findOr("enabled", "1")
            << ") {\n";

        if (IsTruthy(fields, "invariable")) {
            WriteField(out, 4, "invariable", "1");
        }

        WriteField(out, 4, "position", findOr("position", "0,0,0"));
        WriteField(out, 4, "rotation", findOr("rotation", "0,0,0"));
        WriteField(out, 4, "scale", findOr("scale", "1,1,1"));
        WriteField(out, 4, "children", findOr("children", ""));

        if (fields.count("layer") > 0 && TrimCopy(findOr("layer", "0")) != "0") {
            WriteField(out, 4, "layer", fields.at("layer"));
        }
        if (fields.count("tag") > 0 && TrimCopy(findOr("tag", "Untagged")) != "Untagged") {
            WriteField(out, 4, "tag", fields.at("tag"), true);
        }
        if (fields.count("editorIconTint") > 0 && TrimCopy(findOr("editorIconTint", "1,1,1,1")) != "1,1,1,1") {
            WriteField(out, 4, "editorIconTint", fields.at("editorIconTint"));
        }
        if (fields.count("editorIcon") > 0 && !TrimCopy(fields.at("editorIcon")).empty()) {
            WriteField(out, 4, "editorIcon", fields.at("editorIcon"), true);
        }
        if (IsTruthy(fields, "editorIconGizmo")) {
            WriteField(out, 4, "editorIconGizmo", "1");
        }
        if (fields.count("nextInspectorScriptId") > 0) {
            WriteField(out, 4, "nextInspectorScriptId", fields.at("nextInspectorScriptId"));
        }
        if (fields.count("componentOrder") > 0 && !TrimCopy(fields.at("componentOrder")).empty()) {
            WriteField(out, 4, "componentOrder", fields.at("componentOrder"), true);
        }
        out << "\n";

        for (const ComponentSchema& schema : schemas) {
            if (schema.nodeName == "Material") {
                if (!ShouldEmitMaterialNode(fields)) continue;
            } else {
                if (!schema.presenceFlagKey.empty() && !IsTruthy(fields, schema.presenceFlagKey)) {
                    continue;
                }
            }
            WriteComponentNode(out, fields, schema, 4);
            out << "\n";
        }

        const int scriptCount = ResolveScriptCount(fields);
        if (scriptCount > 0) {
            WriteIndent(out, 4);
            out << "NODE_Scripts {\n";
            for (int scriptIndex = 0; scriptIndex < scriptCount; ++scriptIndex) {
                WriteScriptNode(out, fields, scriptIndex, 8);
                if (scriptIndex + 1 < scriptCount) {
                    out << "\n";
                }
            }
            WriteIndent(out, 4);
            out << "}\n";
        }

        out << "}\n";
        if (objectIndex + 1 < doc.objects.size()) {
            out << "\n";
        }
    }

    return true;
}

bool TryParseInt(const std::string& value, int& outValue) {
    try {
        outValue = std::stoi(value);
        return true;
    } catch (...) {
        return false;
    }
}

int ReadCountOrZero(const FieldMap& fields, const std::string& key) {
    auto it = fields.find(key);
    int value = 0;
    if (it != fields.end() && TryParseInt(it->second, value)) {
        return std::max(0, value);
    }
    return 0;
}

ScriptLanguage ParseScriptLanguageOrDefault(const std::string& value) {
    int languageValue = 0;
    if (!TryParseInt(value, languageValue)) {
        return ScriptLanguage::Cpp;
    }
    if (languageValue == static_cast<int>(ScriptLanguage::CSharp)) {
        return ScriptLanguage::CSharp;
    }
    if (languageValue == static_cast<int>(ScriptLanguage::C)) {
        return ScriptLanguage::C;
    }
    return ScriptLanguage::Cpp;
}

void ReapplyModularScripts(const FlatSceneDocument& doc, std::vector<SceneObject>& objects) {
    std::unordered_map<int, SceneObject*> objectsById;
    objectsById.reserve(objects.size());
    for (SceneObject& obj : objects) {
        objectsById[obj.id] = &obj;
    }

    for (const FlatSceneObjectData& objectData : doc.objects) {
        const FieldMap& fields = objectData.fields;
        auto idIt = fields.find("id");
        int objectId = 0;
        if (idIt == fields.end() || !TryParseInt(idIt->second, objectId)) {
            continue;
        }

        auto objectIt = objectsById.find(objectId);
        if (objectIt == objectsById.end() || objectIt->second == nullptr) {
            continue;
        }

        SceneObject& obj = *objectIt->second;
        const int scriptCount = std::max(ReadCountOrZero(fields, "scripts"),
                                         ReadCountOrZero(fields, "scriptCount"));
        obj.scripts.clear();
        obj.scripts.resize(scriptCount);

        for (int scriptIndex = 0; scriptIndex < scriptCount; ++scriptIndex) {
            ScriptComponent script;
            const std::string prefix = "script" + std::to_string(scriptIndex) + "_";

            int inspectorId = 0;
            auto idFieldIt = fields.find(prefix + "id");
            if (idFieldIt != fields.end() && TryParseInt(idFieldIt->second, inspectorId)) {
                script.inspectorId = std::max(0, inspectorId);
            }

            auto pathIt = fields.find(prefix + "path");
            if (pathIt != fields.end()) {
                script.path = pathIt->second;
            }

            auto typeIt = fields.find(prefix + "type");
            if (typeIt != fields.end()) {
                script.managedType = typeIt->second;
            }

            auto enabledIt = fields.find(prefix + "enabled");
            if (enabledIt != fields.end()) {
                script.enabled = IsTruthy(fields, prefix + "enabled");
            }

            auto langIt = fields.find(prefix + "lang");
            if (langIt != fields.end()) {
                script.language = ParseScriptLanguageOrDefault(langIt->second);
            }

            const int settingCount = std::max(ReadCountOrZero(fields, prefix + "settings"),
                                              ReadCountOrZero(fields, prefix + "settingCount"));
            script.settings.resize(settingCount);
            for (int settingIndex = 0; settingIndex < settingCount; ++settingIndex) {
                auto settingIt = fields.find(prefix + "setting" + std::to_string(settingIndex));
                if (settingIt == fields.end()) {
                    continue;
                }

                const std::string& rawSetting = settingIt->second;
                const size_t separator = rawSetting.find(':');
                if (separator == std::string::npos) {
                    script.settings[settingIndex].value = rawSetting;
                    continue;
                }

                script.settings[settingIndex].key = rawSetting.substr(0, separator);
                script.settings[settingIndex].value = rawSetting.substr(separator + 1);
            }

            obj.scripts[scriptIndex] = std::move(script);
        }

        EnsureInspectorComponentMetadata(obj);
    }
}

void EmitFieldLine(std::ostream& out,
                   const FieldMap& fields,
                   const std::string& key,
                   std::set<std::string>& emitted) {
    auto it = fields.find(key);
    if (it == fields.end()) return;
    if (!emitted.insert(key).second) return;
    out << key << "=" << it->second << "\n";
}

void EmitAnimationTrackGroup(std::ostream& out,
                             const FieldMap& fields,
                             int trackIndex,
                             std::set<std::string>& emitted) {
    const std::string prefix = "animTrack" + std::to_string(trackIndex) + "_";
    EmitFieldLine(out, fields, prefix + "enabled", emitted);
    EmitFieldLine(out, fields, prefix + "path", emitted);
    EmitFieldLine(out, fields, prefix + "label", emitted);
    EmitFieldLine(out, fields, prefix + "default", emitted);
    EmitFieldLine(out, fields, prefix + "keyCount", emitted);

    const int keyCount = ReadCountOrZero(fields, prefix + "keyCount");
    for (int keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
        const std::string keyPrefix = prefix + "key" + std::to_string(keyIndex) + "_";
        EmitFieldLine(out, fields, keyPrefix + "time", emitted);
        EmitFieldLine(out, fields, keyPrefix + "value", emitted);
        EmitFieldLine(out, fields, keyPrefix + "interp", emitted);
        EmitFieldLine(out, fields, keyPrefix + "curve", emitted);
        EmitFieldLine(out, fields, keyPrefix + "in", emitted);
        EmitFieldLine(out, fields, keyPrefix + "out", emitted);
    }
}

void EmitScriptGroup(std::ostream& out,
                     const FieldMap& fields,
                     int scriptIndex,
                     std::set<std::string>& emitted) {
    const std::string prefix = "script" + std::to_string(scriptIndex) + "_";
    EmitFieldLine(out, fields, prefix + "id", emitted);
    EmitFieldLine(out, fields, prefix + "path", emitted);
    EmitFieldLine(out, fields, prefix + "lang", emitted);
    EmitFieldLine(out, fields, prefix + "type", emitted);
    EmitFieldLine(out, fields, prefix + "enabled", emitted);
    EmitFieldLine(out, fields, prefix + "settings", emitted);
    EmitFieldLine(out, fields, prefix + "settingCount", emitted);

    const int settingCount = std::max(ReadCountOrZero(fields, prefix + "settings"),
                                      ReadCountOrZero(fields, prefix + "settingCount"));
    for (int settingIndex = 0; settingIndex < settingCount; ++settingIndex) {
        EmitFieldLine(out, fields, prefix + "setting" + std::to_string(settingIndex), emitted);
    }
}

void EmitFlatSceneDocument(std::ostream& out, const FlatSceneDocument& doc) {
    out << "# Scene File\n";
    {
        std::set<std::string> emitted;
        const std::vector<std::string> orderedSettings = {
            "version",
            "nextId",
            "timeOfDay",
            "skyboxMode",
            "skyboxCachedSky",
            "skyboxSunFollowsLight",
            "skyboxVolumetric",
            "skyboxVolumetricIntensity",
            "skyboxVolumetricSteps",
            "skyboxVolumetricScattering",
            "skyboxVolumetricDensity",
            "skyboxVolumetricMaxDistance",
            "skyboxVolumetricHalfRes",
            "skyboxWeatherEnabled",
            "skyboxWeatherMode",
            "skyboxWeatherIntensity",
            "skyboxWeatherWindSpeed",
            "skyboxWeatherRainOpacity",
            "skyboxWeatherRainSpeed",
            "skyboxWeatherRainScale",
            "skyboxWeatherLightningInterval",
            "skyboxWeatherLightningIntensity",
            "skyboxWeatherAmbienceVolume",
            "skyboxWeatherHeavyThreshold",
            "skyboxWeatherClearSound",
            "skyboxWeatherOvercastSound",
            "skyboxWeatherRainSound",
            "skyboxWeatherHeavyRainSound",
            "skyboxWeatherThunderSound",
            "skyboxCloudHorizonFade",
            "skyboxSkyResolution",
            "skyboxSkyRefreshInterval",
            "skyboxCloudMode",
            "skyboxCloudCoverage",
            "skyboxCloudDensity",
            "skyboxCloudSteps",
            "skyboxCloudLightSteps",
            "skyboxCloudBottom",
            "skyboxCloudTop",
            "skyboxCloudWind",
            "skyboxCloudDarkness",
            "skyboxVolumetricMaxIntensity",
            "skyboxVolumetricDenoise",
            "skyboxVolumetricDenoiseRadius",
            "skyboxSsr",
            "skyboxSsrIntensity",
            "skyboxSsrSteps",
            "skyboxSsrRefineSteps",
            "skyboxSsrMaxDistance",
            "skyboxSsrThickness",
            "skyboxSsrMaxRoughness",
            "skyboxSsrHalfRes",
            "skyboxGi",
            "skyboxGiIntensity",
            "skyboxGiResolution",
            "skyboxGiMaxExtent",
            "skyboxGiConeSteps",
            "skyboxGiBounces",
            "skyboxSunTexture",
            "skyboxMoonTexture",
            "skyboxScrollTexture",
            "skyboxScrollRepeat",
            "skyboxScrollLookSensitivity",
            "skyboxScrollVerticalInfluence",
            "skyboxEnvironmentReflections",
            "skyboxEnvironmentReflectionIntensity",
            "skyboxReflectionDistanceFadeStart",
            "skyboxReflectionDistanceFadeEnd",
            "fogEnabled",
            "fogMode",
            "fogColor",
            "fogStart",
            "fogEnd",
            "fogDensity",
            "fogHeight",
            "fogHeightFalloff",
            "objectCount",
        };
        for (const std::string& key : orderedSettings) {
            EmitFieldLine(out, doc.settings, key, emitted);
        }
        for (const auto& [key, value] : doc.settings) {
            (void)value;
            EmitFieldLine(out, doc.settings, key, emitted);
        }
    }
    out << "\n";

    for (const FlatSceneObjectData& object : doc.objects) {
        out << "[Object]\n";
        std::set<std::string> emitted;
        const std::vector<std::string> fixedKeys = {
            "id", "name", "type", "enabled", "invariable", "layer", "tag",
            "editorIconTint", "editorIcon", "editorIconGizmo",
            "hasRenderer", "renderType", "faceCamera", "hasLight", "hasLight2D", "hasReflectionCast",
            "hasCamera", "hasPostFX", "hasUI", "hasShadowCaster2D", "uiType",
            "parentId", "position", "rotation", "scale",
            "hasRigidbody", "rbEnabled", "rbMass", "rbUseCustomCenterOfMass", "rbCenterOfMass", "rbUseGravity", "rbKinematic",
            "rbLinearDamping", "rbAngularDamping", "rbInterpolation", "rbMaxExtrapolation",
            "rbLockRotX", "rbLockRotY", "rbLockRotZ",
            "hasRigidbody2D", "rb2dEnabled", "rb2dUseGravity", "rb2dLockRotation", "rb2dGravityScale", "rb2dLinearDamping", "rb2dVelocity",
            "hasCollider2D", "collider2dEnabled", "collider2dType", "collider2dBox", "collider2dOffset", "collider2dClosed",
            "collider2dEdgeThickness", "collider2dPoints",
            "hasParallaxLayer2D", "parallax2dEnabled", "parallax2dOrder", "parallax2dFactor", "parallax2dRepeatX",
            "parallax2dRepeatY", "parallax2dDisableCulling", "parallax2dSpacing",
            "hasCameraFollow2D", "cameraFollow2dEnabled", "cameraFollow2dTarget", "cameraFollow2dOffset", "cameraFollow2dSmoothTime",
            "hasCollider", "colliderEnabled", "colliderType", "colliderBox", "colliderOffset", "colliderConvex", "colliderIsTrigger",
            "colliderStaticFriction", "colliderDynamicFriction", "colliderRestitution",
            "hasPlayerController", "pcEnabled", "pcMoveSpeed", "pcRunSpeed", "pcLookSensitivity", "pcGroundAcceleration",
            "pcAirAcceleration", "pcBraking", "pcMinSurfaceControl", "pcSlideGravity", "pcPlatformCarry", "pcHeight", "pcRadius", "pcJumpStrength",
            "pcVmEnabled", "pcVmBobFrequency", "pcVmBobVertical", "pcVmBobHorizontal",
            "pcVmBobRoll", "pcVmRunMultiplier", "pcVmLookSway", "pcVmLookSwayStiffness",
            "pcVmLookSwayDamping", "pcVmLookSwayMax", "pcVmStrafeRoll", "pcVmTurnRoll",
            "pcVmRollSmoothing", "pcVmIdleAmount", "pcVmIdleFrequency", "pcVmLandingDip",
            "pcVmLandingStiffness", "pcVmLandingDamping", "pcVmAttachedEnabled", "pcVmAttachedSway",
            "pcVmAttachedStiffness", "pcVmAttachedDamping", "pcVmAttachedMax",
            "pcVmJumpCrouchDip", "pcFeelLookSmoothing", "pcFeelChargedJump", "pcFeelJumpChargeTime", "pcFeelJumpMinScale",
            "pcVmGaitBlend", "pcVmRunLean", "pcVmIdleBlend",
            "pcAudioEnabled", "pcAudioJumpClip", "pcAudioLandClip", "pcAudioVolume", "pcAudioRunVolumeScale", "pcAudioLandVolumeScale", "pcAudioPitchVariance", "pcAudioStepCount",
            "hasAudioSource", "audioEnabled", "audioClip", "audioVolume", "audioLoop", "audioPlayOnStart", "audioSpatial",
            "audioSpatialBlend", "audioMinDistance", "audioMaxDistance", "audioRolloffMode", "audioRolloff", "audioCustomMidDistance",
            "audioCustomMidGain", "audioCustomEndGain",
            "hasAudioFX", "afxEnabled", "afxGlobal", "afxChain",
            "hasVideoPlayer", "videoEnabled", "videoPath", "videoPlayOnAwake", "videoLoop", "videoPlaybackSpeed",
            "videoFlipX", "videoFlipY", "videoPlayAudioFromVideo", "videoRouteAudioToSource", "videoOutputAudioSourceObjectId", "videoAudioVolume",
            "videoAudioMuted", "videoSyncAudioToVideo", "videoAudioSyncTolerance",
            "hasParticleSystem2D", "ps2dEnabled", "ps2dLooping", "ps2dPrewarm", "ps2dPlayOnAwake", "ps2dAutoRandomSeed",
            "ps2dRandomSeed", "ps2dStartDelay", "ps2dStartLifetime", "ps2dStartSpeed", "ps2dStartSize", "ps2dStartRotation",
            "ps2dStartColor", "ps2dGravity", "ps2dSimulationSpeed", "ps2dMaxParticles", "ps2dEmissionRate",
            "ps2dBurstCount", "ps2dBurstTime", "ps2dBurstLoop", "ps2dShape", "ps2dShapeRadius", "ps2dShapeBox",
            "ps2dVelocityOverLifetimeEnabled", "ps2dVelocityOverLifetime", "ps2dColorOverLifetimeEnabled",
            "ps2dColorOverLifetime", "ps2dSizeOverLifetimeEnabled", "ps2dSizeOverLifetime",
            "ps2dRotationOverLifetimeEnabled", "ps2dRotationOverLifetime", "ps2dNoiseEnabled", "ps2dNoiseStrength",
            "ps2dNoiseFrequency", "ps2dTexture", "ps2dMaterial", "ps2dReceiveLighting2D", "ps2dUnlitLighting2D",
            "ps2dEmissiveLighting2D",
            "hasReverbZone", "reverbEnabled", "reverbPreset", "reverbShape", "reverbBox", "reverbRadius", "reverbBlend",
            "reverbMinDistance", "reverbMaxDistance", "reverbRoom", "reverbRoomHF", "reverbRoomLF", "reverbDecayTime", "reverbDecayHFRatio",
            "reverbReflections", "reverbReflectionsDelay", "reverbReverb", "reverbReverbDelay", "reverbHFReference", "reverbLFReference",
            "reverbRoomRolloffFactor", "reverbDiffusion", "reverbDensity",
            "hasGroundBakedType", "groundBakedEnabled", "groundBakedInclude", "groundBakedAreaCost",
            "hasObsticleObject", "obsticleEnabled", "obsticleCarve", "obsticlePadding",
            "hasMapRoot", "mapRootEnabled", "mapRootId", "mapRootStartSector", "mapRootActiveSector",
            "mapRootVisibilityMode", "mapRootNotes",
            "hasMapSector", "mapSectorEnabled", "mapSectorId", "mapSectorGraphPos", "mapSectorColor",
            "mapSectorUseCustomBounds", "mapSectorBoundsCenter", "mapSectorBoundsSize",
            "mapSectorStreamingTag", "mapSectorNotes", "mapSectorEstimatedMemoryMB",
            "hasMapTransition", "mapTransitionEnabled", "mapTransitionId", "mapTransitionSourceSector",
            "mapTransitionDestSector", "mapTransitionSourcePortal", "mapTransitionDestPortal",
            "mapTransitionBidirectional", "mapTransitionLocked", "mapTransitionCondition",
            "mapTransitionKind", "mapTransitionEditorLabel", "mapTransitionHasEntry",
            "mapTransitionEntryPos", "mapTransitionEntryYaw",
            "hasMapPortal", "mapPortalEnabled", "mapPortalId", "mapPortalTransition", "mapPortalSector",
            "mapPortalOpeningSize",
            "hasMapMesh", "mapMeshEnabled", "mapMeshGridSize", "mapMeshSnapToGrid",
            "mapMeshVertexSnapping", "mapMeshSurfaceSnapping", "mapMeshAutoCollision",
            "hasAIAgent", "aiAgentEnabled", "aiAgentUseTargetObject", "aiAgentTargetId", "aiAgentDestination", "aiAgentSpeed",
            "aiAgentStoppingDistance", "aiAgentRepathInterval", "aiAgentAutoRepath", "aiAgentAlignToPath", "aiAgentDebugDrawPath",
            "aiAgentTurnSpeed", "aiAgentAvoidancePadding",
            "hasOffMeshLink", "offMeshLinkEnabled", "offMeshLinkStart", "offMeshLinkEnd", "offMeshLinkBidirectional", "offMeshLinkCostOverride",
            "hasRig25DRoot", "rig25dRootEnabled",
            "hasRig25DNode", "rig25dNodeEnabled", "rig25dNodeId", "rig25dNodeName",
            "hasAnimation", "animEnabled", "animClipAsset",
            "hasVideoPlayer", "videoEnabled", "videoPath", "videoPlayOnAwake", "videoLoop", "videoPlaybackSpeed",
            "videoFlipX", "videoFlipY", "videoPlayAudioFromVideo", "videoRouteAudioToSource", "videoOutputAudioSourceObjectId", "videoAudioVolume",
            "videoAudioMuted", "videoSyncAudioToVideo", "videoAudioSyncTolerance",
            "animClipCount",
        };
        for (const std::string& key : fixedKeys) {
            EmitFieldLine(out, object.fields, key, emitted);
        }

        const int clipCount = ReadCountOrZero(object.fields, "animClipCount");
        for (int clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
            const std::string prefix = "animClip" + std::to_string(clipIndex) + "_";
            EmitFieldLine(out, object.fields, prefix + "name", emitted);
            EmitFieldLine(out, object.fields, prefix + "asset", emitted);
        }

        const std::vector<std::string> animationFixedKeys = {
            "animActiveClipIndex", "animClipLength", "animPlaySpeed", "animLoop", "animPlayOnAwake", "animApplyOnScrub",
            "animKeyCount",
        };
        for (const std::string& key : animationFixedKeys) {
            EmitFieldLine(out, object.fields, key, emitted);
        }

        const int keyCount = ReadCountOrZero(object.fields, "animKeyCount");
        for (int keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
            const std::string prefix = "animKey" + std::to_string(keyIndex) + "_";
            EmitFieldLine(out, object.fields, prefix + "time", emitted);
            EmitFieldLine(out, object.fields, prefix + "pos", emitted);
            EmitFieldLine(out, object.fields, prefix + "rot", emitted);
            EmitFieldLine(out, object.fields, prefix + "scale", emitted);
            EmitFieldLine(out, object.fields, prefix + "interp", emitted);
            EmitFieldLine(out, object.fields, prefix + "curve", emitted);
            EmitFieldLine(out, object.fields, prefix + "in", emitted);
            EmitFieldLine(out, object.fields, prefix + "out", emitted);
        }

        EmitFieldLine(out, object.fields, "animEventCount", emitted);
        const int eventCount = ReadCountOrZero(object.fields, "animEventCount");
        for (int eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
            const std::string prefix = "animEvent" + std::to_string(eventIndex) + "_";
            EmitFieldLine(out, object.fields, prefix + "time", emitted);
            EmitFieldLine(out, object.fields, prefix + "id", emitted);
            EmitFieldLine(out, object.fields, prefix + "payload", emitted);
        }

        EmitFieldLine(out, object.fields, "animTrackCount", emitted);
        const int trackCount = ReadCountOrZero(object.fields, "animTrackCount");
        for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            EmitAnimationTrackGroup(out, object.fields, trackIndex, emitted);
        }

        const std::vector<std::string> postAnimationKeys = {
            "hasSkeletalAnimation", "skelEnabled", "skelUseGpu", "skelAllowCpuFallback", "skelUseAnimation",
            "skelClipIndex", "skelPlaySpeed", "skelLoop", "skelMaxBones",
            "materialColor", "materialAlpha", "materialAmbient", "materialSpecular", "materialShininess", "materialNormalMapIntensity", "materialTextureMix",
            "materialUvTiling", "materialUvOffset", "materialScrollSpeed", "materialScrollDirection",
            "materialTextureFilter",
            "materialPbrEnabled", "materialMetallic", "materialRoughness", "materialOcclusionStrength",
            "materialEmissionColor", "materialEmissionStrength", "materialAlphaCutoff",
            "materialPath", "albedoTex", "overlayTex", "normalMap",
            "materialHeightScale", "materialParallaxSteps", "materialParallaxInvertHeight",
            "metallicRoughnessMap", "metallicMap", "roughnessMap", "opacityMap",
            "emissiveMap", "occlusionMap", "heightMap",
            "shaderPack", "vertexShader", "fragmentShader",
            "useOverlay", "additionalMaterialCount"
        };
        for (const std::string& key : postAnimationKeys) {
            EmitFieldLine(out, object.fields, key, emitted);
        }

        const int additionalMaterialCount = ReadCountOrZero(object.fields, "additionalMaterialCount");
        for (int materialIndex = 0; materialIndex < additionalMaterialCount; ++materialIndex) {
            EmitFieldLine(out, object.fields, "additionalMaterial" + std::to_string(materialIndex), emitted);
        }

        const std::vector<std::string> postMaterialKeys = {
            "nextInspectorScriptId", "componentOrder", "scripts", "scriptCount",
            "lightColor", "lightType", "lightIntensity", "lightRange", "lightEdgeFade", "lightInner", "lightOuter",
            "lightSize", "lightCastShadows", "lightSoftShadows", "lightShadowBias", "lightShadowSoftness", "lightShadowResolution", "lightEnabled",
            "reflectionCastEnabled", "reflectionCastUpdateMode", "reflectionCastBox", "reflectionCastBlend", "reflectionCastIntensity", "reflectionCastResolution",
            "light2dEnabled", "light2dType", "light2dColor", "light2dIntensity", "light2dRadius", "light2dInnerRadius",
            "light2dOuterRadius", "light2dFalloffStrength", "light2dInnerSpotAngle", "light2dOuterSpotAngle",
            "light2dBlendStyle", "light2dOrder", "light2dOverlap", "light2dShadowStrength", "light2dVolumetric",
            "light2dCastShadows", "light2dTargetAllLayers", "light2dTargetLayerMask", "light2dNormalQuality",
            "light2dNormalDistance", "light2dUseDistanceExponent", "light2dDistanceExponent", "light2dCookie",
            "light2dCookieScale", "light2dCookieRotation", "light2dFreeformFeather", "light2dFreeformEdgeFalloff",
            "light2dFlickerEnabled", "light2dFlickerSpeed", "light2dFlickerAmount", "light2dFlickerSeed", "light2dShape",
            "shadowCaster2dEnabled", "shadowCaster2dSelfShadow", "shadowCaster2dTargetAllLayers", "shadowCaster2dTargetLayerMask",
            "shadowCaster2dStrength", "shadowCaster2dShape",
            "cameraType", "cameraFov", "cameraNear", "cameraFar", "cameraPostFX", "cameraUse2D", "cameraPixelsPerUnit",
            "cameraProjection", "cameraFovAxis", "cameraOrthoSize", "cameraRenderShadows", "cameraBackground",
            "cameraBackgroundColor", "cameraCullingMask",
            "uiAnchor", "uiPosition", "uiRotation", "uiSize", "uiMaskChildren", "uiSliderValue", "uiSliderMin", "uiSliderMax",
            "uiLabel", "uiColor", "uiInteractable", "uiSliderStyle", "uiButtonStyle", "uiStylePreset", "uiTextScale", "uiTextFont",
            "uiTextWrap", "uiTextHAlign", "uiTextVAlign", "uiTextEffectFlags", "uiTextEffectSpeed", "uiTextEffectIntensity",
            "uiRenderIn3D", "uiRenderTargetSize", "uiRenderTargetFilter", "uiPseudo3DEnabled", "uiPseudo3DUseOffscreen", "uiPseudo3DPanelSize",
            "uiPseudo3DTopLeftOffset", "uiPseudo3DTopRightOffset", "uiPseudo3DBottomRightOffset", "uiPseudo3DBottomLeftOffset",
            "uiPseudo3DPivot", "uiPseudo3DPerspectiveIntensity", "uiPseudo3DSkewAmount", "uiPseudo3DCurvatureAmount",
            "uiPseudo3DAnchorTargetId", "uiPseudo3DDistanceScaling", "uiPseudo3DAdjustPerspectiveDistance", "uiPseudo3DMinDistance",
            "uiPseudo3DMaxDistance", "uiPseudo3DInteractionDistance", "uiPseudo3DDepthSort", "uiPseudo3DAllowInteraction",
            "uiSpriteSheetEnabled", "uiSpriteSheetGrid", "uiSpriteSheetFrame", "uiSpriteSheetFps", "uiSpriteSheetLoop",
            "uiSpriteCustomFramesEnabled", "uiSpriteSourceSize", "uiNineSliceEnabled", "uiNineSliceBorder", "uiNineSliceTileEdges",
            "uiNineSliceTileCenter", "uiReceiveLighting2D", "uiUnlitLighting2D", "uiEmissiveLighting2D",
            "uiSpriteCustomFrames", "uiSpriteCustomFrameNames", "uiSpriteCustomFrameScales",
            "postEnabled", "postVolumeGlobal", "postVolumePriority", "postVolumeWeight", "postBlendWeight", "postVolumeBlendRadius",
            "postHDREnabled", "postToneMapper", "postWhitePoint", "postGamma", "postBloomEnabled", "postBloomThreshold",
            "postBloomSoftKnee", "postBloomIntensity", "postBloomRadius", "postColorAdjustEnabled", "postExposure",
            "postContrast", "postSaturation", "postColorFilter", "postMotionBlurEnabled", "postMotionBlurStrength",
            "postMotionBlurThreshold", "postMotionBlurClamp", "postVignetteEnabled", "postVignetteIntensity",
            "postVignetteSmoothness", "postChromaticEnabled", "postChromaticAmount", "postSharpenEnabled", "postSharpenStrength",
            "postAOEnabled", "postAORadius", "postAOStrength", "postDitherEnabled", "postDitherIntensity",
            "postDitherColorBits", "postDitherDarkAdjustment", "postDitherPixelation", "postDitherSize", "postDitherContrast",
            "postDitherOffset", "postDitherPalette", "postDitherPattern", "postStaticEnabled", "postStaticIntensity",
            "postStaticGrainScale", "postStaticDarkAreaInfluence", "postStaticSpeed", "postStaticMonochrome",
            "postStaticSparkle", "postStaticDistortionEnabled",
            "postStaticDistortionHorizontalJitterAmount", "postStaticDistortionLineDensity", "postStaticDistortionGlitchFrequency",
            "postStaticDistortionStrength", "postLensDistortionEnabled", "postLensDistortionAmount", "postLensDistortionEdgeFalloff",
            "postLensDistortionCenterOffset", "postLensDistortionEdgeVignetteEnabled",
            "postLensDistortionEdgeVignetteIntensity", "postLensDistortionEdgeVignetteRadius",
            "postLensDistortionEdgeVignetteSoftness", "postLensDistortionEdgeVignetteColor",
            "postPixelationEnabled", "postPixelationSize", "postPosterizeEnabled", "postPosterizeLevels",
            "postScanlinesEnabled", "postScanlinesIntensity", "postScanlinesDensity", "postScanlinesSpeed",
            "postVHSOverlayEnabled", "postVHSOverlayOpacity", "postVHSOverlayScanlineStrength",
            "postVHSOverlayTapeNoise", "postVHSOverlayChromaBleed", "postVHSOverlayBottomNoiseBandHeight",
            "postVHSOverlayBottomNoiseBandIntensity", "postVHSOverlayDistortionStrength", "postVHSOverlayAnimationSpeed",
            "postVHSOverlayColorBleed", "postVHSOverlayBanding", "postVHSOverlaySignalMode", "postVHSOverlayDropouts",
            "postWavyEnabled", "postWavyAmplitude", "postWavyFrequency", "postWavySpeed",
            "postWavyVertical",
            "postScope2DEnabled", "postScope2DMode", "postScope2DMinOrder", "postScope2DMaxOrder",
            "meshPath", "meshSourceIndex", "children"
        };
        for (const std::string& key : postMaterialKeys) {
            EmitFieldLine(out, object.fields, key, emitted);
        }

        const int scriptCount = std::max(ReadCountOrZero(object.fields, "scripts"),
                                         ReadCountOrZero(object.fields, "scriptCount"));
        for (int scriptIndex = 0; scriptIndex < scriptCount; ++scriptIndex) {
            EmitScriptGroup(out, object.fields, scriptIndex, emitted);
        }

        for (const auto& [key, value] : object.fields) {
            (void)value;
            EmitFieldLine(out, object.fields, key, emitted);
        }
        out << "\n";
    }
}

void ApplyNodeHeaderToFields(const ComponentSchema& schema, const BlockHeader& header, FieldMap& fields) {
    if (!schema.presenceFlagKey.empty()) {
        fields[schema.presenceFlagKey] = "1";
    }
    if (!schema.enabledLegacyKey.empty()) {
        auto enabledIt = header.primaryParams.find("enabled");
        if (enabledIt != header.primaryParams.end()) {
            fields[schema.enabledLegacyKey] = enabledIt->second;
        }
    }
    for (const auto& [key, value] : header.primaryParams) {
        if (key == "enabled") continue;
        fields[schema.legacyKeyForNode(key)] = value;
    }
    for (const auto& [key, value] : header.secondaryParams) {
        fields[schema.legacyKeyForNode(key)] = value;
    }
}

bool ParseSettingsBlock(LineCursor& cursor, FieldMap& settings) {
    while (cursor.index < cursor.lines.size()) {
        if (IsBlockClose(cursor.lines[cursor.index])) {
            ++cursor.index;
            return true;
        }
        std::string key;
        std::string value;
        if (ParseAssignment(cursor.lines[cursor.index], key, value)) {
            settings[key] = value;
        }
        ++cursor.index;
    }
    return false;
}

bool ParseSettingValueBlock(LineCursor& cursor, std::string& outValue) {
    outValue.clear();
    while (cursor.index < cursor.lines.size()) {
        if (IsBlockClose(cursor.lines[cursor.index])) {
            ++cursor.index;
            return true;
        }
        std::string key;
        std::string value;
        if (ParseAssignment(cursor.lines[cursor.index], key, value) && key == "value") {
            outValue = value;
        }
        ++cursor.index;
    }
    return false;
}

bool ParseScriptsNode(LineCursor& cursor, FieldMap& fields) {
    std::vector<FieldMap> scripts;
    while (cursor.index < cursor.lines.size()) {
        if (IsBlockClose(cursor.lines[cursor.index])) {
            ++cursor.index;
            fields["scripts"] = std::to_string(scripts.size());
            fields["scriptCount"] = std::to_string(scripts.size());
            for (size_t scriptIndex = 0; scriptIndex < scripts.size(); ++scriptIndex) {
                for (const auto& [key, value] : scripts[scriptIndex]) {
                    fields["script" + std::to_string(scriptIndex) + "_" + key] = value;
                }
            }
            return true;
        }

        BlockHeader header;
        if (!ParseBlockHeader(cursor.lines[cursor.index], header) || header.identifier != "NODE_Script") {
            ++cursor.index;
            continue;
        }
        ++cursor.index;

        FieldMap scriptFields;
        auto enabledIt = header.primaryParams.find("enabled");
        if (enabledIt != header.primaryParams.end()) {
            scriptFields["enabled"] = enabledIt->second;
        }
        for (const auto& [key, value] : header.secondaryParams) {
            if (key == "scriptid") {
                scriptFields["id"] = value;
            } else if (key == "lang") {
                scriptFields["lang"] = value;
            }
        }

        std::vector<std::pair<std::string, std::string>> settings;
        while (cursor.index < cursor.lines.size()) {
            if (IsBlockClose(cursor.lines[cursor.index])) {
                ++cursor.index;
                break;
            }

            BlockHeader nestedHeader;
            if (ParseBlockHeader(cursor.lines[cursor.index], nestedHeader) && nestedHeader.identifier == "SETTING") {
                ++cursor.index;
                std::string settingValue;
                ParseSettingValueBlock(cursor, settingValue);
                const auto keyIt = nestedHeader.secondaryParams.find("key");
                const std::string settingKey = keyIt != nestedHeader.secondaryParams.end() ? keyIt->second : "";
                settings.emplace_back(settingKey, settingValue);
                continue;
            }

            std::string key;
            std::string value;
            if (ParseAssignment(cursor.lines[cursor.index], key, value)) {
                if (key == "managedType") {
                    scriptFields["type"] = value;
                } else if (key == "path") {
                    scriptFields["path"] = value;
                } else if (key == "lang") {
                    scriptFields["lang"] = value;
                } else if (key == "enabled") {
                    scriptFields["enabled"] = value;
                } else if (key == "type") {
                    scriptFields["type"] = value;
                } else {
                    scriptFields[key] = value;
                }
            }
            ++cursor.index;
        }

        scriptFields["settings"] = std::to_string(settings.size());
        scriptFields["settingCount"] = std::to_string(settings.size());
        for (size_t settingIndex = 0; settingIndex < settings.size(); ++settingIndex) {
            scriptFields["setting" + std::to_string(settingIndex)] =
                settings[settingIndex].first + ":" + settings[settingIndex].second;
        }
        scripts.push_back(std::move(scriptFields));
    }
    return false;
}

bool ParseGenericComponentNode(LineCursor& cursor, const ComponentSchema& schema, const BlockHeader& header, FieldMap& fields) {
    ApplyNodeHeaderToFields(schema, header, fields);
    while (cursor.index < cursor.lines.size()) {
        if (IsBlockClose(cursor.lines[cursor.index])) {
            ++cursor.index;
            return true;
        }
        std::string key;
        std::string value;
        if (ParseAssignment(cursor.lines[cursor.index], key, value)) {
            fields[schema.legacyKeyForNode(key)] = value;
        }
        ++cursor.index;
    }
    return false;
}

bool ParseGameObjectBlock(LineCursor& cursor, const BlockHeader& header, FlatSceneDocument& doc) {
    doc.objects.push_back({});
    FieldMap& fields = doc.objects.back().fields;
    for (const auto& [key, value] : header.secondaryParams) {
        fields[key] = value;
    }

    while (cursor.index < cursor.lines.size()) {
        if (IsBlockClose(cursor.lines[cursor.index])) {
            ++cursor.index;
            return true;
        }

        BlockHeader nestedHeader;
        if (ParseBlockHeader(cursor.lines[cursor.index], nestedHeader)) {
            ++cursor.index;
            if (nestedHeader.identifier == "NODE_Scripts") {
                if (!ParseScriptsNode(cursor, fields)) {
                    return false;
                }
                continue;
            }
            if (nestedHeader.identifier.rfind("NODE_", 0) == 0) {
                const std::string nodeName = nestedHeader.identifier.substr(5);
                if (const ComponentSchema* schema = FindSchemaByNodeName(nodeName)) {
                    if (!ParseGenericComponentNode(cursor, *schema, nestedHeader, fields)) {
                        return false;
                    }
                    continue;
                }
            }

            int depth = 1;
            while (cursor.index < cursor.lines.size() && depth > 0) {
                if (ParseBlockHeader(cursor.lines[cursor.index], nestedHeader)) {
                    ++depth;
                } else if (IsBlockClose(cursor.lines[cursor.index])) {
                    --depth;
                }
                ++cursor.index;
            }
            continue;
        }

        std::string key;
        std::string value;
        if (ParseAssignment(cursor.lines[cursor.index], key, value)) {
            fields[key] = value;
        }
        ++cursor.index;
    }
    return false;
}

bool BuildFlatDocumentFromModularStream(std::istream& in, FlatSceneDocument& doc) {
    doc.settings.clear();
    doc.objects.clear();

    const std::vector<std::string> lines = LoadMeaningfulLines(in);
    LineCursor cursor{lines, 0};
    while (cursor.index < cursor.lines.size()) {
        BlockHeader header;
        if (!ParseBlockHeader(cursor.lines[cursor.index], header)) {
            ++cursor.index;
            continue;
        }
        ++cursor.index;

        if (header.identifier == "MODU_SCENESETTINGS") {
            if (!ParseSettingsBlock(cursor, doc.settings)) return false;
            continue;
        }

        if (header.identifier == "MODU_SKYBOX") {
            if (!ParseSettingsBlock(cursor, doc.settings)) return false;
            continue;
        }

        if (header.identifier == "MODU_GAMEOBJECT") {
            if (!ParseGameObjectBlock(cursor, header, doc)) return false;
            continue;
        }

        int depth = 1;
        while (cursor.index < cursor.lines.size() && depth > 0) {
            BlockHeader nested;
            if (ParseBlockHeader(cursor.lines[cursor.index], nested)) {
                ++depth;
            } else if (IsBlockClose(cursor.lines[cursor.index])) {
                --depth;
            }
            ++cursor.index;
        }
    }

    if (doc.settings.count("version") == 0) {
        doc.settings["version"] = std::to_string(SceneSerializationInternal::kModularSceneFormatVersion);
    }
    if (doc.settings.count("objectCount") == 0) {
        doc.settings["objectCount"] = std::to_string(doc.objects.size());
    }
    return true;
}

bool LoadModularScene(std::istream& in,
                      const fs::path& sourcePath,
                      std::vector<SceneObject>& objects,
                      int& nextId,
                      int& outVersion,
                      float* outTimeOfDay,
                      SkyboxSettings* outSkyboxSettings,
                      bool deferAssetLoading,
                      SceneSerializer::Metadata* outMetadata) {
    FlatSceneDocument doc;
    if (!BuildFlatDocumentFromModularStream(in, doc)) {
        return false;
    }

    std::stringstream flatStream;
    EmitFlatSceneDocument(flatStream, doc);
    flatStream.seekg(0);

    const bool loaded = SceneSerializationInternal::LoadLegacySceneStream(flatStream,
                                                                          objects,
                                                                          nextId,
                                                                          outVersion,
                                                                          outTimeOfDay,
                                                                          outSkyboxSettings,
                                                                          deferAssetLoading,
                                                                          nullptr);
    if (!loaded) {
        return false;
    }

    ReapplyModularScripts(doc, objects);

    outVersion = SceneSerializationInternal::kModularSceneFormatVersion;
    if (outMetadata) {
        outMetadata->version = SceneSerializationInternal::kModularSceneFormatVersion;
        outMetadata->fileFormat = SceneSerializer::FileFormat::ModularNodes;
        outMetadata->loadedFromLegacyLayout = false;
        outMetadata->upgradedToModularLayout = false;
        outMetadata->sourcePath = sourcePath;
    }
    return true;
}

} // namespace

bool SceneSerializer::saveScene(const fs::path& filePath,
                                const std::vector<SceneObject>& objects,
                                int nextId,
                                float timeOfDay,
                                const SkyboxSettings& skyboxSettings) {
    return saveScene(filePath, objects, nextId, timeOfDay, skyboxSettings, SaveOptions{});
}

bool SceneSerializer::saveScene(const fs::path& filePath,
                                const std::vector<SceneObject>& objects,
                                int nextId,
                                float timeOfDay,
                                const SkyboxSettings& skyboxSettings,
                                const SaveOptions& options) {
    if (options.preference == SavePreference::ForceLegacyFlat) {
        return SceneSerializationInternal::SaveLegacyScene(filePath,
                                                           objects,
                                                           nextId,
                                                           timeOfDay,
                                                           skyboxSettings,
                                                           options.metadata);
    }

    try {
        std::ofstream out(filePath);
        if (!out.is_open()) return false;
        if (!WriteModularScene(out, objects, nextId, timeOfDay, skyboxSettings)) {
            return false;
        }
        out.close();
        if (options.metadata) {
            const bool wasLegacy = options.metadata->loadedFromLegacyLayout ||
                                   options.metadata->fileFormat == FileFormat::LegacyFlat;
            options.metadata->version = SceneSerializationInternal::kModularSceneFormatVersion;
            options.metadata->fileFormat = FileFormat::ModularNodes;
            options.metadata->loadedFromLegacyLayout = false;
            options.metadata->upgradedToModularLayout = wasLegacy;
            options.metadata->sourcePath = filePath;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to save modular scene: " << e.what() << std::endl;
        return false;
    }
}

bool SceneSerializer::loadScene(const fs::path& filePath,
                                std::vector<SceneObject>& objects,
                                int& nextId,
                                int& outVersion,
                                float* outTimeOfDay,
                                SkyboxSettings* outSkyboxSettings,
                                Metadata* outMetadata) {
    try {
        std::ifstream in(filePath);
        if (!in.is_open()) return false;

        if (DetectModularFormat(in)) {
            return LoadModularScene(in,
                                    filePath,
                                    objects,
                                    nextId,
                                    outVersion,
                                    outTimeOfDay,
                                    outSkyboxSettings,
                                    false,
                                    outMetadata);
        }

        return SceneSerializationInternal::LoadLegacyScene(filePath,
                                                           objects,
                                                           nextId,
                                                           outVersion,
                                                           outTimeOfDay,
                                                           outSkyboxSettings,
                                                           false,
                                                           outMetadata);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load scene: " << e.what() << std::endl;
        return false;
    }
}

bool SceneSerializer::loadSceneDeferred(const fs::path& filePath,
                                        std::vector<SceneObject>& objects,
                                        int& nextId,
                                        int& outVersion,
                                        float* outTimeOfDay,
                                        SkyboxSettings* outSkyboxSettings,
                                        Metadata* outMetadata) {
    try {
        std::ifstream in(filePath);
        if (!in.is_open()) return false;

        if (DetectModularFormat(in)) {
            return LoadModularScene(in,
                                    filePath,
                                    objects,
                                    nextId,
                                    outVersion,
                                    outTimeOfDay,
                                    outSkyboxSettings,
                                    true,
                                    outMetadata);
        }

        return SceneSerializationInternal::LoadLegacyScene(filePath,
                                                           objects,
                                                           nextId,
                                                           outVersion,
                                                           outTimeOfDay,
                                                           outSkyboxSettings,
                                                           true,
                                                           outMetadata);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load scene deferred: " << e.what() << std::endl;
        return false;
    }
}
