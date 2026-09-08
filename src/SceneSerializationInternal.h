#pragma once

#include "ProjectManager.h"
#include <istream>
#include <ostream>
#include <string>

namespace SceneSerializationInternal {

constexpr int kLegacySceneFormatVersion = 28;
constexpr int kModularSceneFormatVersion = 29;

bool SceneValueLooksLikeAbsolutePath(const std::string& value);

std::string MakeSceneTextPathsPortable(const std::string& sceneText);

bool WriteLegacySceneStream(std::ostream& out,
                            const std::vector<SceneObject>& objects,
                            int nextId,
                            float timeOfDay,
                            const SkyboxSettings& skyboxSettings);

bool SaveLegacyScene(const fs::path& filePath,
                     const std::vector<SceneObject>& objects,
                     int nextId,
                     float timeOfDay,
                     const SkyboxSettings& skyboxSettings,
                     SceneSerializer::Metadata* metadata = nullptr);

bool LoadLegacySceneStream(std::istream& in,
                           std::vector<SceneObject>& objects,
                           int& nextId,
                           int& outVersion,
                           float* outTimeOfDay,
                           SkyboxSettings* outSkyboxSettings,
                           bool deferAssetLoading,
                           SceneSerializer::Metadata* outMetadata = nullptr);

bool LoadLegacyScene(const fs::path& filePath,
                     std::vector<SceneObject>& objects,
                     int& nextId,
                     int& outVersion,
                     float* outTimeOfDay,
                     SkyboxSettings* outSkyboxSettings,
                     bool deferAssetLoading,
                     SceneSerializer::Metadata* outMetadata = nullptr);

} // namespace SceneSerializationInternal
