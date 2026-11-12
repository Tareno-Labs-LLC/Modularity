#pragma once
#include "MaterialAssetUtils.h"
#include "Platform/AssetSource.h"
#include "MaterialShaderParameters.h"
#include <array>
#include <cmath>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

// Project-owned material programs and declarative controls. No shader implementation
// or material preset is compiled into the engine.
struct MaterialShaderField {
    int slot = 0;
    std::string type, uniform, label;
    glm::vec4 defaults{0.0f};
    float minimum = 0.0f, maximum = 1.0f;
};
struct MaterialShaderProfile {
    std::string name, path, vert, frag, gbufferFrag, shadowDirVert, shadowCubeVert;
    std::string depthFrag;
    bool textureArrays = false, twoSided = false, cutout = false, instancing = false;
    std::string instancedVert, instancedShadowDirVert, instancedShadowCubeVert;
    std::vector<MaterialShaderField> fields;
};
inline std::vector<std::string> SplitMaterialProfile(const std::string& text, char separator) {
    std::vector<std::string> parts; std::istringstream stream(text); std::string part;
    while (std::getline(stream, part, separator)) parts.push_back(MaterialAssetTrim(part));
    return parts;
}
inline glm::vec4 ParseMaterialShaderValue(const std::string& text) {
    glm::vec4 result(0.0f); const auto parts = SplitMaterialProfile(text, ',');
    for (size_t i=0; i<parts.size() && i<4; ++i) {
        try { float v=std::stof(parts[i]); if (std::isfinite(v)) result[int(i)]=v; } catch (...) {}
    }
    return result;
}
inline std::string MaterialShaderValueText(const glm::vec4& v) {
    std::ostringstream out; out.precision(9); out<<v.x<<","<<v.y<<","<<v.z<<","<<v.w; return out.str();
}
struct MaterialProfileRegistry {
    std::array<std::atomic<MaterialShaderProfile*>, 256> entries{};
    std::vector<std::unique_ptr<MaterialShaderProfile>> owned;
    std::unordered_map<std::string,uint8_t> handles;
    std::mutex mutex;
};
inline MaterialProfileRegistry& GetMaterialProfileRegistry() { static MaterialProfileRegistry registry; return registry; }
inline const MaterialShaderProfile& GetMaterialShaderProfile(uint8_t id) {
    static const MaterialShaderProfile empty;
    auto* p=GetMaterialProfileRegistry().entries[id].load(std::memory_order_acquire);
    return p ? *p : empty;
}
inline uint8_t RegisterMaterialShaderProfile(const std::string& path) {
    if (path.empty()) return 0;
    // Different projects may contain the same relative profile name. Key the
    // registry and stage paths by the resolved asset, while persisting portable paths.
    const std::string resolved=Modularity::Platform::ResolveAssetPath(path);
    auto& registry=GetMaterialProfileRegistry(); std::lock_guard<std::mutex> guard(registry.mutex);
    const auto it=registry.handles.find(resolved); if (it!=registry.handles.end()) return it->second;
    if (registry.owned.size()>=255) return 0;
    const auto bytes=Modularity::Platform::GetAssetSource().ReadAll(resolved);
    if (bytes.empty()) return 0;
    auto profile=std::make_unique<MaterialShaderProfile>(); profile->path=path;
    std::istringstream stream(std::string(bytes.begin(),bytes.end())); std::string line;
    try {
        while (std::getline(stream,line)) {
            auto eq=line.find('='); if(eq==std::string::npos) continue;
            const auto key=MaterialAssetLowercase(MaterialAssetTrim(line.substr(0,eq)));
            const auto value=MaterialAssetTrim(line.substr(eq+1));
            auto stage=[&]{ return ResolveShaderPackReferencedPath(fs::path(resolved),value).generic_string(); };
            if(key=="name") profile->name=value;
            else if(key=="vertex") profile->vert=stage();
            else if(key=="fragment") profile->frag=stage();
            else if(key=="gbufferfragment") profile->gbufferFrag=stage();
            else if(key=="depthfragment") profile->depthFrag=stage();
            else if(key=="shadowdirectionalvertex") profile->shadowDirVert=stage();
            else if(key=="shadowcubevertex") profile->shadowCubeVert=stage();
            else if(key=="texturearrays") profile->textureArrays=value=="1";
            else if(key=="twosided") profile->twoSided=value=="1";
            else if(key=="cutout") profile->cutout=value=="1";
            else if(key=="instancing") profile->instancing=value=="1";
            else if(key=="uniform") {
                const auto parts=SplitMaterialProfile(value,'|'); if(parts.size()!=7) continue;
                MaterialShaderField f; f.slot=std::stoi(parts[0]); if(f.slot<0||f.slot>=32) continue;
                f.type=parts[1]; if(f.type!="float"&&f.type!="int"&&f.type!="bool"&&f.type!="vec2"&&f.type!="vec3"&&f.type!="vec4") continue;
                f.uniform=parts[2]; f.label=parts[3];
                auto limits=SplitMaterialProfile(parts[6],','); if(limits.size()!=2) continue;
                f.defaults=ParseMaterialShaderValue(parts[4]);
                f.minimum=std::stof(limits[0]); f.maximum=std::stof(limits[1]);
                profile->fields.push_back(std::move(f));
            }
        }
    } catch (...) { return 0; }
    if(profile->vert.empty()||profile->frag.empty()||profile->fields.empty()) return 0;
    if (profile->instancing) {
        const auto variant=[](const std::string& stage) {
            return stage.empty() ? std::string() : stage + (stage.find('?') == std::string::npos ? "?" : ",") + "MODU_INSTANCED";
        };
        profile->instancedVert=variant(profile->vert);
        profile->instancedShadowDirVert=variant(profile->shadowDirVert);
        profile->instancedShadowCubeVert=variant(profile->shadowCubeVert);
    }
    const uint8_t id=static_cast<uint8_t>(registry.owned.size()+1);
    auto* raw=profile.get(); registry.owned.push_back(std::move(profile)); registry.handles.emplace(resolved,id);
    registry.entries[id].store(raw,std::memory_order_release); return id;
}
inline void SetMaterialShaderProfile(MaterialShaderParameters& parameters, const std::string& path, bool defaults=true) {
    if(path.size()>=sizeof(parameters.profilePath)) return;
    const uint8_t id=RegisterMaterialShaderProfile(path);
    std::fill(std::begin(parameters.profilePath),std::end(parameters.profilePath),'\0');
    std::copy(path.begin(),path.end(),parameters.profilePath); parameters.profileId=id;
    if(defaults) { parameters.values={}; for(const auto& f:GetMaterialShaderProfile(id).fields) parameters.values[f.slot]=f.defaults; }
}
inline bool ParseMaterialShaderParameter(const std::string& key,const std::string& value,MaterialShaderParameters& p) {
    if(key=="shaderProfile") { SetMaterialShaderProfile(p,value); return true; }
    if(key.rfind("shaderValue",0)!=0) return false;
    try { const int slot=std::stoi(key.substr(11)); if(slot>=0&&slot<32) p.values[slot]=ParseMaterialShaderValue(value); } catch(...) {}
    return true;
}
inline void WriteMaterialShaderParameters(std::ostream& out,const MaterialShaderParameters& p,const char* prefix="") {
    out<<prefix<<"shaderProfile="<<Modularity::Platform::ToPortableAssetPath(p.profilePath)<<"\n";
    for(size_t i=0;i<p.values.size();++i) out<<prefix<<"shaderValue"<<i<<"="<<MaterialShaderValueText(p.values[i])<<"\n";
}
