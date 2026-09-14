#include "Engine.h"
#include "HardwareProfile.h"
#include "ModelLoader.h"
#include "../../include/Platform/AssetSource.h"
#include "../../include/ThirdParty/stb_image.h"
#include "../../include/Textures/StbFlipScope.h"
#include "../DragPreviewOverlay.h"
#include "../EditorLocalization.h"
#include "../ModuCPPLanguagePack.h"
#include "../Modu2DStats.h"
#ifdef __ANDROID__
#include "../AndroidRuntime/AndroidRuntime.h"
#endif
#include <algorithm>
#include <array>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <future>
#include <chrono>
#include <future>

#ifdef _WIN32
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#endif

namespace Loc = Modularity::Loc;

#pragma region File Icons
namespace FileIcons {
    namespace {
        enum IconSlot {
            IconSlot_FolderEmpty = 0,
            IconSlot_FolderFull,
            IconSlot_Script,
            IconSlot_Scene,
            IconSlot_Material,
            IconSlot_Video,
            IconSlot_Audio,
            IconSlot_Text,
            IconSlot_Model,
            IconSlot_Unknown,
            IconSlot_Count
        };

        constexpr int kIconAtlasCell = 160;
        constexpr int kIconAtlasCols = 4;
        constexpr int kIconAtlasRows = 3;
        constexpr int kIconAtlasWidth = kIconAtlasCell * kIconAtlasCols;
        constexpr int kIconAtlasHeight = kIconAtlasCell * kIconAtlasRows;
        constexpr int kIconAtlasMaxMipLevel = 2;

        const char* IconSlotPath(int slot) {
            switch (slot) {
                case IconSlot_FolderEmpty: return "Resources/Engine-Root/File Explorer/Folder Empty.png";
                case IconSlot_FolderFull:  return "Resources/Engine-Root/File Explorer/Folder Full.png";
                case IconSlot_Script:      return "Resources/Engine-Root/File Explorer/File Icon Script.png";
                case IconSlot_Scene:       return "Resources/Engine-Root/File Explorer/File Icon Scenes.png";
                case IconSlot_Material:    return "Resources/Engine-Root/File Explorer/File Icon Material.png";
                case IconSlot_Video:       return "Resources/Engine-Root/File Explorer/File Icon Video File.png";
                case IconSlot_Audio:       return "Resources/Engine-Root/File Explorer/File Icon Audio File.png";
                case IconSlot_Text:        return "Resources/Engine-Root/File Explorer/File Icon Text.png";
                case IconSlot_Model:       return "Resources/Engine-Root/File Explorer/File Icon Model.png";
                case IconSlot_Unknown:     return "Resources/Engine-Root/File Explorer/File Icon Unknown or empty.png";
                default: return nullptr;
            }
        }

        int IconSlotForCategory(FileCategory category, bool folderHasItems) {
            switch (category) {
                case FileCategory::Folder:   return folderHasItems ? IconSlot_FolderFull : IconSlot_FolderEmpty;
                case FileCategory::Script:   return IconSlot_Script;
                case FileCategory::Scene:    return IconSlot_Scene;
                case FileCategory::Material: return IconSlot_Material;
                case FileCategory::Video:    return IconSlot_Video;
                case FileCategory::Audio:    return IconSlot_Audio;
                case FileCategory::Text:     return IconSlot_Text;
                case FileCategory::Shader:   return IconSlot_Text;
                case FileCategory::Model:    return IconSlot_Model;
                case FileCategory::Unknown:  return IconSlot_Unknown;
                default: return -1;
            }
        }

        void BlitIconIntoAtlasCell(const unsigned char* src, int srcW, int srcH,
                                   std::vector<unsigned char>& atlas, int slot) {
            if (!src || srcW <= 0 || srcH <= 0) return;
            const int cellX = (slot % kIconAtlasCols) * kIconAtlasCell;
            const int cellY = (slot / kIconAtlasCols) * kIconAtlasCell;
            for (int y = 0; y < kIconAtlasCell; ++y) {
                const int sy0 = static_cast<int>(static_cast<int64_t>(y) * srcH / kIconAtlasCell);
                const int sy1 = std::max(sy0 + 1,
                    static_cast<int>(static_cast<int64_t>(y + 1) * srcH / kIconAtlasCell));
                for (int x = 0; x < kIconAtlasCell; ++x) {
                    const int sx0 = static_cast<int>(static_cast<int64_t>(x) * srcW / kIconAtlasCell);
                    const int sx1 = std::max(sx0 + 1,
                        static_cast<int>(static_cast<int64_t>(x + 1) * srcW / kIconAtlasCell));
                    uint64_t accR = 0, accG = 0, accB = 0, accA = 0;
                    uint64_t samples = 0;
                    for (int sy = sy0; sy < sy1 && sy < srcH; ++sy) {
                        const unsigned char* row = src + (static_cast<size_t>(sy) * srcW + sx0) * 4;
                        for (int sx = sx0; sx < sx1 && sx < srcW; ++sx, row += 4) {
                            const uint64_t a = row[3];
                            accR += static_cast<uint64_t>(row[0]) * a;
                            accG += static_cast<uint64_t>(row[1]) * a;
                            accB += static_cast<uint64_t>(row[2]) * a;
                            accA += a;
                            ++samples;
                        }
                    }
                    if (samples == 0) continue;
                    unsigned char* out = atlas.data() +
                        (static_cast<size_t>(cellY + y) * kIconAtlasWidth + (cellX + x)) * 4;
                    out[0] = accA ? static_cast<unsigned char>(accR / accA) : 0;
                    out[1] = accA ? static_cast<unsigned char>(accG / accA) : 0;
                    out[2] = accA ? static_cast<unsigned char>(accB / accA) : 0;
                    out[3] = static_cast<unsigned char>(accA / samples);
                }
            }
        }

        std::vector<unsigned char> DecodeIconAtlasPixels() {
            // ImGui samples the atlas top-down, so it is decoded top-down.
            const StbFlipScope flipForAtlas(false);
            std::vector<unsigned char> atlas(
                static_cast<size_t>(kIconAtlasWidth) * kIconAtlasHeight * 4, 0);
            for (int slot = 0; slot < IconSlot_Count; ++slot) {
                const char* path = IconSlotPath(slot);
                if (!path) continue;
                const std::vector<uint8_t> bytes =
                    Modularity::Platform::GetAssetSource().ReadAll(path);
                if (bytes.empty()) continue;
                int w = 0, h = 0, channels = 0;
                unsigned char* pixels = stbi_load_from_memory(
                    bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
                if (!pixels) continue;
                BlitIconIntoAtlasCell(pixels, w, h, atlas, slot);
                stbi_image_free(pixels);
            }
            return atlas;
        }

        struct IconAtlas {
            GLuint texture = 0;
            bool started = false;
            bool failed = false;
            std::future<std::vector<unsigned char>> decode;
            int lastPollFrame = -1;
        };

        IconAtlas& GetIconAtlas() {
            static IconAtlas atlas;
            return atlas;
        }

        bool EnsureIconAtlas() {
            IconAtlas& atlas = GetIconAtlas();
            if (atlas.texture != 0) return true;
            if (atlas.failed) return false;

            const int frame = ImGui::GetFrameCount();
            if (atlas.lastPollFrame == frame) return false;
            atlas.lastPollFrame = frame;

            if (!atlas.started) {
                atlas.started = true;
                atlas.decode = std::async(std::launch::async, &DecodeIconAtlasPixels);
                return false;
            }
            if (!atlas.decode.valid()) {
                atlas.failed = true;
                return false;
            }
            if (atlas.decode.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return false;
            }

            const std::vector<unsigned char> pixels = atlas.decode.get();
            if (pixels.size() != static_cast<size_t>(kIconAtlasWidth) * kIconAtlasHeight * 4) {
                atlas.failed = true;
                return false;
            }

            GLint previousTexture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
            glGenTextures(1, &atlas.texture);
            if (atlas.texture == 0) {
                atlas.failed = true;
                return false;
            }
            glBindTexture(GL_TEXTURE_2D, atlas.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kIconAtlasWidth, kIconAtlasHeight, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, kIconAtlasMaxMipLevel);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
            return true;
        }

    }

    bool TryGetAtlasIcon(FileCategory category, bool folderHasItems,
                         ImTextureID& outTexture, ImVec2& outUv0, ImVec2& outUv1) {
        const int slot = IconSlotForCategory(category, folderHasItems);
        if (slot < 0 || !EnsureIconAtlas()) return false;
        outTexture = (ImTextureID)(intptr_t)GetIconAtlas().texture;
        outUv0 = ImVec2(static_cast<float>((slot % kIconAtlasCols) * kIconAtlasCell) / kIconAtlasWidth,
                        static_cast<float>((slot / kIconAtlasCols) * kIconAtlasCell) / kIconAtlasHeight);
        outUv1 = ImVec2(outUv0.x + static_cast<float>(kIconAtlasCell) / kIconAtlasWidth,
                        outUv0.y + static_cast<float>(kIconAtlasCell) / kIconAtlasHeight);
        return true;
    }

    namespace {
        struct QueuedIcon {
            ImVec2 min;
            ImVec2 max;
            ImVec2 uv0;
            ImVec2 uv1;
        };

        std::vector<QueuedIcon>& GetIconQueue() {
            static std::vector<QueuedIcon> queue;
            return queue;
        }

        bool DrawTexturedIcon(Renderer& renderer, ImDrawList* drawList, FileCategory category, ImVec2 pos, float size, bool folderHasItems) {
            (void)renderer;
            ImTextureID texture{};
            ImVec2 uv0, uv1;
            if (!TryGetAtlasIcon(category, folderHasItems, texture, uv0, uv1)) return false;
            (void)drawList;
            GetIconQueue().push_back(
                QueuedIcon{pos, ImVec2(pos.x + size, pos.y + size), uv0, uv1});
            return true;
        }

        ImU32 BlendColor(ImU32 a, ImU32 b, float t) {
            int ar = a & 0xFF;
            int ag = (a >> 8) & 0xFF;
            int ab = (a >> 16) & 0xFF;
            int aa = (a >> 24) & 0xFF;
            int br = b & 0xFF;
            int bg = (b >> 8) & 0xFF;
            int bb = (b >> 16) & 0xFF;
            int ba = (b >> 24) & 0xFF;
            int r = ar + static_cast<int>((br - ar) * t);
            int g = ag + static_cast<int>((bg - ag) * t);
            int bch = ab + static_cast<int>((bb - ab) * t);
            int aout = aa + static_cast<int>((ba - aa) * t);
            return IM_COL32(r, g, bch, aout);
        }

        struct PaperFrame {
            ImVec2 min;
            ImVec2 max;
        };

        PaperFrame DrawSheetFileBase(ImDrawList* drawList, ImVec2 pos, float size, ImU32 accentColor) {
            const ImU32 kSheetBase = IM_COL32(248, 248, 248, 255);
            const ImU32 kSheetBack = IM_COL32(238, 238, 238, 235);
            const ImU32 kSheetEdge = IM_COL32(185, 185, 185, 200);
            const ImU32 kSheetFold = IM_COL32(232, 232, 232, 255);
            const ImU32 kSheetShadow = IM_COL32(0, 0, 0, 55);
            const ImU32 kSheetInner = IM_COL32(255, 255, 255, 80);
            const ImU32 kFoldShadow = IM_COL32(0, 0, 0, 35);

            float w = size * 0.78f;
            float h = size * 0.95f;
            float offsetX = (size - w) * 0.5f;
            float offsetY = (size - h) * 0.5f;
            float cornerSize = w * 0.24f;
            float rounding = size * 0.075f;
            float shadowOffset = size * 0.04f;
            float backOffset = size * 0.02f;

            ImVec2 min = ImVec2(pos.x + offsetX, pos.y + offsetY);
            ImVec2 max = ImVec2(pos.x + offsetX + w, pos.y + offsetY + h);

            ImVec2 backMin = ImVec2(min.x + backOffset, min.y + backOffset * 0.6f);
            ImVec2 backMax = ImVec2(max.x + backOffset, max.y + backOffset * 0.6f);
            drawList->AddRectFilled(backMin, backMax, kSheetBack, rounding);

            drawList->AddRectFilled(ImVec2(min.x + shadowOffset, min.y + shadowOffset),
                                    ImVec2(max.x + shadowOffset, max.y + shadowOffset),
                                    kSheetShadow, rounding);

            drawList->AddRectFilled(min, max, kSheetBase, rounding);
            drawList->AddRect(min, max, kSheetEdge, rounding, 0, 0.4f);
            drawList->AddRect(ImVec2(min.x + 1.0f, min.y + 1.0f),
                              ImVec2(max.x - 1.0f, max.y - 1.0f),
                              kSheetInner, rounding, 0, 0.6f);

            ImVec2 foldA(max.x - cornerSize, min.y);
            ImVec2 foldB(max.x, min.y + cornerSize);
            ImVec2 foldC(max.x - cornerSize, min.y + cornerSize);
            drawList->AddTriangleFilled(ImVec2(foldA.x + 1.0f, foldA.y + 1.0f),
                                        ImVec2(foldB.x + 1.0f, foldB.y + 1.0f),
                                        ImVec2(foldC.x + 1.0f, foldC.y + 1.0f),
                                        kFoldShadow);
            drawList->AddTriangleFilled(foldA, foldB, ImVec2(max.x - cornerSize, min.y + cornerSize), kSheetFold);
            drawList->AddTriangle(foldA, foldB, foldC, kSheetEdge, 1.0f);

            ImU32 band = BlendColor(accentColor, kSheetBase, 0.55f);
            float bandH = h * 0.16f;
            drawList->AddRectFilled(ImVec2(min.x, max.y - bandH), max, band, rounding);

            ImVec2 contentMin(min.x + w * 0.12f, min.y + h * 0.16f);
            ImVec2 contentMax(max.x - w * 0.12f, max.y - h * 0.24f);
            return {contentMin, contentMax};
        }

    }

    // Draw a folder icon
    void DrawFolderIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        const ImU32 kFolderBase = IM_COL32(198, 178, 120, 255);
        const ImU32 kFolderTab = IM_COL32(215, 197, 140, 255);
        const ImU32 kFolderEdge = IM_COL32(120, 104, 70, 200);
        const ImU32 kFolderShadow = IM_COL32(110, 95, 60, 60);

        float w = size;
        float bodyY = size * 0.273f;
        float bodyH = size * 0.547f;
        float tabY = size * 0.125f;
        float tabH = size * 0.203f;
        float tabW = size * 0.43f;
        float rounding = size * 0.07f;
        float shadowOffset = size * 0.025f;
        float outline = std::max(0.8f, size * 0.04f);

        ImVec2 bodyMin(pos.x, pos.y + bodyY);
        ImVec2 bodyMax(pos.x + w, pos.y + bodyY + bodyH);
        ImVec2 tabMin(pos.x + w * 0.031f, pos.y + tabY);
        ImVec2 tabMax(pos.x + w * 0.031f + tabW, pos.y + tabY + tabH);

        drawList->AddRectFilled(ImVec2(bodyMin.x + shadowOffset, bodyMin.y + shadowOffset),
                                ImVec2(bodyMax.x + shadowOffset, bodyMax.y + shadowOffset),
                                kFolderShadow, rounding);

        drawList->AddRectFilled(bodyMin, bodyMax, kFolderBase, rounding);
        drawList->AddRectFilled(tabMin, tabMax, kFolderTab, rounding);

        ImU32 band = BlendColor(color, kFolderBase, 0.7f);
        float bandH = size * 0.094f;
        drawList->AddRectFilled(ImVec2(bodyMin.x, bodyMax.y - bandH), bodyMax, band, rounding);

        drawList->AddRect(bodyMin, bodyMax, kFolderEdge, rounding, 0, outline);
        drawList->AddRect(tabMin, tabMax, kFolderEdge, rounding, 0, outline * 0.85f);
    }

    void DrawFolderIconOpen(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        const ImU32 kFolderBase = IM_COL32(198, 178, 120, 255);
        const ImU32 kFolderTab = IM_COL32(215, 197, 140, 255);
        const ImU32 kFolderEdge = IM_COL32(120, 104, 70, 200);
        const ImU32 kFolderShadow = IM_COL32(110, 95, 60, 55);
        const ImU32 kPaperFill = IM_COL32(245, 245, 245, 255);

        float w = size;
        float bodyY = size * 0.273f;
        float bodyH = size * 0.547f;
        float tabY = size * 0.145f;
        float tabH = size * 0.17f;
        float tabW = size * 0.40f;
        float rounding = size * 0.075f;
        float shadowOffset = size * 0.02f;
        float outline = std::max(0.8f, size * 0.04f);
        float openOffset = size * 0.06f;
        float backRounding = rounding * 0.35f;
        float tabRounding = rounding * 0.45f;

        ImVec2 bodyMin(pos.x, pos.y + bodyY);
        ImVec2 bodyMax(pos.x + w, pos.y + bodyY + bodyH);
        ImVec2 backMin(bodyMin.x, bodyMin.y - openOffset);
        ImVec2 backMax(bodyMax.x, bodyMax.y - openOffset);
        ImVec2 tabMin(pos.x + w * 0.06f, pos.y + tabY - openOffset * 0.4f);
        ImVec2 tabMax(pos.x + w * 0.06f + tabW, pos.y + tabY + tabH - openOffset * 0.4f);

        drawList->AddRectFilled(ImVec2(bodyMin.x + shadowOffset, bodyMin.y + shadowOffset),
                                ImVec2(bodyMax.x + shadowOffset, bodyMax.y + shadowOffset),
                                kFolderShadow, rounding);

        drawList->AddRectFilled(backMin, backMax, kFolderTab, backRounding);
        drawList->AddRect(backMin, backMax, kFolderEdge, backRounding, 0, outline * 0.8f);

        drawList->AddRectFilled(tabMin, tabMax, kFolderTab, tabRounding);
        drawList->AddRect(tabMin, tabMax, kFolderEdge, tabRounding, 0, outline * 0.75f);

        ImVec2 paperMin(pos.x + w * 0.1f, backMin.y + bodyH * 0.08f);
        ImVec2 paperMax(pos.x + w * 0.9f, backMin.y + bodyH * 0.62f);
        float paperRadius = rounding * 0.8f;
        float paperOutline = outline * 0.6f;

        ImVec2 p1Min = ImVec2(paperMin.x + w * 0.02f, paperMin.y - size * 0.04f);
        ImVec2 p1Max = ImVec2(paperMax.x - w * 0.02f, paperMax.y - size * 0.04f);
        ImVec2 p2Min = ImVec2(paperMin.x + w * 0.04f, paperMin.y - size * 0.02f);
        ImVec2 p2Max = ImVec2(paperMax.x - w * 0.04f, paperMax.y - size * 0.02f);
        ImVec2 p3Min = paperMin;
        ImVec2 p3Max = paperMax;

        drawList->AddRectFilled(p1Min, p1Max, kPaperFill, paperRadius);
        drawList->AddRect(p1Min, p1Max, kFolderEdge, paperRadius, 0, paperOutline);
        drawList->AddRectFilled(p2Min, p2Max, kPaperFill, paperRadius);
        drawList->AddRect(p2Min, p2Max, kFolderEdge, paperRadius, 0, paperOutline);
        drawList->AddRectFilled(p3Min, p3Max, kPaperFill, paperRadius);
        drawList->AddRect(p3Min, p3Max, kFolderEdge, paperRadius, 0, paperOutline);

        ImVec2 frontMin(bodyMin.x, bodyMin.y + openOffset);
        ImVec2 frontMax(bodyMax.x, bodyMax.y);

        drawList->AddRectFilled(frontMin, frontMax, kFolderBase, rounding);
        ImU32 band = BlendColor(color, kFolderBase, 0.7f);
        float bandH = size * 0.094f;
        drawList->AddRectFilled(ImVec2(frontMin.x, frontMax.y - bandH), frontMax, band, rounding);
        drawList->AddRect(frontMin, frontMax, kFolderEdge, rounding, 0, outline);
    }

    // Draw a scene/document icon
    void DrawSceneIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        DrawSheetFileBase(drawList, pos, size, color);
    }

    // Draw a 3D model icon (cube wireframe)
    void DrawModelIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        PaperFrame frame = DrawSheetFileBase(drawList, pos, size, color);
        ImU32 ink = IM_COL32(90, 90, 90, 230);
        ImVec2 min = frame.min;
        ImVec2 max = frame.max;
        float w = max.x - min.x;
        float h = max.y - min.y;
        float s = std::min(w, h) * 0.6f;
        ImVec2 origin(min.x + w * 0.25f, min.y + h * 0.25f);
        float depth = s * 0.35f;

        ImVec2 f1(origin.x, origin.y + depth);
        ImVec2 f2(origin.x + s, origin.y + depth);
        ImVec2 f3(origin.x + s, origin.y + s + depth);
        ImVec2 f4(origin.x, origin.y + s + depth);

        ImVec2 b1(f1.x + depth, f1.y - depth);
        ImVec2 b2(f2.x + depth, f2.y - depth);
        ImVec2 b3(f3.x + depth, f3.y - depth);
        ImVec2 b4(f4.x + depth, f4.y - depth);

        drawList->AddLine(f1, f2, ink, 1.4f);
        drawList->AddLine(f2, f3, ink, 1.4f);
        drawList->AddLine(f3, f4, ink, 1.4f);
        drawList->AddLine(f4, f1, ink, 1.4f);
        drawList->AddLine(b1, b2, ink, 1.2f);
        drawList->AddLine(b2, b3, ink, 1.2f);
        drawList->AddLine(b3, b4, ink, 1.2f);
        drawList->AddLine(b4, b1, ink, 1.2f);
        drawList->AddLine(f1, b1, ink, 1.2f);
        drawList->AddLine(f2, b2, ink, 1.2f);
        drawList->AddLine(f3, b3, ink, 1.2f);
        drawList->AddLine(f4, b4, ink, 1.2f);
    }

    // Draw a texture/image icon
    void DrawTextureIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        PaperFrame frame = DrawSheetFileBase(drawList, pos, size, color);
        ImU32 ink = IM_COL32(90, 90, 90, 230);
        ImVec2 min = frame.min;
        ImVec2 max = frame.max;
        float w = max.x - min.x;
        float h = max.y - min.y;

        ImVec2 frameMin(min.x + w * 0.08f, min.y + h * 0.12f);
        ImVec2 frameMax(max.x - w * 0.08f, max.y - h * 0.1f);
        drawList->AddRect(frameMin, frameMax, ink, 3.0f, 0, 0.8f);

        float midY = frameMax.y - h * 0.28f;
        drawList->AddTriangleFilled(
            ImVec2(frameMin.x + w * 0.1f, frameMax.y),
            ImVec2(frameMin.x + w * 0.35f, midY),
            ImVec2(frameMin.x + w * 0.6f, frameMax.y),
            ink
        );
        drawList->AddTriangleFilled(
            ImVec2(frameMin.x + w * 0.45f, frameMax.y),
            ImVec2(frameMin.x + w * 0.7f, midY + h * 0.1f),
            ImVec2(frameMin.x + w * 0.9f, frameMax.y),
            ink
        );

        float sunR = std::min(w, h) * 0.12f;
        drawList->AddCircleFilled(ImVec2(frameMax.x - w * 0.18f, frameMin.y + h * 0.2f), sunR, ink);
    }

    // Draw a shader icon (code brackets)
    void DrawShaderIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        PaperFrame frame = DrawSheetFileBase(drawList, pos, size, color);
        ImU32 ink = IM_COL32(90, 90, 90, 230);
        ImVec2 min = frame.min;
        ImVec2 max = frame.max;
        float w = max.x - min.x;
        float h = max.y - min.y;

        float lineH = h * 0.12f;
        float lineY = min.y + h * 0.2f;
        drawList->AddRectFilled(ImVec2(min.x + w * 0.15f, lineY), ImVec2(min.x + w * 0.75f, lineY + lineH), ink);
        lineY += h * 0.22f;
        drawList->AddRectFilled(ImVec2(min.x + w * 0.2f, lineY), ImVec2(min.x + w * 0.85f, lineY + lineH), ink);
        lineY += h * 0.22f;
        drawList->AddRectFilled(ImVec2(min.x + w * 0.15f, lineY), ImVec2(min.x + w * 0.55f, lineY + lineH), ink);
    }

    // Draw an audio icon (speaker/waveform)
    void DrawAudioIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        PaperFrame frame = DrawSheetFileBase(drawList, pos, size, color);
        ImU32 ink = IM_COL32(90, 90, 90, 230);
        ImVec2 min = frame.min;
        ImVec2 max = frame.max;
        float w = max.x - min.x;
        float h = max.y - min.y;
        float spkW = w * 0.25f;
        float spkH = h * 0.28f;
        float cx = min.x + w * 0.35f;
        float cy = min.y + h * 0.55f;

        drawList->AddRectFilled(
            ImVec2(cx - spkW * 0.5f, cy - spkH * 0.5f),
            ImVec2(cx + spkW * 0.5f, cy + spkH * 0.5f),
            ink
        );
        drawList->AddTriangleFilled(
            ImVec2(cx + spkW * 0.5f, cy - spkH * 0.5f),
            ImVec2(cx + spkW * 0.5f, cy + spkH * 0.5f),
            ImVec2(cx + spkW * 1.1f, cy + spkH * 0.9f),
            ink
        );
        drawList->AddTriangleFilled(
            ImVec2(cx + spkW * 0.5f, cy - spkH * 0.5f),
            ImVec2(cx + spkW * 1.1f, cy - spkH * 0.9f),
            ImVec2(cx + spkW * 1.1f, cy + spkH * 0.9f),
            ink
        );

        float waveX = cx + spkW * 1.4f;
        drawList->AddBezierQuadratic(
            ImVec2(waveX, cy - h * 0.12f),
            ImVec2(waveX + w * 0.12f, cy),
            ImVec2(waveX, cy + h * 0.12f),
            ink, 1.6f
        );
        waveX += w * 0.12f;
        drawList->AddBezierQuadratic(
            ImVec2(waveX, cy - h * 0.2f),
            ImVec2(waveX + w * 0.14f, cy),
            ImVec2(waveX, cy + h * 0.2f),
            ink, 1.6f
        );
    }

    // Draw a generic file icon
    void DrawFileIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        DrawSheetFileBase(drawList, pos, size, color);
    }

    // Draw a script/code icon
    void DrawScriptIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        PaperFrame frame = DrawSheetFileBase(drawList, pos, size, color);
        ImU32 ink = IM_COL32(90, 90, 90, 230);
        ImVec2 min = frame.min;
        ImVec2 max = frame.max;
        float w = max.x - min.x;
        float h = max.y - min.y;
        float cx = min.x + w * 0.5f;
        float cy = min.y + h * 0.5f;
        float bSize = std::min(w, h) * 0.28f;

        drawList->AddLine(ImVec2(cx - bSize * 0.5f, cy - bSize), ImVec2(cx - bSize * 1.3f, cy), ink, 1.8f);
        drawList->AddLine(ImVec2(cx - bSize * 1.3f, cy), ImVec2(cx - bSize * 0.5f, cy + bSize), ink, 1.8f);
        drawList->AddLine(ImVec2(cx + bSize * 0.5f, cy - bSize), ImVec2(cx + bSize * 1.3f, cy), ink, 1.8f);
        drawList->AddLine(ImVec2(cx + bSize * 1.3f, cy), ImVec2(cx + bSize * 0.5f, cy + bSize), ink, 1.8f);
    }

    // Draw a text icon
    void DrawTextIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        PaperFrame frame = DrawSheetFileBase(drawList, pos, size, color);
        ImU32 ink = IM_COL32(90, 90, 90, 230);
        ImVec2 min = frame.min;
        ImVec2 max = frame.max;
        float w = max.x - min.x;
        float h = max.y - min.y;
        float lineY = min.y + h * 0.25f;
        float lineH = h * 0.12f;
        float spacing = h * 0.2f;

        for (int i = 0; i < 3; ++i) {
            float lineW = (i == 1) ? w * 0.55f : w * 0.75f;
            drawList->AddRectFilled(ImVec2(min.x + w * 0.12f, lineY),
                                    ImVec2(min.x + w * 0.12f + lineW, lineY + lineH),
                                    ink);
            lineY += spacing;
        }
    }

    void DrawIcon(Renderer& renderer, ImDrawList* drawList, FileCategory category, ImVec2 pos, float size, ImU32 color, bool folderHasItems) {
        if (DrawTexturedIcon(renderer, drawList, category, pos, size, folderHasItems)) {
            return;
        }

        switch (category) {
            case FileCategory::Folder:
                if (folderHasItems) {
                    DrawFolderIconOpen(drawList, pos, size, color);
                } else {
                    DrawFolderIcon(drawList, pos, size, color);
                }
                break;
            case FileCategory::Scene:   DrawSceneIcon(drawList, pos, size, color); break;
            case FileCategory::Model:   DrawModelIcon(drawList, pos, size, color); break;
            case FileCategory::Material:DrawShaderIcon(drawList, pos, size, color); break;
            case FileCategory::Texture: DrawTextureIcon(drawList, pos, size, color); break;
            case FileCategory::Video:   DrawTextureIcon(drawList, pos, size, color); break;
            case FileCategory::Shader:  DrawTextIcon(drawList, pos, size, color); break;
            case FileCategory::Script:  DrawScriptIcon(drawList, pos, size, color); break;
            case FileCategory::Audio:   DrawAudioIcon(drawList, pos, size, color); break;
            case FileCategory::Text:    DrawTextIcon(drawList, pos, size, color); break;
            default:                    DrawFileIcon(drawList, pos, size, color); break;
        }
    }

    void FlushIconBatch(ImDrawList* drawList, ImVec2 clipMin, ImVec2 clipMax) {
        std::vector<QueuedIcon>& queue = GetIconQueue();
        if (queue.empty()) return;
        const IconAtlas& atlas = GetIconAtlas();
        if (drawList && atlas.texture != 0 && clipMax.x > clipMin.x && clipMax.y > clipMin.y) {
            const ImTextureID texture = (ImTextureID)(intptr_t)atlas.texture;
            drawList->PushClipRect(clipMin, clipMax, true);
            for (const QueuedIcon& icon : queue) {
                drawList->AddImage(texture, icon.min, icon.max, icon.uv0, icon.uv1);
            }
            drawList->PopClipRect();
        }
        queue.clear();
    }
}
#pragma endregion

#pragma region File Actions
namespace {
    constexpr int kMaxPreviewBuildsPerFrame = 3;
    constexpr double kMaxPreviewBuildMsPerFrame = 4.0;

    struct FileBrowserPreviewFrameBudget {
        int remainingBuilds = kMaxPreviewBuildsPerFrame;
        std::chrono::steady_clock::time_point frameStart{};
        bool frameStartValid = false;
    };

    int g_texturePreviewGeneration = 0;

    // Upper bound on a thumbnail's long edge. The cells are drawn at roughly 64px, so 256
    // leaves room for larger grid zoom levels while keeping a 4K source from costing 21 MB
    // (2048^2 after Texture's size cap, with mips) to show as an icon.
    constexpr int kTexturePreviewMaxDim = 256;

    bool PreviewGlContextLive() {
#ifdef __ANDROID__
        return true;
#else
        return glfwGetCurrentContext() != nullptr;
#endif
    }

    // A thumbnail used to draw the scene texture's GL id straight from
    // Renderer::getTexture, which cost twice over. The full-resolution texture stayed
    // resident in the scene cache just to fill a ~64px cell, and the id was captured once
    // and then drawn for up to 1800 frames while the renderer - which refreshes an entry's
    // LRU stamp only inside getTexture - saw that entry as idle and was free to evict it.
    // Drawing a deleted name samples black, or shows whatever texture later inherited the
    // recycled id. Previews that this editor can decode itself now own a small copy;
    // formats only the engine decoders handle are still borrowed, but re-requested every
    // frame so the renderer sees them as in use.
    struct CachedTexturePreview {
        fs::file_time_type stamp{};
        GLuint textureId = 0;
        int width = 0;
        int height = 0;
        int lastUsedFrame = 0;
        int generation = 0;
        bool attempted = false;
        bool ready = false;
        bool ownsTexture = false;

        CachedTexturePreview() = default;
        ~CachedTexturePreview() { releaseTexture(); }
        CachedTexturePreview(const CachedTexturePreview&) = delete;
        CachedTexturePreview& operator=(const CachedTexturePreview&) = delete;
        CachedTexturePreview(CachedTexturePreview&& other) noexcept { takeFrom(other); }
        CachedTexturePreview& operator=(CachedTexturePreview&& other) noexcept {
            if (this != &other) {
                releaseTexture();
                takeFrom(other);
            }
            return *this;
        }

        void releaseTexture() {
            // This cache is a function-local static, so it is destroyed after the window and
            // its context are gone. Deleting a name on a dead context is undefined, hence the
            // liveness check rather than an unconditional glDeleteTextures.
            if (ownsTexture && textureId != 0 && PreviewGlContextLive()) {
                glDeleteTextures(1, &textureId);
            }
            textureId = 0;
            ownsTexture = false;
        }

    private:
        void takeFrom(CachedTexturePreview& other) {
            stamp = other.stamp;
            textureId = other.textureId;
            width = other.width;
            height = other.height;
            lastUsedFrame = other.lastUsedFrame;
            generation = other.generation;
            attempted = other.attempted;
            ready = other.ready;
            ownsTexture = other.ownsTexture;
            other.textureId = 0;
            other.ownsTexture = false;
        }
    };

    struct CachedModelPreview {
        fs::file_time_type stamp{};
        bool loaded = false;
        bool attempted = false;
        bool isObj = false;
        int meshId = -1;
        int previewSlot = -1;
        GLuint previewTextureId = 0;
        int previewWidth = 0;
        int previewHeight = 0;
        int lastUsedFrame = 0;
        glm::vec3 boundsMin = glm::vec3(FLT_MAX);
        glm::vec3 boundsMax = glm::vec3(-FLT_MAX);
    };

    struct CachedVideoPreview {
        fs::file_time_type stamp{};
        int lastUsedFrame = 0;
        bool attempted = false;
        bool loaded = false;
        std::unique_ptr<VideoPlayer> player;
    };

    struct FileBrowserTextLayoutKey {
        std::string selectionKey;
        int widthBucket = 0;
        bool operator==(const FileBrowserTextLayoutKey& other) const {
            return widthBucket == other.widthBucket && selectionKey == other.selectionKey;
        }
    };

    struct FileBrowserTextLayoutKeyHash {
        size_t operator()(const FileBrowserTextLayoutKey& key) const {
            return std::hash<std::string>{}(key.selectionKey) ^
                   (std::hash<int>{}(key.widthBucket) + 0x9e3779b9 + (std::hash<std::string>{}(key.selectionKey) << 6));
        }
    };

    struct FileBrowserTextLayoutCache {
        std::unordered_map<FileBrowserTextLayoutKey, std::string, FileBrowserTextLayoutKeyHash> labels;
    };

    struct FolderTreeChildCache {
        struct Node {
            std::vector<fs::path> childDirs;
            double lastScanTime = -1.0;
            bool scannedWithHidden = false;
        };
        std::unordered_map<std::string, Node> nodes;

        const std::vector<fs::path>& children(const fs::path& path, bool showHidden, double now) {
            Node& node = nodes[PathToUtf8(path)];
            constexpr double kRescanIntervalSeconds = 1.0;
            const bool stale = node.lastScanTime < 0.0 ||
                               node.scannedWithHidden != showHidden ||
                               (now - node.lastScanTime) > kRescanIntervalSeconds;
            if (!stale) {
                return node.childDirs;
            }

            node.childDirs.clear();
            std::error_code ec;
            for (fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                const fs::directory_entry& entry = *it;
                std::error_code dirEc;
                if (!entry.is_directory(dirEc) || dirEc) {
                    continue;
                }
                const std::string dirName = PathToUtf8(entry.path().filename());
                if (!showHidden && !dirName.empty() && dirName[0] == '.') {
                    continue;
                }
                node.childDirs.push_back(entry.path());
            }
            std::sort(node.childDirs.begin(), node.childDirs.end(),
                      [](const fs::path& a, const fs::path& b) {
                          return PathToUtf8(a.filename()) < PathToUtf8(b.filename());
                      });

            node.lastScanTime = now;
            node.scannedWithHidden = showHidden;
            Modu2DStats::CountDirScan();
            return node.childDirs;
        }
    };

    FileBrowserPreviewFrameBudget& GetFileBrowserPreviewBudget() {
        static FileBrowserPreviewFrameBudget budget;
        return budget;
    }

    void ResetFileBrowserPreviewBudget() {
        FileBrowserPreviewFrameBudget& budget = GetFileBrowserPreviewBudget();
        budget.remainingBuilds = kMaxPreviewBuildsPerFrame;
        budget.frameStartValid = false;
    }

    bool ConsumeFileBrowserPreviewBudget() {
        FileBrowserPreviewFrameBudget& budget = GetFileBrowserPreviewBudget();
        if (budget.remainingBuilds <= 0) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!budget.frameStartValid) {
            budget.frameStart = now;
            budget.frameStartValid = true;
        } else if (std::chrono::duration<double, std::milli>(now - budget.frameStart).count() >=
                   kMaxPreviewBuildMsPerFrame) {
            budget.remainingBuilds = 0;
            return false;
        }
        --budget.remainingBuilds;
        return true;
    }

    uint64_t HashPreviewPath(const fs::path& path) {
        return static_cast<uint64_t>(std::hash<std::string>{}(path.generic_string()));
    }

    int AllocatePreviewSlotForPath(const fs::path& path, int slotBase) {
        return slotBase + static_cast<int>(HashPreviewPath(path) % 8192ull);
    }

    template <typename CacheT>
    void TrimFileBrowserPreviewCache(CacheT& cache, const std::string& keepKey,
                                     size_t maxEntries, int maxIdleFrames) {
        if (cache.size() <= maxEntries) {
            return;
        }

        const int frame = ImGui::GetFrameCount();
        for (auto it = cache.begin(); it != cache.end();) {
            if (it->first != keepKey && frame - it->second.lastUsedFrame > maxIdleFrames) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }

        if (cache.size() <= maxEntries) {
            return;
        }

        std::vector<std::pair<int, std::string>> entries;
        entries.reserve(cache.size());
        for (const auto& [key, preview] : cache) {
            if (key == keepKey) continue;
            entries.emplace_back(preview.lastUsedFrame, key);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;
                  });

        for (const auto& entry : entries) {
            if (cache.size() <= maxEntries) {
                break;
            }
            cache.erase(entry.second);
        }
    }

    // Alpha-weighted box filter, matching BlitIconIntoAtlasCell: averaging straight RGB
    // would let fully transparent texels drag colour into the visible ones and haloes show
    // up on cutout foliage and UI sprites.
    void BoxDownsampleRGBA(const unsigned char* src, int srcW, int srcH,
                           unsigned char* dst, int dstW, int dstH) {
        for (int y = 0; y < dstH; ++y) {
            const int sy0 = static_cast<int>(static_cast<int64_t>(y) * srcH / dstH);
            const int sy1 = std::max(sy0 + 1,
                static_cast<int>(static_cast<int64_t>(y + 1) * srcH / dstH));
            for (int x = 0; x < dstW; ++x) {
                const int sx0 = static_cast<int>(static_cast<int64_t>(x) * srcW / dstW);
                const int sx1 = std::max(sx0 + 1,
                    static_cast<int>(static_cast<int64_t>(x + 1) * srcW / dstW));
                uint64_t accR = 0, accG = 0, accB = 0, accA = 0, samples = 0;
                for (int sy = sy0; sy < sy1 && sy < srcH; ++sy) {
                    const unsigned char* row = src + (static_cast<size_t>(sy) * srcW + sx0) * 4;
                    for (int sx = sx0; sx < sx1 && sx < srcW; ++sx, row += 4) {
                        const uint64_t a = row[3];
                        accR += static_cast<uint64_t>(row[0]) * a;
                        accG += static_cast<uint64_t>(row[1]) * a;
                        accB += static_cast<uint64_t>(row[2]) * a;
                        accA += a;
                        ++samples;
                    }
                }
                unsigned char* out = dst + (static_cast<size_t>(y) * dstW + x) * 4;
                if (samples == 0) {
                    out[0] = out[1] = out[2] = out[3] = 0;
                    continue;
                }
                out[0] = accA ? static_cast<unsigned char>(accR / accA) : 0;
                out[1] = accA ? static_cast<unsigned char>(accG / accA) : 0;
                out[2] = accA ? static_cast<unsigned char>(accB / accA) : 0;
                out[3] = static_cast<unsigned char>(accA / samples);
            }
        }
    }

    // stb_image covers the common cases; .dds and .exr need the engine decoders and are
    // left to the scene cache.
    bool PreviewFormatDecodableHere(const fs::path& path) {
        std::string ext = PathToUtf8(path.extension());
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
               ext == ".bmp" || ext == ".tga" || ext == ".hdr";
    }

    // Decodes at full size (stb cannot decode partially) but uploads only the reduced copy,
    // so the cost is transient CPU work instead of a permanent resident texture.
    GLuint CreateDownscaledPreviewTexture(const fs::path& path, int& outW, int& outH) {
        // The scene textures are loaded bottom-up, and DrawTexturePreview flips V to match.
        // Decoding the same way keeps one set of UVs correct for both preview paths.
        const StbFlipScope flipForPreview(true);
        int srcW = 0, srcH = 0, channels = 0;
        unsigned char* pixels = stbi_load(PathToUtf8(path).c_str(), &srcW, &srcH, &channels, 4);
        if (!pixels || srcW <= 0 || srcH <= 0) {
            if (pixels) stbi_image_free(pixels);
            return 0;
        }

        int dstW = srcW, dstH = srcH;
        const int longEdge = std::max(srcW, srcH);
        if (longEdge > kTexturePreviewMaxDim) {
            const double scale = static_cast<double>(kTexturePreviewMaxDim) / longEdge;
            dstW = std::max(1, static_cast<int>(srcW * scale));
            dstH = std::max(1, static_cast<int>(srcH * scale));
        }

        std::vector<unsigned char> scaled;
        const unsigned char* upload = pixels;
        if (dstW != srcW || dstH != srcH) {
            scaled.assign(static_cast<size_t>(dstW) * dstH * 4, 0);
            BoxDownsampleRGBA(pixels, srcW, srcH, scaled.data(), dstW, dstH);
            upload = scaled.data();
        }

        GLint previousTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        GLuint id = 0;
        glGenTextures(1, &id);
        if (id == 0) {
            stbi_image_free(pixels);
            return 0;
        }
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dstW, dstH, 0, GL_RGBA, GL_UNSIGNED_BYTE, upload);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

        stbi_image_free(pixels);
        outW = dstW;
        outH = dstH;
        return id;
    }

    CachedTexturePreview& GetTexturePreviewData(Renderer& renderer,
                                                const fs::path& path,
                                                const fs::file_time_type& stamp) {
        static std::unordered_map<std::string, CachedTexturePreview> cache;
        constexpr size_t kMaxTexturePreviewEntries = 512;
        const std::string key = PathToUtf8(path);
        CachedTexturePreview& preview = cache[key];
        preview.lastUsedFrame = ImGui::GetFrameCount();
        if (preview.stamp != stamp || preview.generation != g_texturePreviewGeneration) {
            preview = {};
            preview.stamp = stamp;
            preview.generation = g_texturePreviewGeneration;
            preview.lastUsedFrame = ImGui::GetFrameCount();
        }
        if (!preview.attempted && ConsumeFileBrowserPreviewBudget()) {
            preview.attempted = true;
            MODU_PROFILE_SCOPE("Thumbnail Texture Load", ProfilerSampleCategory::UI);
            bool loaded = false;
            if (PreviewFormatDecodableHere(path)) {
                int decodedW = 0, decodedH = 0;
                const GLuint id = CreateDownscaledPreviewTexture(path, decodedW, decodedH);
                if (id != 0) {
                    preview.textureId = id;
                    preview.ownsTexture = true;
                    preview.width = decodedW;
                    preview.height = decodedH;
                    preview.ready = true;
                    loaded = true;
                }
            }
            // Falling through on a failed decode is deliberate: a .png that is really an EXR
            // still reaches the engine decoders, and a genuine failure gets recorded in
            // textureLoadErrors so TexturePreviewError can explain it.
            if (!loaded) {
                if (Texture* tex = renderer.getTexture(key);
                    tex && tex->GetID() != 0 && tex->GetWidth() > 0 && tex->GetHeight() > 0) {
                    preview.textureId = tex->GetID();
                    preview.width = tex->GetWidth();
                    preview.height = tex->GetHeight();
                    preview.ready = true;
                }
            }
        } else if (preview.ready && !preview.ownsTexture) {
            // Borrowed from the scene cache, so re-request it to refresh the renderer's LRU
            // stamp. purgeTextureCacheIfNeeded spares only entries touched this frame and its
            // second pass ignores recency entirely, so without this the id about to be drawn
            // can be deleted underneath us and the cell shows another texture entirely.
            if (Texture* tex = renderer.getTexture(key);
                tex && tex->GetID() != 0 && tex->GetWidth() > 0 && tex->GetHeight() > 0) {
                preview.textureId = tex->GetID();
                preview.width = tex->GetWidth();
                preview.height = tex->GetHeight();
            } else {
                preview.textureId = 0;
                preview.ready = false;
            }
        }
        TrimFileBrowserPreviewCache(cache, key, kMaxTexturePreviewEntries, 1800);
        return preview;
    }

    Texture* GetModularityLogoTexture(Renderer& renderer) {
        static const fs::path kLogoPath("Resources/Engine-Root/Modu-Logo.png");
        return renderer.getTexture(PathToUtf8(kLogoPath));
    }

    CachedVideoPreview& GetVideoPreviewData(const fs::path& path, const fs::file_time_type& stamp) {
        static std::unordered_map<std::string, CachedVideoPreview> cache;
        constexpr size_t kMaxVideoPreviewEntries = 48;
        const std::string key = PathToUtf8(path);
        CachedVideoPreview& preview = cache[key];
        preview.lastUsedFrame = ImGui::GetFrameCount();
        if (preview.stamp != stamp) {
            preview = {};
            preview.stamp = stamp;
            preview.lastUsedFrame = ImGui::GetFrameCount();
        }
        if (!preview.attempted && ConsumeFileBrowserPreviewBudget()) {
            preview.attempted = true;
            MODU_PROFILE_SCOPE("Thumbnail Video Load", ProfilerSampleCategory::UI);
            preview.player = std::make_unique<VideoPlayer>();
            preview.player->SetPlayAudioFromVideo(false);
            preview.player->SetLoop(false);
            preview.loaded = preview.player->LoadVideo(PathToUtf8(path), false) &&
                             preview.player->HasTextureOverride() &&
                             preview.player->GetTextureId() != 0 &&
                             preview.player->GetWidth() > 0 &&
                             preview.player->GetHeight() > 0;
            if (!preview.loaded) {
                preview.player.reset();
            }
        }
        TrimFileBrowserPreviewCache(cache, key, kMaxVideoPreviewEntries, 900);
        return preview;
    }

    CachedModelPreview& GetModelPreviewData(const fs::path& path, const fs::file_time_type& stamp) {
        static std::unordered_map<std::string, CachedModelPreview> cache;
        constexpr size_t kMaxModelPreviewEntries = 128;
        const std::string key = PathToUtf8(path);
        CachedModelPreview& preview = cache[key];
        preview.lastUsedFrame = ImGui::GetFrameCount();
        if (preview.stamp != stamp) {
            const int existingSlot = preview.previewSlot;
            preview = {};
            preview.stamp = stamp;
            preview.previewSlot = existingSlot;
            preview.lastUsedFrame = ImGui::GetFrameCount();
        }
        if (preview.attempted || !ConsumeFileBrowserPreviewBudget()) {
            TrimFileBrowserPreviewCache(cache, key, kMaxModelPreviewEntries, 1200);
            return preview;
        }

        preview.attempted = true;
        MODU_PROFILE_SCOPE("Thumbnail Model Load", ProfilerSampleCategory::UI);
        std::string ext = PathToUtf8(path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        preview.isObj = (ext == ".obj");

        if (preview.isObj) {
            std::string err;
            preview.meshId = g_objLoader.loadOBJ(PathToUtf8(path), err);
            const auto* info = g_objLoader.getMeshInfo(preview.meshId);
            if (preview.meshId >= 0 && info) {
                preview.loaded = true;
                preview.boundsMin = info->boundsMin;
                preview.boundsMax = info->boundsMax;
            }
            TrimFileBrowserPreviewCache(cache, key, kMaxModelPreviewEntries, 1200);
            return preview;
        }

        ModelLoadResult result = getModelLoader().loadModel(PathToUtf8(path));
        const auto* info = getModelLoader().getMeshInfo(result.meshIndex);
        if (result.success && result.meshIndex >= 0 && info) {
            preview.loaded = true;
            preview.meshId = result.meshIndex;
            preview.boundsMin = info->boundsMin;
            preview.boundsMax = info->boundsMax;
        }
        TrimFileBrowserPreviewCache(cache, key, kMaxModelPreviewEntries, 1200);
        return preview;
    }

    std::string TexturePreviewError(Renderer& renderer, const FileBrowser::CachedEntry& entry) {
        CachedTexturePreview& tex = GetTexturePreviewData(renderer, entry.path, entry.lastWriteTime);
        if (!tex.attempted || tex.ready) return std::string();
        return renderer.getTextureLoadError(PathToUtf8(entry.path));
    }

    void DrawTexturePreviewErrorTooltip(Renderer& renderer, const FileBrowser::CachedEntry& entry,
                                        const ImVec2& min, const ImVec2& max) {
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) return;
        if (!ImGui::IsMouseHoveringRect(min, max)) return;
        const std::string reason = TexturePreviewError(renderer, entry);
        if (reason.empty()) return;
        ImGui::SetTooltip("Could not decode this image.\n%s", reason.c_str());
    }

    bool DrawTexturePreview(Renderer& renderer, ImDrawList* drawList, const FileBrowser::CachedEntry& entry,
                            ImVec2 min, ImVec2 max, float rounding) {
        CachedTexturePreview& tex = GetTexturePreviewData(renderer, entry.path, entry.lastWriteTime);
        if (!tex.ready || tex.textureId == 0 || tex.width <= 0 || tex.height <= 0) {
            if (tex.attempted) {
                drawList->AddRectFilled(min, max, IM_COL32(38, 24, 26, 255), rounding);
                drawList->AddRect(min, max, IM_COL32(190, 90, 90, 200), rounding, 0, 1.0f);
                const char* mark = "!";
                const ImVec2 markSize = ImGui::CalcTextSize(mark);
                drawList->AddText(ImVec2((min.x + max.x - markSize.x) * 0.5f,
                                         (min.y + max.y - markSize.y) * 0.5f),
                                  IM_COL32(240, 150, 150, 255), mark);
                return true;
            }
            return false;
        }

        const float availW = max.x - min.x;
        const float availH = max.y - min.y;
        const float scale = std::min(availW / static_cast<float>(tex.width),
                                     availH / static_cast<float>(tex.height));
        const float drawW = static_cast<float>(tex.width) * scale;
        const float drawH = static_cast<float>(tex.height) * scale;
        const ImVec2 imgMin(min.x + (availW - drawW) * 0.5f, min.y + (availH - drawH) * 0.5f);
        const ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);

        drawList->AddRectFilled(min, max, IM_COL32(24, 27, 34, 255), rounding);
        drawList->PushClipRect(min, max, true);
        drawList->AddImage((ImTextureID)(intptr_t)tex.textureId, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0));
        drawList->PopClipRect();
        drawList->AddRect(min, max, IM_COL32(96, 108, 126, 210), rounding, 0, 1.0f);
        return true;
    }

    bool DrawSceneLogoPreview(Renderer& renderer, ImDrawList* drawList, ImVec2 min, ImVec2 max, float rounding) {
        ImTextureID texture{};
        ImVec2 uv0(0.0f, 1.0f);
        ImVec2 uv1(1.0f, 0.0f);
        float srcW = 1.0f;
        float srcH = 1.0f;
        if (FileIcons::TryGetAtlasIcon(FileCategory::Scene, false, texture, uv0, uv1)) {
            srcW = srcH = 1.0f;
        } else {
            Texture* tex = GetModularityLogoTexture(renderer);
            if (!tex || tex->GetID() == 0 || tex->GetWidth() <= 0 || tex->GetHeight() <= 0) {
                return false;
            }
            texture = (ImTextureID)(intptr_t)tex->GetID();
            uv0 = ImVec2(0.0f, 1.0f);
            uv1 = ImVec2(1.0f, 0.0f);
            srcW = static_cast<float>(tex->GetWidth());
            srcH = static_cast<float>(tex->GetHeight());
        }

        const float availW = max.x - min.x;
        const float availH = max.y - min.y;
        const float innerPad = std::min(availW, availH) * 0.08f;
        const ImVec2 paddedMin(min.x + innerPad, min.y + innerPad);
        const ImVec2 paddedMax(max.x - innerPad, max.y - innerPad);
        const float innerW = paddedMax.x - paddedMin.x;
        const float innerH = paddedMax.y - paddedMin.y;
        const float scale = std::min(innerW / srcW, innerH / srcH);
        const float drawW = srcW * scale;
        const float drawH = srcH * scale;
        const ImVec2 imgMin(paddedMin.x + (innerW - drawW) * 0.5f, paddedMin.y + (innerH - drawH) * 0.5f);
        const ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);

        drawList->AddRectFilled(min, max, IM_COL32(24, 27, 34, 255), rounding);
        drawList->PushClipRect(min, max, true);
        drawList->AddImage(texture, imgMin, imgMax, uv0, uv1);
        drawList->PopClipRect();
        drawList->AddRect(min, max, IM_COL32(96, 108, 126, 210), rounding, 0, 1.0f);
        return true;
    }

    bool DrawVideoPreview(Renderer& renderer, ImDrawList* drawList, const FileBrowser::CachedEntry& entry,
                          ImVec2 min, ImVec2 max, float rounding) {
        (void)renderer;
        constexpr float kIconDesignSize = 700.0f;
        ImTextureID iconTexture{};
        ImVec2 iconUv0, iconUv1;
        if (!FileIcons::TryGetAtlasIcon(FileCategory::Video, false, iconTexture, iconUv0, iconUv1)) {
            return false;
        }

        const float availW = max.x - min.x;
        const float availH = max.y - min.y;
        const float iconScale = std::min(availW, availH) / kIconDesignSize;
        const float iconW = kIconDesignSize * iconScale;
        const float iconH = kIconDesignSize * iconScale;
        const ImVec2 iconMin(min.x + (availW - iconW) * 0.5f, min.y + (availH - iconH) * 0.5f);
        const ImVec2 iconMax(iconMin.x + iconW, iconMin.y + iconH);

        drawList->AddRectFilled(min, max, IM_COL32(24, 27, 34, 255), rounding);
        drawList->PushClipRect(min, max, true);

        CachedVideoPreview& preview = GetVideoPreviewData(entry.path, entry.lastWriteTime);
        if (preview.loaded && preview.player) {
            constexpr float kFrameLayerWidth = 614.0f;
            constexpr float kFrameLayerHeight = 426.0f;
            const ImVec2 frameMin(
                iconMin.x + ((kIconDesignSize - kFrameLayerWidth) * 0.5f) * iconScale,
                iconMin.y + ((kIconDesignSize - kFrameLayerHeight) * 0.5f) * iconScale);
            const ImVec2 frameMax(frameMin.x + kFrameLayerWidth * iconScale,
                                  frameMin.y + kFrameLayerHeight * iconScale);
            const float frameW = frameMax.x - frameMin.x;
            const float frameH = frameMax.y - frameMin.y;
            const float sourceAspect = static_cast<float>(preview.player->GetWidth()) /
                                       static_cast<float>(std::max(1, preview.player->GetHeight()));
            const float targetAspect = frameW / std::max(1.0f, frameH);
            ImVec2 uvMin(0.0f, 0.0f);
            ImVec2 uvMax(1.0f, 1.0f);
            if (sourceAspect > targetAspect) {
                const float visibleU = targetAspect / std::max(0.001f, sourceAspect);
                const float crop = (1.0f - visibleU) * 0.5f;
                uvMin.x = crop;
                uvMax.x = 1.0f - crop;
            } else if (sourceAspect < targetAspect) {
                const float visibleV = sourceAspect / std::max(0.001f, targetAspect);
                const float crop = (1.0f - visibleV) * 0.5f;
                uvMin.y = crop;
                uvMax.y = 1.0f - crop;
            }
            drawList->AddImage((ImTextureID)(intptr_t)preview.player->GetTextureId(),
                               frameMin, frameMax, uvMin, uvMax);
        }

        drawList->AddImage(iconTexture, iconMin, iconMax, iconUv0, iconUv1);
        drawList->PopClipRect();
        drawList->AddRect(min, max, IM_COL32(96, 108, 126, 210), rounding, 0, 1.0f);
        return true;
    }

    bool DrawModelPreview(Renderer& renderer, ImDrawList* drawList, const FileBrowser::CachedEntry& entry,
                          ImVec2 min, ImVec2 max, int previewSlotBase, float rounding) {
        CachedModelPreview& preview = GetModelPreviewData(entry.path, entry.lastWriteTime);
        if (!preview.loaded || preview.meshId < 0) {
            return false;
        }

        if (preview.previewSlot < 0) {
            preview.previewSlot = AllocatePreviewSlotForPath(entry.path, previewSlotBase);
        }

        const int width = std::max(48, static_cast<int>(max.x - min.x));
        const int height = std::max(48, static_cast<int>(max.y - min.y));
        if ((preview.previewTextureId == 0 || preview.previewWidth != width || preview.previewHeight != height) &&
            ConsumeFileBrowserPreviewBudget()) {
            MODU_PROFILE_SCOPE("Thumbnail Model Render", ProfilerSampleCategory::UI);
            preview.previewWidth = width;
            preview.previewHeight = height;

            glm::vec3 boundsMin = preview.boundsMin;
            glm::vec3 boundsMax = preview.boundsMax;
            if (boundsMin.x >= boundsMax.x || boundsMin.y >= boundsMax.y || boundsMin.z >= boundsMax.z) {
                boundsMin = glm::vec3(-0.5f);
                boundsMax = glm::vec3(0.5f);
            }
            const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
            const glm::vec3 size = glm::max(boundsMax - boundsMin, glm::vec3(0.001f));
            const float radius = std::max({ size.x, size.y, size.z }) * 0.5f;
            const float uniformScale = (radius > 0.0f) ? (1.5f / radius) : 1.0f;
            const float scaledHalfHeight = size.y * 0.5f * uniformScale;

            SceneObject obj("FilePreviewModel", ObjectType::Model, -1);
            obj.hasRenderer = true;
            obj.renderType = preview.isObj ? RenderType::OBJMesh : RenderType::Model;
            obj.meshId = preview.meshId;
            obj.position = -center;
            obj.rotation = glm::vec3(-18.0f, 32.0f, 0.0f);
            obj.scale = glm::vec3(uniformScale);
            obj.material.color = glm::vec3(0.90f, 0.92f, 0.96f);
            obj.material.ambientStrength = 0.34f;
            obj.material.specularStrength = 0.22f;
            obj.material.shininess = 24.0f;
            obj.albedoTexturePath.clear();
            obj.overlayTexturePath.clear();
            obj.normalMapPath.clear();

            SceneObject ground("FilePreviewGround", ObjectType::Plane, -2);
            ground.hasRenderer = true;
            ground.renderType = RenderType::Plane;
            ground.position = glm::vec3(0.0f, -scaledHalfHeight - 0.28f, 0.0f);
            ground.rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
            ground.scale = glm::vec3(3.0f, 3.0f, 1.0f);
            ground.material.color = glm::vec3(0.22f, 0.24f, 0.28f);
            ground.material.ambientStrength = 0.22f;
            ground.material.specularStrength = 0.02f;
            ground.material.shininess = 4.0f;

            Camera cam;
            const glm::vec3 target(0.0f, 0.0f, 0.0f);
            cam.position = glm::vec3(radius * 1.9f, radius * 1.05f, radius * 2.3f + 0.4f);
            cam.front = glm::normalize(target - cam.position);
            glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
            glm::vec3 right = glm::cross(cam.front, worldUp);
            if (glm::dot(right, right) < 1e-6f) {
                worldUp = glm::vec3(0.0f, 0.0f, 1.0f);
                right = glm::cross(cam.front, worldUp);
            }
            right = glm::normalize(right);
            cam.up = glm::normalize(glm::cross(right, cam.front));

            std::vector<SceneObject> scene = { ground, obj };
            preview.previewTextureId = renderer.renderScenePreview(
                cam, scene, width, height, 32.0f, 0.1f, 100.0f, false, preview.previewSlot);
        }

        if (preview.previewTextureId == 0) {
            return false;
        }

        drawList->AddRectFilled(min, max, IM_COL32(20, 23, 29, 255), rounding);
        drawList->PushClipRect(min, max, true);
        drawList->AddImage((ImTextureID)(intptr_t)preview.previewTextureId, min, max, ImVec2(0, 1), ImVec2(1, 0));
        drawList->PopClipRect();
        drawList->AddRect(min, max, IM_COL32(96, 108, 126, 210), rounding, 0, 1.0f);
        return true;
    }

    const std::string& GetTruncatedLabel(FileBrowserTextLayoutCache& cache,
                                         const FileBrowser::CachedEntry& entry,
                                         float maxTextWidth) {
        const int widthBucket = std::max(1, static_cast<int>(std::round(maxTextWidth)));
        const FileBrowserTextLayoutKey key{ entry.selectionKey, widthBucket };
        auto it = cache.labels.find(key);
        if (it != cache.labels.end()) {
            return it->second;
        }

        std::string displayName = entry.filename;
        if (ImGui::CalcTextSize(displayName.c_str()).x > maxTextWidth) {
            while (displayName.size() > 3) {
                displayName.pop_back();
                const std::string candidate = displayName + "...";
                if (ImGui::CalcTextSize(candidate.c_str()).x <= maxTextWidth) {
                    displayName = candidate;
                    break;
                }
            }
        }
        return cache.labels.emplace(key, std::move(displayName)).first->second;
    }

    enum class CreateKind {
        Folder,
        ModuMakoScript,
        ModuCppScript,
        CppScript,
        CScript,
        Header,
        Text,
        Json,
        Shader,
        CombinedShader
    };

    std::string fileSelectionKey(const fs::path& path) {
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(path, ec);
        if (!ec) {
            return canonical.generic_string();
        }
        return fs::absolute(path, ec).lexically_normal().generic_string();
    }

    fs::path makeUniquePath(const fs::path& basePath) {
        if (!fs::exists(basePath)) {
            return basePath;
        }
        std::string stem = PathToUtf8(basePath.stem());
        std::string ext = PathToUtf8(basePath.extension());
        for (int i = 1; i < 10000; ++i) {
            fs::path candidate = basePath.parent_path() / (stem + std::to_string(i) + ext);
            if (!fs::exists(candidate)) {
                return candidate;
            }
        }
        return basePath;
    }

    bool writeFileContents(const fs::path& path, const std::string& contents) {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return false;
        }
        out << contents;
        return static_cast<bool>(out);
    }

    bool createScrollingShaderPair(const fs::path& dir,
                                   fs::path& outVertPath,
                                   fs::path& outFragPath,
                                   std::string& error) {
        static const char* kVertSource =
            "#version 330 core\n"
            "layout (location = 0) in vec3 aPos;\n"
            "layout (location = 1) in vec3 aNormal;\n"
            "layout (location = 2) in vec2 aTexCoord;\n"
            "layout (location = 3) in ivec4 aBoneIds;\n"
            "layout (location = 4) in vec4 aBoneWeights;\n"
            "\n"
            "out vec3 FragPos;\n"
            "out vec3 Normal;\n"
            "out vec2 TexCoord;\n"
            "\n"
            "uniform mat4 model;\n"
            "uniform mat4 view;\n"
            "uniform mat4 projection;\n"
            "uniform mat4 bones[128];\n"
            "uniform int boneCount;\n"
            "uniform bool useSkinning;\n"
            "\n"
            "void main()\n"
            "{\n"
            "    vec4 localPos = vec4(aPos, 1.0);\n"
            "    vec3 localNormal = aNormal;\n"
            "\n"
            "    if (useSkinning) {\n"
            "        vec4 skinnedPos = vec4(0.0);\n"
            "        vec3 skinnedNormal = vec3(0.0);\n"
            "        for (int i = 0; i < 4; ++i) {\n"
            "            int id = aBoneIds[i];\n"
            "            float w = aBoneWeights[i];\n"
            "            if (w <= 0.0 || id < 0 || id >= boneCount) continue;\n"
            "            mat4 b = bones[id];\n"
            "            skinnedPos += (b * localPos) * w;\n"
            "            skinnedNormal += mat3(b) * localNormal * w;\n"
            "        }\n"
            "        localPos = skinnedPos;\n"
            "        localNormal = skinnedNormal;\n"
            "    }\n"
            "\n"
            "    vec4 worldPos = model * localPos;\n"
            "    FragPos = vec3(worldPos);\n"
            "    Normal = mat3(transpose(inverse(model))) * localNormal;\n"
            "    TexCoord = aTexCoord;\n"
            "    gl_Position = projection * view * worldPos;\n"
            "}\n";

        static const char* kFragSource =
            "#version 330 core\n"
            "out vec4 FragColor;\n"
            "\n"
            "in vec3 FragPos;\n"
            "in vec3 Normal;\n"
            "in vec2 TexCoord;\n"
            "\n"
            "uniform sampler2D texture1;\n"
            "uniform sampler2D overlayTex;\n"
            "uniform float mixAmount = 0.2;\n"
            "uniform bool hasOverlay = false;\n"
            "uniform bool unlit = false;\n"
            "\n"
            "uniform float uTime = 0.0;\n"
            "uniform vec3 materialColor = vec3(1.0);\n"
            "uniform float ambientStrength = 0.2;\n"
            "uniform float specularStrength = 0.5;\n"
            "uniform float shininess = 32.0;\n"
            "uniform vec3 viewPos;\n"
            "\n"
            "void main()\n"
            "{\n"
            "    float speed = mix(0.08, 1.2, clamp(mixAmount, 0.0, 1.0));\n"
            "    vec2 baseDir = normalize(vec2(1.0, 0.3));\n"
            "    vec2 baseUV = TexCoord + baseDir * (uTime * speed);\n"
            "    vec4 baseSample = texture(texture1, baseUV);\n"
            "\n"
            "    vec3 color = baseSample.rgb;\n"
            "    if (hasOverlay) {\n"
            "        vec2 overlayDir = normalize(vec2(-0.65, 1.0));\n"
            "        vec2 overlayUV = TexCoord + overlayDir * (uTime * speed * 0.65);\n"
            "        vec3 overlayColor = texture(overlayTex, overlayUV).rgb;\n"
            "        color = mix(color, overlayColor, clamp(mixAmount, 0.0, 1.0));\n"
            "    }\n"
            "    color *= materialColor;\n"
            "\n"
            "    if (unlit) {\n"
            "        FragColor = vec4(color, baseSample.a);\n"
            "        return;\n"
            "    }\n"
            "\n"
            "    vec3 N = normalize(Normal);\n"
            "    vec3 L = normalize(vec3(0.35, 0.9, 0.2));\n"
            "    vec3 V = normalize(viewPos - FragPos);\n"
            "    vec3 H = normalize(L + V);\n"
            "\n"
            "    float diffuse = max(dot(N, L), 0.0);\n"
            "    float spec = pow(max(dot(N, H), 0.0), max(shininess, 1.0)) * clamp(specularStrength, 0.0, 2.0);\n"
            "    vec3 lit = color * (clamp(ambientStrength, 0.0, 1.0) + diffuse) + vec3(spec);\n"
            "\n"
            "    FragColor = vec4(lit, baseSample.a);\n"
            "}\n";

        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            error = ec.message();
            return false;
        }

        outVertPath = makeUniquePath(dir / "scroll_texture_vert.glsl");
        outFragPath = makeUniquePath(dir / "scroll_texture_frag.glsl");

        if (!writeFileContents(outVertPath, kVertSource)) {
            error = std::strerror(errno);
            return false;
        }
        if (!writeFileContents(outFragPath, kFragSource)) {
            error = std::strerror(errno);
            std::error_code removeEc;
            fs::remove(outVertPath, removeEc);
            return false;
        }
        return true;
    }

    bool openPathInShell(const fs::path& path) {
        #ifdef _WIN32
        std::wstring widePath = path.wstring();
        HINSTANCE result = ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
        #elif defined(__ANDROID__)
        (void)path;
        return false;
        #elif __linux__
        std::string cmd = "xdg-open \"" + PathToUtf8(path) + "\"";
        return std::system(cmd.c_str()) == 0;
        #else
        return false;
        #endif
    }

    void openPathInFileManager(const fs::path& path) {
        #ifdef _WIN32
        std::wstring widePath = path.wstring();
        if (fs::is_directory(path)) {
            ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            std::wstring args = L"/select,\"" + widePath + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        #elif defined(__ANDROID__)
        (void)path;
        #elif __linux__
        fs::path folder = fs::is_directory(path) ? path : path.parent_path();
        std::string cmd = "xdg-open \"" + PathToUtf8(folder) + "\"";
        std::system(cmd.c_str());
        #endif
    }

#ifdef _WIN32
    void openPathWithDialog(const fs::path& path) {
        std::wstring widePath = path.wstring();
        std::wstring args = L"shell32.dll,OpenAs_RunDLL \"" + widePath + L"\"";
        ShellExecuteW(nullptr, nullptr, L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
#endif

    [[maybe_unused]] std::string trimWhitespace(std::string value) {
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    #if defined(__linux__) && !defined(__ANDROID__)
    std::string shellQuote(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('\'');
        for (char c : value) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    }

    bool commandExists(const char* command) {
        if (!command || !*command) {
            return false;
        }
        std::string check = "command -v ";
        check += command;
        check += " >/dev/null 2>&1";
        return std::system(check.c_str()) == 0;
    }

    std::optional<std::string> runSelectionDialogCommand(const std::string& command) {
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return std::nullopt;
        }
        std::string output;
        char buffer[512];
        while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe)) {
            output += buffer;
        }
        pclose(pipe);
        output = trimWhitespace(output);
        if (output.empty()) {
            return std::nullopt;
        }
        return output;
    }
    #endif

#ifdef _WIN32
    class ScopedComApartment {
    public:
        ScopedComApartment() {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ownsApartment = SUCCEEDED(hr);
        }
        ~ScopedComApartment() {
            if (ownsApartment) {
                CoUninitialize();
            }
        }
        ScopedComApartment(const ScopedComApartment&) = delete;
        ScopedComApartment& operator=(const ScopedComApartment&) = delete;

    private:
        bool ownsApartment = false;
    };

    HWND activeEditorWindow() {
        return GetActiveWindow();
    }

    int CALLBACK browseSetInitialSelection(HWND dialog, UINT message, LPARAM, LPARAM userData) {
        if (message == BFFM_INITIALIZED && userData != 0) {
            SendMessageW(dialog, BFFM_SETSELECTIONW, TRUE, userData);
        }
        return 0;
    }
#endif

    [[maybe_unused]] std::optional<fs::path> chooseImportFilePath(const fs::path& initialDir) {
        #ifdef _WIN32
        ScopedComApartment comApartment;
        std::wstring initialDirWide = initialDir.wstring();
        std::vector<wchar_t> fileBuffer(32768, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = activeEditorWindow();
        ofn.lpstrFilter = L"All Files\0*.*\0";
        ofn.lpstrFile = fileBuffer.data();
        ofn.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        ofn.lpstrInitialDir = initialDirWide.empty() ? nullptr : initialDirWide.c_str();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn) == TRUE) {
            return fs::path(fileBuffer.data());
        }
        return std::nullopt;
        #elif defined(__ANDROID__)
        (void)initialDir;
        return std::nullopt;
        #elif __linux__
        const fs::path initial = initialDir.empty() ? fs::current_path() : initialDir;
        const std::string initialString = PathToUtf8(initial);
        const std::string initialForDialog =
            (!initialString.empty() && initialString.back() == '/') ? initialString : (initialString + "/");

        if (commandExists("zenity")) {
            std::string cmd = "zenity --file-selection --filename=" + shellQuote(initialForDialog) + " 2>/dev/null";
            if (auto selected = runSelectionDialogCommand(cmd)) {
                return fs::path(*selected);
            }
        }
        if (commandExists("kdialog")) {
            std::string cmd = "kdialog --getopenfilename " + shellQuote(initialString) + " 2>/dev/null";
            if (auto selected = runSelectionDialogCommand(cmd)) {
                return fs::path(*selected);
            }
        }
        return std::nullopt;
        #else
        (void)initialDir;
        return std::nullopt;
        #endif
    }

    [[maybe_unused]] std::optional<fs::path> chooseImportFolderPath(const fs::path& initialDir) {
        #ifdef _WIN32
        ScopedComApartment comApartment;
        std::wstring initialDirWide = initialDir.wstring();
        BROWSEINFOW info{};
        info.hwndOwner = activeEditorWindow();
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        info.lpszTitle = L"Select Folder";
        if (!initialDirWide.empty()) {
            info.lpfn = browseSetInitialSelection;
            info.lParam = reinterpret_cast<LPARAM>(initialDirWide.c_str());
        }
        LPITEMIDLIST selected = SHBrowseForFolderW(&info);
        if (!selected) {
            return std::nullopt;
        }
        std::array<wchar_t, MAX_PATH> folderPath{};
        std::optional<fs::path> result;
        if (SHGetPathFromIDListW(selected, folderPath.data())) {
            result = fs::path(folderPath.data());
        }
        CoTaskMemFree(selected);
        return result;
        #elif defined(__ANDROID__)
        (void)initialDir;
        return std::nullopt;
        #elif __linux__
        const fs::path initial = initialDir.empty() ? fs::current_path() : initialDir;
        const std::string initialString = PathToUtf8(initial);
        const std::string initialForDialog =
            (!initialString.empty() && initialString.back() == '/') ? initialString : (initialString + "/");

        if (commandExists("zenity")) {
            std::string cmd = "zenity --file-selection --directory --filename=" + shellQuote(initialForDialog) + " 2>/dev/null";
            if (auto selected = runSelectionDialogCommand(cmd)) {
                return fs::path(*selected);
            }
        }
        if (commandExists("kdialog")) {
            std::string cmd = "kdialog --getexistingdirectory " + shellQuote(initialString) + " 2>/dev/null";
            if (auto selected = runSelectionDialogCommand(cmd)) {
                return fs::path(*selected);
            }
        }
        return std::nullopt;
        #else
        (void)initialDir;
        return std::nullopt;
        #endif
    }

    bool supportsNativeImportPathPicker() {
        #ifdef _WIN32
        return true;
        #elif defined(__ANDROID__)
        return Modularity::AndroidRuntime::SupportsFilePicker();
        #elif __linux__
        return commandExists("zenity") || commandExists("kdialog");
        #else
        return false;
        #endif
    }
}
#pragma endregion

#pragma region File Browser Panel
// Uses FileBrowser state for navigation, selection, and drag-drop.
void Engine::renderFileBrowserPanel() {
    const auto __fbStart = std::chrono::steady_clock::now();
    ResetFileBrowserPreviewBudget();
    ImGui::Begin(Loc::Window("WINDOW_PROJECT", "Project"), &showFileBrowser);

    if (fileBrowser.needsRefresh) {
        const auto __fbRefStart = std::chrono::steady_clock::now();
        fileBrowser.refresh();
        const double __fbRefMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - __fbRefStart).count();
        if (__fbRefMs > Modularity::HardwareProfile::TimingBudget(50.0)) {
            std::fprintf(stderr, "[ModuTimer] fileBrowser.refresh %.1f ms\n", __fbRefMs);
        }
    }
    fileBrowser.pollExternalChanges(ImGui::GetTime());

    static bool showDeletePopup = false;
    static bool showRenamePopup = false;
    static bool triggerDeletePopup = false;
    static bool triggerRenamePopup = false;
    static bool showReloadScriptsPopup = false;
    static bool triggerReloadScriptsPopup = false;
    static fs::path pendingDeletePath;
    static fs::path pendingRenamePath;
    static char renameName[256] = "";
    static std::vector<fs::path> fileClipboard;
    static bool fileClipboardIsCut = false;

    static bool navSuppressHover = false;
    if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        navSuppressHover = false;
    }

    fileBrowserPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool fileKeysActive = fileBrowserPanelFocused && !ImGui::GetIO().WantTextInput;
    const bool fileContextMenuKeyRequested = fileKeysActive &&
        !fileBrowser.selectedFile.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_Menu, false) ||
         (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)));
    static bool showImportAssetsPopup = false;
    static bool triggerImportAssetsPopup = false;
    static fs::path pendingImportTargetPath;
    static char importAssetPaths[4096] = "";
    static std::vector<fs::path> pendingLanguageTranslationFiles;
    static std::vector<std::string> pendingLanguageTranslationSourceIds;
    static bool triggerImportLanguagePopup = false;
    static bool importLanguageDontAskAgain = false;
#ifdef __ANDROID__
    static bool androidImportPickerPending = false;
    static fs::path androidImportPickerTargetPath;
#endif
    bool settingsDirty = false;

    auto openEntry = [&](const fs::directory_entry& entry) {
        if (entry.is_directory()) {
            fileBrowser.navigateTo(entry.path());
            return;
        }
        if (fileBrowser.isTextureFile(entry)) {
            if (hasSpriteEditorPackage()) {
                loadPixelSpriteDocument(entry.path());
            } else {
                openPathInShell(entry.path());
            }
            return;
        }
        if (fileBrowser.isModelFile(entry)) {
            bool isObj = fileBrowser.isOBJFile(entry);
            std::string defaultName = PathToUtf8(entry.path().stem());
            if (isObj) {
                pendingOBJPath = PathToUtf8(entry.path());
                strncpy(importOBJName, defaultName.c_str(), sizeof(importOBJName) - 1);
                importOBJName[sizeof(importOBJName) - 1] = '\0';
                showImportOBJDialog = true;
            } else {
                pendingModelPath = PathToUtf8(entry.path());
                strncpy(importModelName, defaultName.c_str(), sizeof(importModelName) - 1);
                importModelName[sizeof(importModelName) - 1] = '\0';
                showImportModelDialog = true;
            }
            return;
        }
        const std::string packageExt = PathToUtf8(entry.path().extension());
        if (packageExt == ".modupak") {
            openModuPakImportDialog(entry.path());
            return;
        }
        if (packageExt == ".moduobj") {
            // Opens for editing, the way double-clicking a scene loads it - a packaged
            // object is the thing you edit, and instancing one into the scene is what
            // dragging it does. Import stays on the right-click menu for the cases
            // where a copy in the current scene really is what was wanted.
            openModuObjEditor(entry.path());
            return;
        }
        if (fileBrowser.getFileCategory(entry) == FileCategory::Material) {
            if (SceneObject* sel = getSelectedObject()) {
                sel->materialPath = PathToUtf8(entry.path());
                loadMaterialFromFile(*sel);
            }
            return;
        }
        if (fileBrowser.isSceneFile(entry)) {
            std::string sceneName = PathToUtf8(entry.path().stem());
            loadScene(sceneName);
            logToConsole("Loaded scene: " + sceneName);
            return;
        }
        FileCategory category = fileBrowser.getFileCategory(entry);
        if (category == FileCategory::Video) {
            return;
        }
        if (category == FileCategory::Script || category == FileCategory::Shader) {
            openScriptInEditor(entry.path());
            return;
        }
        openPathInShell(entry.path());
    };

    auto createEntry = [&](const fs::path& dir, CreateKind kind, const std::string& name) {
        fs::path baseDir = dir.empty() ? fileBrowser.currentPath : dir;
        fs::path target = baseDir / name;
        const bool createScript =
            kind == CreateKind::ModuMakoScript || kind == CreateKind::ModuCppScript ||
            kind == CreateKind::CppScript || kind == CreateKind::CScript;
        if (kind != CreateKind::Folder && target.extension().empty()) {
            switch (kind) {
                case CreateKind::ModuMakoScript: target += ".modumako"; break;
                case CreateKind::ModuCppScript: target += ".moducpp"; break;
                case CreateKind::CppScript: target += ".cpp"; break;
                case CreateKind::CScript: target += ".c"; break;
                case CreateKind::Header: target += ".h"; break;
                case CreateKind::Text: target += ".txt"; break;
                case CreateKind::Json: target += ".json"; break;
                case CreateKind::Shader: target += ".glsl"; break;
                case CreateKind::CombinedShader: target += ".shader"; break;
                default: break;
            }
        }
        if (!createScript) {
            target = makeUniquePath(target);
        }

        std::error_code ec;
        if (!createScript && !baseDir.empty() && !fs::exists(baseDir)) {
            fs::create_directories(baseDir, ec);
        }
        if (!createScript && !baseDir.empty() && !fs::exists(baseDir)) {
            addConsoleMessage("Create failed: target folder missing " + PathToUtf8(baseDir),
                              ConsoleMessageType::Error);
            return false;
        }

        bool created = false;
        if (kind == CreateKind::Folder) {
            created = fs::create_directories(target, ec);
        } else {
            if (createScript) {
                ScriptScaffoldKind scaffoldKind = ScriptScaffoldKind::ModuCpp;
                switch (kind) {
                    case CreateKind::ModuMakoScript: scaffoldKind = ScriptScaffoldKind::ModuMako; break;
                    case CreateKind::ModuCppScript: scaffoldKind = ScriptScaffoldKind::ModuCpp; break;
                    case CreateKind::CppScript: scaffoldKind = ScriptScaffoldKind::Cpp; break;
                    case CreateKind::CScript: scaffoldKind = ScriptScaffoldKind::C; break;
                    default: break;
                }
                ScriptLanguage ignoredLanguage = ScriptLanguage::Cpp;
                std::string ignoredManagedType;
                std::string createError;
                created = createScriptAsset(scaffoldKind,
                                            PathToUtf8(target.stem()),
                                            baseDir,
                                            target,
                                            ignoredLanguage,
                                            ignoredManagedType,
                                            createError);
                if (!created) {
                    addConsoleMessage("Create failed: " + createError, ConsoleMessageType::Error);
                    return false;
                }
            } else {
                std::string contents;
                switch (kind) {
                    case CreateKind::Header:
                        contents = "#pragma once\n";
                        break;
                    case CreateKind::Json:
                        contents = "{\n}\n";
                        break;
                    case CreateKind::Shader:
                        contents =
                            "// Shader entry point\n"
                            "void main() {\n"
                            "}\n";
                        break;
                    case CreateKind::CombinedShader:
                        contents =
                            "#shader vertex\n"
                            "#version 330 core\n"
                            "layout (location = 0) in vec3 aPos;\n"
                            "layout (location = 1) in vec3 aNormal;\n"
                            "layout (location = 2) in vec2 aTexCoord;\n"
                            "\n"
                            "uniform mat4 model;\n"
                            "uniform mat4 view;\n"
                            "uniform mat4 projection;\n"
                            "\n"
                            "out vec2 TexCoord;\n"
                            "\n"
                            "void main()\n"
                            "{\n"
                            "    TexCoord = aTexCoord;\n"
                            "    gl_Position = projection * view * model * vec4(aPos, 1.0);\n"
                            "}\n"
                            "\n"
                            "#shader fragment\n"
                            "#version 330 core\n"
                            "out vec4 FragColor;\n"
                            "\n"
                            "in vec2 TexCoord;\n"
                            "\n"
                            "uniform sampler2D texture1;\n"
                            "uniform vec3 materialColor = vec3(1.0);\n"
                            "\n"
                            "void main()\n"
                            "{\n"
                            "    FragColor = texture(texture1, TexCoord) * vec4(materialColor, 1.0);\n"
                            "}\n";
                        break;
                    case CreateKind::Text:
                    default:
                        contents.clear();
                        break;
                }
                created = writeFileContents(target, contents);
            }
        }

        if (created) {
            fileBrowser.selectedFile = target;
            fileBrowser.selectedFiles.clear();
            fileBrowser.selectedFileKeys.clear();
            fileBrowser.selectedFiles.push_back(target);
            fileBrowser.selectedFileKeys.insert(fileSelectionKey(target));
            fileBrowser.needsRefresh = true;
            fileBrowser.refresh();
            addConsoleMessage("Created: " + PathToUtf8(target), ConsoleMessageType::Success);
            return true;
        }

        std::string reason = ec ? ec.message() : std::strerror(errno);
        if (kind == CreateKind::Folder) {
            addConsoleMessage("Create failed: unable to create folder " + PathToUtf8(target) + " (" + reason + ")",
                              ConsoleMessageType::Error);
        } else {
            addConsoleMessage("Create failed: unable to write " + PathToUtf8(target) + " (" + reason + ")",
                              ConsoleMessageType::Error);
        }
        return false;
    };

    auto drawCreateMenu = [&](const fs::path& dir) {
        if (ImGui::MenuItem("Folder")) {
            createEntry(dir, CreateKind::Folder, "New Folder");
        }

        if (ImGui::BeginMenu("Scripts")) {
            if (ImGui::MenuItem("ModuMAKO Script")) {
                createEntry(dir, CreateKind::ModuMakoScript, "NewScript.modumako");
            }
            if (ImGui::MenuItem("ModuCPP Script")) {
                createEntry(dir, CreateKind::ModuCppScript, "NewScript.moducpp");
            }
            if (ImGui::MenuItem("C++ Script")) {
                createEntry(dir, CreateKind::CppScript, "NewScript.cpp");
            }
            if (ImGui::MenuItem("C Script")) {
                createEntry(dir, CreateKind::CScript, "NewScript.c");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Code")) {
            if (ImGui::MenuItem("Header")) {
                createEntry(dir, CreateKind::Header, "NewHeader.h");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Data")) {
            if (ImGui::MenuItem("Text File")) {
                createEntry(dir, CreateKind::Text, "NewFile.txt");
            }
            if (ImGui::MenuItem("JSON File")) {
                createEntry(dir, CreateKind::Json, "NewData.json");
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Graphics")) {
            if (ImGui::MenuItem("Shader")) {
                createEntry(dir, CreateKind::Shader, "NewShader.glsl");
            }
            if (ImGui::MenuItem("Combined Shader")) {
                createEntry(dir, CreateKind::CombinedShader, "NewShader.shader");
            }
            if (ImGui::MenuItem("Scrolling Shader Pair")) {
                fs::path vertPath;
                fs::path fragPath;
                std::string error;
                if (createScrollingShaderPair(dir, vertPath, fragPath, error)) {
                    fileBrowser.needsRefresh = true;
                    addConsoleMessage("Created scrolling shaders: " + PathToUtf8(vertPath.filename()) +
                                      ", " + PathToUtf8(fragPath.filename()),
                                      ConsoleMessageType::Success);
                } else {
                    addConsoleMessage("Failed to create scrolling shaders in " +
                                      PathToUtf8(dir) + " (" + error + ")",
                                      ConsoleMessageType::Error);
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Assets")) {
            if (ImGui::MenuItem("Material")) {
                fs::path target = dir / "NewMaterial.mat";
                int counter = 1;
                while (fs::exists(target)) {
                    target = dir / ("NewMaterial" + std::to_string(counter++) + ".mat");
                }
                SceneObject temp("Material", ObjectType::Cube, -1);
                temp.materialPath = PathToUtf8(target);
                saveMaterialToFile(temp);
                fileBrowser.needsRefresh = true;
            }
            ImGui::EndMenu();
        }
    };

    auto normalizePath = [](const fs::path& path) {
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(path, ec);
        if (!ec) {
            return canonical;
        }
        return path.lexically_normal();
    };

    auto isPathWithin = [&](const fs::path& root, const fs::path& path) {
        if (root.empty()) return true;
        const fs::path normalizedRoot = normalizePath(root);
        const fs::path normalizedPath = normalizePath(path);
        if (normalizedPath == normalizedRoot) return true;
        fs::path current = normalizedPath;
        while (current.has_parent_path()) {
            fs::path parent = current.parent_path();
            if (parent.empty() || parent == current) {
                break;
            }
            current = parent;
            if (current == normalizedRoot) {
                return true;
            }
        }
        return false;
    };

    auto clearFileSelection = [&]() {
        fileBrowser.selectedFiles.clear();
        fileBrowser.selectedFileKeys.clear();
    };

    auto addFileSelection = [&](const fs::path& path) {
        const std::string key = fileSelectionKey(path);
        if (fileBrowser.selectedFileKeys.insert(key).second) {
            fileBrowser.selectedFiles.push_back(path);
        }
    };

    auto removeFileSelection = [&](const fs::path& path) {
        const std::string key = fileSelectionKey(path);
        if (fileBrowser.selectedFileKeys.erase(key) == 0) {
            return;
        }
        fileBrowser.selectedFiles.erase(
            std::remove_if(fileBrowser.selectedFiles.begin(), fileBrowser.selectedFiles.end(),
                           [&](const fs::path& selected) {
                               return fileSelectionKey(selected) == key;
                           }),
            fileBrowser.selectedFiles.end());
    };

    auto selectFileEntry = [&](int index) {
        if (index < 0 || index >= static_cast<int>(fileBrowser.entries.size())) {
            return;
        }

        const fs::path path = fileBrowser.entries[static_cast<size_t>(index)].path();
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        const bool shift = ImGui::GetIO().KeyShift;

        if (shift && fileBrowser.selectionAnchorIndex >= 0 &&
            fileBrowser.selectionAnchorIndex < static_cast<int>(fileBrowser.entries.size())) {
            if (!ctrl) {
                clearFileSelection();
            }
            int first = std::min(fileBrowser.selectionAnchorIndex, index);
            int last = std::max(fileBrowser.selectionAnchorIndex, index);
            for (int i = first; i <= last; ++i) {
                addFileSelection(fileBrowser.entries[static_cast<size_t>(i)].path());
            }
            fileBrowser.selectedFile = path;
            return;
        }

        if (ctrl) {
            const std::string key = fileSelectionKey(path);
            if (fileBrowser.selectedFileKeys.find(key) != fileBrowser.selectedFileKeys.end()) {
                removeFileSelection(path);
                fileBrowser.selectedFile = fileBrowser.selectedFiles.empty()
                    ? fs::path()
                    : fileBrowser.selectedFiles.back();
            } else {
                addFileSelection(path);
                fileBrowser.selectedFile = path;
            }
            fileBrowser.selectionAnchorIndex = index;
            return;
        }

        clearFileSelection();
        addFileSelection(path);
        fileBrowser.selectedFile = path;
        fileBrowser.selectionAnchorIndex = index;
    };

    auto remapSelectedPathAfterMove = [&](const fs::path& sourcePath, const fs::path& movedPath) {
        fs::path selected = normalizePath(fileBrowser.selectedFile);
        fs::path source = normalizePath(sourcePath);
        fs::path moved = normalizePath(movedPath);
        if (selected.empty()) return;
        if (selected == source) {
            fileBrowser.selectedFile = moved;
            clearFileSelection();
            addFileSelection(moved);
            return;
        }
        if (!isPathWithin(source, selected)) {
            return;
        }
        std::error_code relEc;
        fs::path rel = fs::relative(selected, source, relEc);
        if (relEc || rel.empty()) {
            return;
        }
        fileBrowser.selectedFile = moved / rel;
        clearFileSelection();
        addFileSelection(fileBrowser.selectedFile);
    };

    auto languagePackDisplayName = [](const std::string& id) {
        return ModuCPPLang::FindLanguageOrCanonical(id).displayName;
    };

    auto runImportTranslation = [&](const std::vector<fs::path>& files,
                                    const ModuCPPLang::LanguagePack& target) {
        if (files.empty()) return;
        const ModuCPPLang::TranslationReport report = ModuCPPLang::TranslateScriptFiles(files, target);
        if (report.translated > 0) {
            addConsoleMessage("Translated " + std::to_string(report.translated) +
                              " imported script(s) to " + target.displayName + ".",
                              ConsoleMessageType::Success);
        }
        if (report.failed > 0) {
            addConsoleMessage("Failed to translate " + std::to_string(report.failed) +
                              " imported script(s).", ConsoleMessageType::Warning);
            if (!report.firstError.empty()) {
                addConsoleMessage(report.firstError, ConsoleMessageType::Error);
            }
        }
        fileBrowser.needsRefresh = true;
    };

    auto noteImportedScriptLanguages = [&](const fs::path& importedPath) {
        const ModuCPPLang::LanguagePack& projectLanguage =
            ModuCPPLang::FindLanguageOrCanonical(projectManager.effectiveModuCppLanguage());
        const ModuCPPLang::ScriptLanguageSurvey survey =
            ModuCPPLang::SurveyScripts(importedPath, projectLanguage);
        if (survey.files.empty()) return;

        switch (projectManager.currentProject.languageSettings.importTranslatePolicy) {
            case ProjectImportTranslatePolicy::Always:
                runImportTranslation(survey.files, projectLanguage);
                return;
            case ProjectImportTranslatePolicy::Never:
                addConsoleMessage("Imported " + std::to_string(survey.files.size()) +
                                  " script(s) written in another ModuCPP syntax language; kept as-is.",
                                  ConsoleMessageType::Info);
                return;
            case ProjectImportTranslatePolicy::Ask:
            default:
                break;
        }

        for (const fs::path& file : survey.files) {
            pendingLanguageTranslationFiles.push_back(file);
        }
        for (const std::string& id : survey.detectedLanguageIds) {
            if (std::find(pendingLanguageTranslationSourceIds.begin(),
                          pendingLanguageTranslationSourceIds.end(), id) ==
                pendingLanguageTranslationSourceIds.end()) {
                pendingLanguageTranslationSourceIds.push_back(id);
            }
        }
        triggerImportLanguagePopup = true;
    };

    auto importPathIntoDirectory = [&](const fs::path& sourcePath, const fs::path& destinationDir) {
        std::error_code ec;
        if (sourcePath.empty() || destinationDir.empty()) {
            return false;
        }
        if (!fs::exists(sourcePath, ec) || ec) {
            addConsoleMessage("Import failed: source missing " + PathToUtf8(sourcePath), ConsoleMessageType::Error);
            return false;
        }
        if (!fs::exists(destinationDir, ec) || ec || !fs::is_directory(destinationDir, ec)) {
            addConsoleMessage("Import failed: destination folder missing " + PathToUtf8(destinationDir), ConsoleMessageType::Error);
            return false;
        }

        const fs::path normalizedSource = normalizePath(sourcePath);
        const fs::path normalizedDestination = normalizePath(destinationDir);
        if (fs::is_directory(normalizedSource, ec) && !ec && isPathWithin(normalizedSource, normalizedDestination)) {
            addConsoleMessage("Import failed: cannot import a folder into itself " + PathToUtf8(normalizedSource),
                              ConsoleMessageType::Error);
            return false;
        }

        fs::path targetPath = makeUniquePath(normalizedDestination / normalizedSource.filename());
        ec.clear();
        if (fs::is_directory(normalizedSource, ec) && !ec) {
            fs::copy(normalizedSource, targetPath,
                     fs::copy_options::recursive |
                     fs::copy_options::copy_symlinks |
                     fs::copy_options::skip_existing,
                     ec);
        } else {
            fs::copy_file(normalizedSource, targetPath, fs::copy_options::overwrite_existing, ec);
        }

        if (ec) {
            addConsoleMessage("Import failed: " + PathToUtf8(normalizedSource) + " (" + ec.message() + ")",
                              ConsoleMessageType::Error);
            return false;
        }
        addConsoleMessage("Imported: " + PathToUtf8(targetPath), ConsoleMessageType::Success);
        noteImportedScriptLanguages(targetPath);
        fileBrowser.selectedFile = targetPath;
        fileBrowser.needsRefresh = true;
        projectManager.currentProject.hasUnsavedChanges = true;
        return true;
    };

#ifndef __ANDROID__
    auto importDirectoryContentsIntoDirectory = [&](const fs::path& sourceDir, const fs::path& destinationDir) {
        std::error_code ec;
        if (sourceDir.empty() || destinationDir.empty()) {
            return false;
        }
        if (!fs::exists(sourceDir, ec) || ec || !fs::is_directory(sourceDir, ec)) {
            addConsoleMessage("Import failed: source folder missing " + PathToUtf8(sourceDir), ConsoleMessageType::Error);
            return false;
        }
        if (!fs::exists(destinationDir, ec) || ec || !fs::is_directory(destinationDir, ec)) {
            addConsoleMessage("Import failed: destination folder missing " + PathToUtf8(destinationDir), ConsoleMessageType::Error);
            return false;
        }

        const fs::path normalizedSource = normalizePath(sourceDir);
        const fs::path normalizedDestination = normalizePath(destinationDir);
        if (normalizedSource == normalizedDestination) {
            addConsoleMessage("Import failed: source and destination folder are the same.", ConsoleMessageType::Error);
            return false;
        }

        int importedCount = 0;
        int failedCount = 0;
        for (const auto& child : fs::directory_iterator(normalizedSource, ec)) {
            if (ec) {
                addConsoleMessage("Import failed while reading folder: " + PathToUtf8(normalizedSource) +
                                  " (" + ec.message() + ")", ConsoleMessageType::Error);
                return false;
            }
            if (importPathIntoDirectory(child.path(), normalizedDestination)) {
                ++importedCount;
            } else {
                ++failedCount;
            }
        }

        if (importedCount == 0 && failedCount == 0) {
            addConsoleMessage("Import skipped: folder is empty " + PathToUtf8(normalizedSource), ConsoleMessageType::Info);
            return false;
        }
        if (importedCount > 0) {
            addConsoleMessage("Imported " + std::to_string(importedCount) + " item(s) from folder contents.",
                              ConsoleMessageType::Success);
        }
        if (failedCount > 0) {
            addConsoleMessage("Failed to import " + std::to_string(failedCount) + " item(s) from folder contents.",
                              ConsoleMessageType::Warning);
        }
        return importedCount > 0;
    };
#endif

    auto movePathIntoDirectory = [&](const fs::path& sourcePath, const fs::path& destinationDir) {
        std::error_code ec;
        if (sourcePath.empty() || destinationDir.empty()) {
            return false;
        }
        if (!fs::exists(sourcePath, ec) || ec) {
            return false;
        }
        if (!fs::exists(destinationDir, ec) || ec || !fs::is_directory(destinationDir, ec)) {
            return false;
        }
        if (!fileBrowser.projectRoot.empty() && !isPathWithin(fileBrowser.projectRoot, sourcePath)) {
            addConsoleMessage("Move blocked: source is outside the project root.", ConsoleMessageType::Warning);
            return false;
        }
        if (!fileBrowser.projectRoot.empty() && !isPathWithin(fileBrowser.projectRoot, destinationDir)) {
            addConsoleMessage("Move blocked: destination is outside the project root.", ConsoleMessageType::Warning);
            return false;
        }

        fs::path normalizedSource = normalizePath(sourcePath);
        fs::path normalizedDestination = normalizePath(destinationDir);
        fs::path sourceParent = normalizePath(normalizedSource.parent_path());
        if (normalizedDestination == sourceParent) {
            return false;
        }
        if (fs::is_directory(normalizedSource, ec) && !ec && isPathWithin(normalizedSource, normalizedDestination)) {
            addConsoleMessage("Move failed: cannot move a folder into itself.", ConsoleMessageType::Error);
            return false;
        }

        fs::path targetPath = normalizedDestination / normalizedSource.filename();
        if (targetPath == normalizedSource) {
            return false;
        }
        if (fs::exists(targetPath, ec) && !ec) {
            targetPath = makeUniquePath(targetPath);
        }

        ec.clear();
        fs::rename(normalizedSource, targetPath, ec);
        if (ec) {
            ec.clear();
            if (fs::is_directory(normalizedSource, ec) && !ec) {
                fs::copy(normalizedSource, targetPath,
                         fs::copy_options::recursive |
                         fs::copy_options::copy_symlinks |
                         fs::copy_options::skip_existing,
                         ec);
                if (!ec) {
                    fs::remove_all(normalizedSource, ec);
                }
            } else {
                fs::copy_file(normalizedSource, targetPath, fs::copy_options::overwrite_existing, ec);
                if (!ec) {
                    fs::remove(normalizedSource, ec);
                }
            }
        }

        if (ec) {
            addConsoleMessage("Move failed: " + PathToUtf8(normalizedSource) + " (" + ec.message() + ")",
                              ConsoleMessageType::Error);
            return false;
        }

        remapSelectedPathAfterMove(normalizedSource, targetPath);
        fileBrowser.needsRefresh = true;
        projectManager.currentProject.hasUnsavedChanges = true;
        addConsoleMessage("Moved: " + PathToUtf8(normalizedSource.filename()) + " -> " + PathToUtf8(targetPath),
                          ConsoleMessageType::Success);
        return true;
    };

    auto handleMovePayloadToDirectory = [&](const ImGuiPayload* payload, const fs::path& destinationDir) {
        if (!payload || payload->DataSize <= 0 || !payload->Data) {
            return false;
        }
        const char* sourcePathChars = static_cast<const char*>(payload->Data);
        if (!sourcePathChars || !*sourcePathChars) {
            return false;
        }
        return movePathIntoDirectory(fs::path(sourcePathChars), destinationDir);
    };

    auto copySelectionToClipboard = [&](bool cut) {
        if (fileBrowser.selectedFiles.empty()) {
            return;
        }
        fileClipboard = fileBrowser.selectedFiles;
        fileClipboardIsCut = cut;
        addConsoleMessage(std::string(cut ? "Cut " : "Copied ") +
                          std::to_string(fileClipboard.size()) + " item(s)",
                          ConsoleMessageType::Info);
    };

    auto pasteClipboardIntoDirectory = [&](const fs::path& destinationDir) {
        if (fileClipboard.empty()) {
            return;
        }
        for (const fs::path& source : fileClipboard) {
            if (fileClipboardIsCut) {
                movePathIntoDirectory(source, destinationDir);
            } else {
                importPathIntoDirectory(source, destinationDir);
            }
        }
        if (fileClipboardIsCut) {
            fileClipboard.clear();
            fileClipboardIsCut = false;
        }
    };

    auto queueImportAssetsDialog = [&](const fs::path& destinationDir) {
        pendingImportTargetPath = destinationDir.empty() ? fileBrowser.currentPath : destinationDir;
        importAssetPaths[0] = '\0';
        showImportAssetsPopup = true;
        triggerImportAssetsPopup = true;
    };

#ifdef __ANDROID__
    auto pollAndroidImportPickerResult = [&]() {
        Modularity::AndroidRuntime::FilePickerResult result;
        if (!Modularity::AndroidRuntime::PollFilePickerResult(result)) {
            return;
        }

        androidImportPickerPending = false;
        fs::path targetDir = androidImportPickerTargetPath.empty()
            ? fileBrowser.currentPath
            : androidImportPickerTargetPath;
        androidImportPickerTargetPath.clear();

        if (result.canceled && result.paths.empty()) {
            addConsoleMessage("Import cancelled.", ConsoleMessageType::Info);
            return;
        }
        if (!result.error.empty()) {
            addConsoleMessage("Android file picker: " + result.error,
                              result.paths.empty() ? ConsoleMessageType::Error : ConsoleMessageType::Warning);
        }
        if (result.paths.empty()) {
            return;
        }

        int importedCount = 0;
        int failedCount = 0;
        for (const std::string& path : result.paths) {
            if (path.empty()) {
                continue;
            }
            if (importPathIntoDirectory(fs::path(path), targetDir)) {
                ++importedCount;
            } else {
                ++failedCount;
            }
        }
        if (importedCount > 0) {
            addConsoleMessage("Imported " + std::to_string(importedCount) + " Android asset(s).",
                              ConsoleMessageType::Success);
            showImportAssetsPopup = false;
        }
        if (failedCount > 0) {
            addConsoleMessage("Failed to import " + std::to_string(failedCount) + " Android asset(s).",
                              ConsoleMessageType::Warning);
        }
    };
    pollAndroidImportPickerResult();
#endif

    // Get colors for categories
    auto getCategoryColor = [](FileCategory cat) -> ImU32 {
        switch (cat) {
            case FileCategory::Folder:  return IM_COL32(255, 200, 80, 255);   // Yellow/orange
            case FileCategory::Scene:   return IM_COL32(100, 180, 255, 255);  // Blue
            case FileCategory::Model:   return IM_COL32(100, 220, 140, 255);  // Green
            case FileCategory::Material:return IM_COL32(220, 200, 120, 255);  // Gold
            case FileCategory::Texture: return IM_COL32(220, 130, 220, 255);  // Purple/pink
            case FileCategory::Video:   return IM_COL32(220, 90, 120, 255);   // Red
            case FileCategory::Shader:  return IM_COL32(255, 140, 90, 255);   // Orange
            case FileCategory::Script:  return IM_COL32(130, 200, 255, 255);  // Light blue
            case FileCategory::Audio:   return IM_COL32(255, 180, 100, 255);  // Warm orange
            case FileCategory::Text:    return IM_COL32(180, 180, 180, 255);  // Gray
            default:                    return IM_COL32(150, 150, 150, 255);  // Dark gray
        }
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 2.0f));
    bool canGoBack = fileBrowser.historyIndex > 0;
    bool canGoForward = fileBrowser.historyIndex < (int)fileBrowser.pathHistory.size() - 1;
    bool canGoUp = fileBrowser.currentPath != fileBrowser.projectRoot &&
                   fileBrowser.currentPath.has_parent_path();

    ImGui::BeginDisabled(!canGoBack);
    ImGui::Button("<##Back", ImVec2(24, 0));
    if (ImGui::IsItemActivated()) fileBrowser.navigateBack();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back");

    ImGui::SameLine();
    ImGui::BeginDisabled(!canGoForward);
    ImGui::Button(">##Forward", ImVec2(24, 0));
    if (ImGui::IsItemActivated()) fileBrowser.navigateForward();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward");

    ImGui::SameLine();
    ImGui::BeginDisabled(!canGoUp);
    ImGui::Button("^##Up", ImVec2(24, 0));
    if (ImGui::IsItemActivated()) fileBrowser.navigateUp();
    ImGui::EndDisabled();
    if (canGoUp && ImGui::BeginDragDropTarget()) {
        fs::path parentTarget = fileBrowser.currentPath.parent_path();
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_BROWSER_ENTRY")) {
            handleMovePayloadToDirectory(payload, parentTarget);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
            handleMovePayloadToDirectory(payload, parentTarget);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Up one folder");
    ImGui::PopStyleVar();

    ImGui::SameLine();

    ImGui::SetNextItemWidth(168.0f);
    if (ImGui::InputTextWithHint("##Search", "Filter files...", fileBrowserSearch, sizeof(fileBrowserSearch))) {
        fileBrowser.searchFilter = fileBrowserSearch;
        fileBrowser.needsRefresh = true;
    }
    if (fileBrowserSearch[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::Button("x##ClearSearch", ImVec2(20.0f, 0.0f))) {
            fileBrowserSearch[0] = '\0';
            fileBrowser.searchFilter.clear();
            fileBrowser.needsRefresh = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear search");
    }

    if (fileBrowser.isRefreshing()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Refreshing...");
    }

    ImGui::SameLine();
    bool isGridMode = fileBrowser.viewMode == FileBrowserViewMode::Grid;
    if (isGridMode) {
        ImGui::SetNextItemWidth(76.0f);
        if (ImGui::SliderFloat("##IconScale", &fileBrowserIconScale, 0.6f, 2.0f, "%.1fx")) {
            settingsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Icon Size: %.1fx", fileBrowserIconScale);
        ImGui::SameLine();
    }

    if (isGridMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Grid", ImVec2(44, 0)) && !isGridMode) {
        fileBrowser.viewMode = FileBrowserViewMode::Grid;
        settingsDirty = true;
    }
    if (isGridMode) {
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine();
    if (!isGridMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("List", ImVec2(44, 0)) && isGridMode) {
        fileBrowser.viewMode = FileBrowserViewMode::List;
        settingsDirty = true;
    }
    if (!isGridMode) {
        ImGui::PopStyleColor(3);
    }

    ImGui::SameLine();
    if (ImGui::Button(showFileBrowserSidebar ? "Hide Side" : "Show Side", ImVec2(72, 0))) {
        showFileBrowserSidebar = !showFileBrowserSidebar;
        settingsDirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle sidebar");

    std::string itemCount = std::to_string(fileBrowser.entries.size()) + " items";
    if (!fileBrowser.scanError.empty()) itemCount += "  (!)";
    float itemCountWidth = ImGui::CalcTextSize(itemCount.c_str()).x;
    float rightX = ImGui::GetWindowContentRegionMax().x - itemCountWidth - 8.0f;
    if (rightX > ImGui::GetCursorPosX()) {
        ImGui::SameLine(rightX);
    } else {
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%s", itemCount.c_str());
    if (!fileBrowser.scanError.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", fileBrowser.scanError.c_str());
    }

    ImGui::PopStyleVar(2);

    {
        fs::path relativePath;
        if (fileBrowser.projectRoot.empty()) {
            relativePath = fileBrowser.currentPath.filename();
        } else {
            try {
                relativePath = fs::relative(fileBrowser.currentPath, fileBrowser.projectRoot);
            } catch (...) {
                relativePath = fileBrowser.currentPath.filename();
            }
        }

        std::vector<fs::path> pathParts;
        pathParts.push_back(fileBrowser.projectRoot);
        fs::path accumulated = fileBrowser.projectRoot;
        for (const auto& part : relativePath) {
            if (part != ".") {
                accumulated /= part;
                pathParts.push_back(accumulated);
            }
        }

        struct Breadcrumb {
            std::string label;
            fs::path target;
        };
        std::vector<Breadcrumb> allCrumbs;
        allCrumbs.reserve(pathParts.size());
        for (size_t i = 0; i < pathParts.size(); ++i) {
            std::string name = (i == 0) ? "Project" : PathToUtf8(pathParts[i].filename());
            allCrumbs.push_back({name, pathParts[i]});
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        const float avail = ImGui::GetContentRegionAvail().x;
        auto crumbWidth = [&](const std::string& s) {
            return ImGui::CalcTextSize(s.c_str()).x + style.FramePadding.x * 2.0f;
        };
        const float sepWidth = ImGui::CalcTextSize("/").x + 4.0f;

        float fullWidth = 0.0f;
        for (size_t i = 0; i < allCrumbs.size(); ++i) {
            fullWidth += crumbWidth(allCrumbs[i].label);
            if (i + 1 < allCrumbs.size()) fullWidth += sepWidth;
        }

        std::vector<Breadcrumb> crumbs;
        if (fullWidth <= avail || allCrumbs.size() <= 2) {
            crumbs = allCrumbs;
        } else {
            const std::string kEllipsis = "...";
            float used = crumbWidth(allCrumbs.front().label) + sepWidth + crumbWidth(kEllipsis);
            std::vector<size_t> tail;
            for (size_t i = allCrumbs.size() - 1; i >= 1; --i) {
                const float add = sepWidth + crumbWidth(allCrumbs[i].label);
                if (used + add > avail && !tail.empty()) break;
                used += add;
                tail.push_back(i);
                if (i == 1) break;
            }
            const size_t firstTailIdx = tail.back();
            if (firstTailIdx <= 1) {
                crumbs = allCrumbs;
            } else {
                crumbs.push_back(allCrumbs.front());
                crumbs.push_back({kEllipsis, allCrumbs[firstTailIdx - 1].target});
                for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
                    crumbs.push_back(allCrumbs[*it]);
                }
            }
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
        for (size_t i = 0; i < crumbs.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(crumbs[i].label.c_str())) {
                fileBrowser.navigateTo(crumbs[i].target);
            }
            if (crumbs[i].label == "..." && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", PathToUtf8(crumbs[i].target).c_str());
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_BROWSER_ENTRY")) {
                    handleMovePayloadToDirectory(payload, crumbs[i].target);
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                    handleMovePayloadToDirectory(payload, crumbs[i].target);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
            if (i < crumbs.size() - 1) {
                ImGui::SameLine(0, 2);
                ImGui::TextDisabled("/");
                ImGui::SameLine(0, 2);
            }
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Dummy(ImVec2(0.0f, 3.0f));

    // === FILE CONTENT AREA ===
    ImGui::BeginChild("FileContent", ImVec2(0, 0), false);

    if (!pendingExternalFileDrops.empty()) {
        const ImVec2 contentMin = ImGui::GetWindowPos();
        const ImVec2 contentMax(contentMin.x + ImGui::GetWindowWidth(),
                                contentMin.y + ImGui::GetWindowHeight());
        for (const ExternalFileDropEvent& drop : pendingExternalFileDrops) {
            // A package is an action, not an asset. Dropping one anywhere in the editor
            // opens its import dialog instead of copying the archive into the project,
            // which is both what someone dragging one in is asking for and what keeps
            // .modupak files from accumulating in Assets/ as dead weight.
            const std::string droppedExt = PathToUtf8(drop.path.extension());
            if (droppedExt == ".modupak") {
                openModuPakImportDialog(drop.path);
                continue;
            }
            const bool droppedOverPanel =
                (drop.mouseX >= contentMin.x && drop.mouseX <= contentMax.x &&
                 drop.mouseY >= contentMin.y && drop.mouseY <= contentMax.y);
            if (droppedOverPanel) {
                importPathIntoDirectory(drop.path, fileBrowser.currentPath);
            }
        }
        pendingExternalFileDrops.clear();
    }

    if (showFileBrowserSidebar) {
        float minSidebarWidth = 150.0f;
        float maxSidebarWidth = std::max(minSidebarWidth, ImGui::GetContentRegionAvail().x * 0.5f);
        fileBrowserSidebarWidth = std::clamp(fileBrowserSidebarWidth, minSidebarWidth, maxSidebarWidth);

        ImGui::BeginChild("FileSidebar", ImVec2(fileBrowserSidebarWidth, 0), false);
        const bool sidebarTouch =
            (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_IsTouchScreen) != 0;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            sidebarTouch ? ImVec2(6.0f, 7.0f) : ImVec2(4.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            sidebarTouch ? ImVec2(6.0f, 8.0f) : ImVec2(4.0f, 2.0f));
        ImGui::TextDisabled("Favorites");
        ImGui::SameLine();
        if (ImGui::SmallButton("+##AddFavorite")) {
            fs::path current = normalizePath(fileBrowser.currentPath);
            bool exists = false;
            for (const auto& fav : fileBrowserFavorites) {
                if (normalizePath(fav) == current) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                fileBrowserFavorites.push_back(current);
                settingsDirty = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add current folder");

        fs::path baseRoot = fileBrowser.projectRoot.empty()
            ? projectManager.currentProject.projectPath
            : fileBrowser.projectRoot;
        fs::path normalizedCurrent = normalizePath(fileBrowser.currentPath);

        for (size_t i = 0; i < fileBrowserFavorites.size(); ++i) {
            fs::path fav = fileBrowserFavorites[i];
            std::string label;
            std::error_code ec;
            fs::path rel = fs::relative(fav, baseRoot, ec);
            std::string relStr = rel.generic_string();
            if (!ec && !rel.empty() && relStr.find("..") != 0) {
                label = relStr;
                if (label.empty() || label == ".") {
                    label = "Project";
                }
            } else {
                label = PathToUtf8(fav.filename());
                if (label.empty()) {
                    label = PathToUtf8(fav);
                }
            }

            bool exists = fs::exists(fav);
            ImGui::PushID(static_cast<int>(i));
            if (!exists) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(label.c_str(), normalizePath(fav) == normalizedCurrent)) {
                if (exists) {
                    fileBrowser.navigateTo(fav);
                }
            }
            if (!exists) {
                ImGui::EndDisabled();
            }
            if (ImGui::BeginPopupContextItem("FavContext")) {
                if (ImGui::MenuItem("Remove")) {
                    fileBrowserFavorites.erase(fileBrowserFavorites.begin() + static_cast<int>(i));
                    settingsDirty = true;
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                if (exists && ImGui::MenuItem("Open in File Explorer")) {
                    openPathInFileManager(fav);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Folders");
        ImGui::BeginChild("FolderTree", ImVec2(0, 0), false);

        static FolderTreeChildCache sFolderTreeChildCache;
        const double treeNow = ImGui::GetTime();

        auto drawFolderTree = [&](auto&& self, const fs::path& path) -> void {
            if (!fs::exists(path)) {
                return;
            }
            std::string name = PathToUtf8(path.filename());
            if (name.empty()) {
                name = "Project";
            }
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                       ImGuiTreeNodeFlags_SpanFullWidth;
            if (fileBrowser.currentPath == path) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            ImGui::PushID(PathToUtf8(path).c_str());
            bool open = ImGui::TreeNodeEx(name.c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                fileBrowser.navigateTo(path);
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_BROWSER_ENTRY")) {
                    handleMovePayloadToDirectory(payload, path);
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                    handleMovePayloadToDirectory(payload, path);
                }
                ImGui::EndDragDropTarget();
            }
            if (open) {
                const std::vector<fs::path>& dirs =
                    sFolderTreeChildCache.children(path, fileBrowser.showHiddenFiles, treeNow);
                for (const auto& dir : dirs) {
                    self(self, dir);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };

        if (!baseRoot.empty()) {
            drawFolderTree(drawFolderTree, baseRoot);
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::EndChild();

        ImGui::SameLine();
        float splitterHeight = ImGui::GetContentRegionAvail().y;
        if (splitterHeight < 1.0f) {
            splitterHeight = 1.0f;
        }
        ImGui::InvisibleButton("SidebarSplitter", ImVec2(4.0f, splitterHeight));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive()) {
            fileBrowserSidebarWidth += ImGui::GetIO().MouseDelta.x;
            fileBrowserSidebarWidth = std::clamp(fileBrowserSidebarWidth, minSidebarWidth, maxSidebarWidth);
            settingsDirty = true;
        }
        ImGui::SameLine();
    }

    ImGui::BeginChild("FileMain", ImVec2(0, 0), false);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    static FileBrowserTextLayoutCache sFileBrowserTextLayoutCache;

    const bool fileBrowserTouch =
        (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_IsTouchScreen) != 0;
    int navColumns = 1;
    float navRowStride = 0.0f;
    const std::string primarySelectionKey = fileBrowser.selectedFile.empty()
        ? std::string()
        : fileSelectionKey(fileBrowser.selectedFile);
    if (fileBrowser.viewMode == FileBrowserViewMode::Grid) {
        MODU_PROFILE_SCOPE("Project Grid Render", ProfilerSampleCategory::UI);
        float baseIconSize = fileBrowserTouch ? 84.0f : 56.0f;
        float iconSize = baseIconSize * fileBrowserIconScale;
        float padding = 6.0f * fileBrowserIconScale;
        float textHeight = 17.0f;
        float cellWidth = iconSize + padding * 2;
        float cellHeight = iconSize + padding * 1 + textHeight;

        float windowWidth = ImGui::GetContentRegionAvail().x;
        int columns = std::max(1, (int)((windowWidth + padding) / (cellWidth + padding)));
        const int rowCount = static_cast<int>((fileBrowser.cachedEntries.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));

        navColumns = columns;
        if (ImGui::BeginTable("FileGrid", columns, ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX)) {
            const float cellStride = cellHeight + ImGui::GetStyle().CellPadding.y * 2.0f;
            navRowStride = cellStride;
            ImGuiListClipper clipper;
            clipper.Begin(rowCount, cellStride);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, cellHeight);
                    for (int column = 0; column < columns; ++column) {
                        const int i = row * columns + column;
                        ImGui::TableSetColumnIndex(column);
                        if (i >= static_cast<int>(fileBrowser.cachedEntries.size())) {
                            ImGui::Dummy(ImVec2(cellWidth, cellHeight));
                            continue;
                        }

                        const auto& entry = fileBrowser.entries[static_cast<size_t>(i)];
                        const auto& cached = fileBrowser.cachedEntries[static_cast<size_t>(i)];
                        const FileCategory category = cached.category;
                        const bool isSelected = fileBrowser.selectedFileKeys.find(cached.selectionKey) != fileBrowser.selectedFileKeys.end();

                        ImGui::PushID(i);

                        ImVec2 cellStart = ImGui::GetCursorScreenPos();
                        ImVec2 cellEnd(cellStart.x + cellWidth, cellStart.y + cellHeight);

                        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                        if (ImGui::InvisibleButton("##cell", ImVec2(cellWidth, cellHeight))) {
                            selectFileEntry(i);
                        }
                        ImGui::PopItemFlag();
                        const bool hovered = ImGui::IsItemHovered() && !navSuppressHover;
                        const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(0);

                        const ImU32 bgColor = isSelected ? IM_COL32(72, 98, 132, 205) :
                                            (hovered ? IM_COL32(48, 52, 60, 165) : IM_COL32(0, 0, 0, 0));
                        if (bgColor != IM_COL32(0, 0, 0, 0)) {
                            drawList->AddRectFilled(cellStart, cellEnd, bgColor, 7.0f);
                        }

                        if (isSelected) {
                            drawList->AddRect(cellStart, cellEnd, IM_COL32(108, 162, 224, 255), 7.0f, 0, 1.8f);
                        } else if (hovered) {
                            drawList->AddRect(cellStart, cellEnd, IM_COL32(84, 98, 116, 210), 7.0f, 0, 1.0f);
                        }

                        const ImVec2 iconPos(
                            cellStart.x + (cellWidth - iconSize) * 0.5f,
                            cellStart.y + padding
                        );
                        const ImVec2 previewMin = iconPos;
                        const ImVec2 previewMax(iconPos.x + iconSize, iconPos.y + iconSize);
                        bool drewPreview = false;
                        if (category == FileCategory::Scene) {
                            drewPreview = DrawSceneLogoPreview(renderer, drawList, previewMin, previewMax, 9.0f);
                        } else if (category == FileCategory::Texture) {
                            drewPreview = DrawTexturePreview(renderer, drawList, cached, previewMin, previewMax, 9.0f);
                            DrawTexturePreviewErrorTooltip(renderer, cached, previewMin, previewMax);
                        } else if (category == FileCategory::Model) {
                            drewPreview = DrawModelPreview(renderer, drawList, cached, previewMin, previewMax, 1000, 9.0f);
                        } else if (category == FileCategory::Video) {
                            drewPreview = DrawVideoPreview(renderer, drawList, cached, previewMin, previewMax, 9.0f);
                        }
                        if (!drewPreview) {
                            FileIcons::DrawIcon(renderer, drawList, category, iconPos, iconSize, getCategoryColor(category), cached.folderHasItems);
                        }

                        const float maxTextWidth = cellWidth - 4.0f;
                        const std::string& displayName = GetTruncatedLabel(sFileBrowserTextLayoutCache, cached, maxTextWidth);
                        const ImVec2 textSize = ImGui::CalcTextSize(displayName.c_str());
                        const ImVec2 textPos(
                            cellStart.x + (cellWidth - textSize.x) * 0.5f,
                            cellStart.y + padding + iconSize + 4.0f
                        );
                        drawList->AddText(textPos, IM_COL32(228, 232, 240, 255), displayName.c_str());

                        if (doubleClicked) {
                            openEntry(entry);
                        }

                        if (fileContextMenuKeyRequested && cached.selectionKey == primarySelectionKey) {
                            ImGui::OpenPopup("FileContextMenu");
                        }
                        if (ImGui::BeginPopupContextItem("FileContextMenu")) {
                            if (ImGui::MenuItem("Open", "Enter")) {
                                openEntry(entry);
                            }
                            #ifdef _WIN32
                            if (!entry.is_directory() && ImGui::MenuItem("Open With...")) {
                                openPathWithDialog(entry.path());
                            }
                            #endif
                            if (entry.is_directory() && ImGui::BeginMenu("New")) {
                                drawCreateMenu(entry.path());
                                ImGui::EndMenu();
                            }
                            if (entry.is_directory() && ImGui::MenuItem("Import Assets Here...")) {
                                queueImportAssetsDialog(entry.path());
                            }
                            if (fileBrowser.isModelFile(entry)) {
                                bool isObj = fileBrowser.isOBJFile(entry);
                                bool isMMesh = IsMMeshPath(entry.path());
                                if (ImGui::MenuItem("Import to Scene")) {
                                    std::string defaultName = PathToUtf8(entry.path().stem());
                                    if (isObj) {
                                        pendingOBJPath = PathToUtf8(entry.path());
                                        strncpy(importOBJName, defaultName.c_str(), sizeof(importOBJName) - 1);
                                        showImportOBJDialog = true;
                                    } else {
                                        pendingModelPath = PathToUtf8(entry.path());
                                        strncpy(importModelName, defaultName.c_str(), sizeof(importModelName) - 1);
                                        showImportModelDialog = true;
                                    }
                                }
                                if (ImGui::MenuItem("Quick Import")) {
                                    if (isObj) {
                                        importOBJToScene(PathToUtf8(entry.path()), "");
                                    } else {
                                        importModelToScene(PathToUtf8(entry.path()), "");
                                    }
                                }
                                if (!isMMesh && ImGui::MenuItem("Convert to Raw Mesh")) {
                                    convertModelToRawMesh(PathToUtf8(entry.path()));
                                }
                            }
                            if (fileBrowser.getFileCategory(entry) == FileCategory::Material) {
                                if (ImGui::MenuItem("Apply to Selected")) {
                                    if (SceneObject* sel = getSelectedObject()) {
                                        sel->materialPath = PathToUtf8(entry.path());
                                        loadMaterialFromFile(*sel);
                                    }
                                }
                            }
                            if (fileBrowser.getFileCategory(entry) == FileCategory::Script) {
                                if (ImGui::MenuItem("Compile Script")) {
                                    compileScriptFile(entry.path());
                                }
                            }
                            if (fileBrowser.getFileCategory(entry) == FileCategory::Texture) {
                                if (ImGui::BeginMenu("GPU Storage Format")) {
                                    const std::string texAbs = PathToUtf8(entry.path());
                                    const TextureFormatPolicy current = renderer.getTextureFormatOverride(texAbs);
                                    auto applyFormat = [&](TextureFormatPolicy policy) {
                                        renderer.setTextureFormatOverride(texAbs, policy);
                                        std::error_code relEc;
                                        fs::path rel = fs::relative(entry.path(),
                                                                    projectManager.currentProject.projectPath, relEc);
                                        const std::string relKey = (relEc || rel.empty())
                                            ? texAbs : rel.generic_string();
                                        auto& overrides = projectManager.currentProject.textureFormatOverrides;
                                        if (policy == TextureFormatPolicy::Auto) {
                                            overrides.erase(relKey);
                                        } else {
                                            overrides[relKey] = ToString(policy);
                                        }
                                        projectManager.currentProject.hasUnsavedChanges = true;
                                        projectManager.currentProject.saveProjectFile();
                                        ++g_texturePreviewGeneration;
                                    };
                                    struct FormatChoice { TextureFormatPolicy policy; const char* label; const char* hint; };
                                    static const FormatChoice kChoices[] = {
                                        { TextureFormatPolicy::Auto,    "Auto (adaptive)", "Smallest format the image allows" },
                                        { TextureFormatPolicy::Full,    "Full (RGBA8)",    "32bpp, highest quality" },
                                        { TextureFormatPolicy::RGB565,  "RGB565",          "16bpp, no alpha" },
                                        { TextureFormatPolicy::RGB5_A1, "RGB5_A1",         "16bpp, 1-bit cutout alpha" },
                                        { TextureFormatPolicy::RGBA4,   "RGBA4",           "16bpp, smooth alpha (may band)" },
                                    };
                                    for (const FormatChoice& choice : kChoices) {
                                        if (ImGui::MenuItem(choice.label, nullptr, current == choice.policy)) {
                                            applyFormat(choice.policy);
                                        }
                                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", choice.hint);
                                    }
                                    ImGui::EndMenu();
                                }
                                if (hasSpriteEditorPackage() && ImGui::MenuItem("Open in Pixel Sprite Editor")) {
                                    loadPixelSpriteDocument(entry.path());
                                }
                                if (ImGui::MenuItem("Create 2.5D Sprite")) {
                                    addObject(ObjectType::Sprite25D, PathToUtf8(entry.path().stem()));
                                    if (!sceneObjects.empty()) {
                                        SceneObject& created = sceneObjects.back();
                                        created.albedoTexturePath = PathToUtf8(entry.path());
                                        created.material.textureFilter = MaterialProperties::TextureFilter::Point;
                                        if (Texture* tex = renderer.getTexture(created.albedoTexturePath, MaterialProperties::TextureFilter::Point)) {
                                            if (tex->GetWidth() > 0 && tex->GetHeight() > 0) {
                                                created.ui.size = glm::vec2(static_cast<float>(tex->GetWidth()),
                                                                            static_cast<float>(tex->GetHeight()));
                                            }
                                        }
                                        projectManager.currentProject.hasUnsavedChanges = true;
                                    }
                                }
                                if (has2DWorldPackage() && ImGui::MenuItem("Create Sprite2D")) {
                                    int canvasId = -1;
                                    for (const auto& obj : sceneObjects) {
                                        if (obj.hasUI && obj.ui.type == UIElementType::Canvas) {
                                            canvasId = obj.id;
                                            break;
                                        }
                                    }
                                    if (canvasId < 0) {
                                        addObject(ObjectType::Canvas, "Canvas");
                                        if (!sceneObjects.empty()) {
                                            canvasId = sceneObjects.back().id;
                                        }
                                    }
                                    addObject(ObjectType::Sprite2D, PathToUtf8(entry.path().stem()));
                                    if (!sceneObjects.empty()) {
                                        SceneObject& created = sceneObjects.back();
                                        created.albedoTexturePath = PathToUtf8(entry.path());
                                        created.material.textureFilter = MaterialProperties::TextureFilter::Point;
                                        if (Texture* tex = renderer.getTexture(created.albedoTexturePath, MaterialProperties::TextureFilter::Point)) {
                                            if (tex->GetWidth() > 0 && tex->GetHeight() > 0) {
                                                created.ui.size = glm::vec2(static_cast<float>(tex->GetWidth()),
                                                                            static_cast<float>(tex->GetHeight()));
                                            }
                                        }
                                        if (canvasId >= 0) {
                                            setParent(created.id, canvasId);
                                        }
                                        projectManager.currentProject.hasUnsavedChanges = true;
                                    }
                                }
                                if (hasSpritesheetPackage() && ImGui::MenuItem("Import Sprite Sheet...")) {
                                    pendingSpriteSheetPath = PathToUtf8(entry.path());
                                    std::snprintf(importSpriteSheetName, sizeof(importSpriteSheetName), "%s",
                                                  PathToUtf8(entry.path().stem()).c_str());
                                    importSpriteSheetTarget = isProject25DPipeline()
                                        ? SpriteSheetImportTarget::Sprite25D
                                        : (has2DWorldPackage()
                                            ? SpriteSheetImportTarget::Sprite2D
                                            : SpriteSheetImportTarget::UIImage);
                                    importSpriteSheetColumns = 4;
                                    importSpriteSheetRows = 4;
                                    importSpriteSheetFps = 12.0f;
                                    showImportSpriteSheetDialog = true;
                                }
                            }
                            if (entry.path().extension() == ".modupak" && ImGui::MenuItem("Import ModuPAK...")) {
                                openModuPakImportDialog(entry.path());
                            }
                            if (entry.path().extension() == ".moduobj" && ImGui::MenuItem("Import ModuOBJ...")) {
                                openModuObjImportDialog(entry.path());
                            }
                            if (entry.path().extension() == ".moduobj" && ImGui::MenuItem("Edit Packaged Object")) {
                                openModuObjEditor(entry.path());
                            }
                            if (ImGui::MenuItem("Export Into ModuPAK...")) {
                                std::vector<fs::path> seed = fileBrowser.selectedFiles.empty()
                                    ? std::vector<fs::path>{entry.path()}
                                    : fileBrowser.selectedFiles;
                                openModuPakExportDialog(seed);
                            }
                            ImGui::Separator();
                            if (ImGui::MenuItem("Open in File Explorer")) {
                                openPathInFileManager(entry.path());
                            }
                            if (ImGui::MenuItem("Move to Parent Folder")) {
                                movePathIntoDirectory(entry.path(), entry.path().parent_path());
                            }
                            if (!fileBrowser.projectRoot.empty() &&
                                normalizePath(entry.path().parent_path()) != normalizePath(fileBrowser.projectRoot) &&
                                ImGui::MenuItem("Move to Project Root")) {
                                movePathIntoDirectory(entry.path(), fileBrowser.projectRoot);
                            }
                            if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                                copySelectionToClipboard(false);
                            }
                            if (ImGui::MenuItem("Cut", "Ctrl+X")) {
                                copySelectionToClipboard(true);
                            }
                            if (entry.is_directory() &&
                                ImGui::MenuItem("Paste Into Folder", "Ctrl+V", false, !fileClipboard.empty())) {
                                pasteClipboardIntoDirectory(entry.path());
                            }
                            if (ImGui::MenuItem("Rename", "F2")) {
                                pendingRenamePath = entry.path();
                                std::string baseName = PathToUtf8(pendingRenamePath.filename());
                                std::strncpy(renameName, baseName.c_str(), sizeof(renameName) - 1);
                                renameName[sizeof(renameName) - 1] = '\0';
                                showRenamePopup = true;
                                triggerRenamePopup = true;
                                addConsoleMessage("Rename request: " + PathToUtf8(pendingRenamePath), ConsoleMessageType::Info);
                            }
                            if (ImGui::MenuItem(entry.is_directory() ? "Delete Folder" : "Delete File", "Del")) {
                                pendingDeletePath = entry.path();
                                showDeletePopup = true;
                                triggerDeletePopup = true;
                                addConsoleMessage("Delete request: " + PathToUtf8(pendingDeletePath), ConsoleMessageType::Info);
                            }
                            ImGui::EndPopup();
                        }

                        if (entry.is_directory() && ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_BROWSER_ENTRY")) {
                                handleMovePayloadToDirectory(payload, entry.path());
                            }
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                                handleMovePayloadToDirectory(payload, entry.path());
                            }
                            ImGui::EndDragDropTarget();
                        }

                        if (DragPreview::BeginSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                            std::string payloadPath = PathToUtf8(entry.path());
                            const char* payloadType = entry.is_directory() ? "FILE_BROWSER_ENTRY" : "FILE_PATH";
                            ImGui::SetDragDropPayload(payloadType, payloadPath.c_str(), payloadPath.size() + 1);
                            ImTextureID dragIcon = (ImTextureID)0;
                            ImVec2 dragUv0(0.0f, 1.0f), dragUv1(1.0f, 0.0f);
                            FileIcons::TryGetAtlasIcon(cached.category, cached.folderHasItems,
                                                       dragIcon, dragUv0, dragUv1);
                            DragPreview::SubmitMeta(cached.filename.c_str(), dragIcon, payloadType,
                                                    dragUv0, dragUv1);
                            DragPreview::EndSource();
                        }

                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndTable();
        }
        FileIcons::FlushIconBatch(drawList, ImGui::GetWindowPos(),
                                  ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));

    } else {
        // List View
        MODU_PROFILE_SCOPE("Project List Render", ProfilerSampleCategory::UI);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 1.0f));
        const float listRowHeight = fileBrowserTouch ? 50.0f : 34.0f;
        navRowStride = listRowHeight;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(fileBrowser.cachedEntries.size()), listRowHeight);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& entry = fileBrowser.entries[static_cast<size_t>(i)];
                const auto& cached = fileBrowser.cachedEntries[static_cast<size_t>(i)];
                const FileCategory category = cached.category;
                const bool isSelected = fileBrowser.selectedFileKeys.find(cached.selectionKey) != fileBrowser.selectedFileKeys.end();

                ImGui::PushID(i);

                const bool showRichPreview = (category == FileCategory::Scene ||
                                              category == FileCategory::Texture ||
                                              category == FileCategory::Model ||
                                              category == FileCategory::Video);
                const float rowHeight = listRowHeight;
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight))) {
                    selectFileEntry(i);

                    if (ImGui::IsMouseDoubleClicked(0)) {
                        openEntry(entry);
                    }
                }
                ImGui::PopItemFlag();
                ImVec2 rowMin = ImGui::GetItemRectMin();
                ImVec2 rowMax = ImGui::GetItemRectMax();
                bool rowHovered = ImGui::IsItemHovered() && !navSuppressHover;

                if (isSelected || rowHovered) {
                    ImU32 rowFill = isSelected ? IM_COL32(68, 95, 126, 190) : IM_COL32(44, 48, 56, 145);
                    drawList->AddRectFilled(rowMin, rowMax, rowFill, 5.0f);
                    if (isSelected) {
                        drawList->AddRect(rowMin, rowMax, IM_COL32(106, 158, 220, 235), 5.0f, 0, 1.2f);
                    } else {
                        drawList->AddRect(rowMin, rowMax, IM_COL32(82, 95, 112, 180), 5.0f, 0, 1.0f);
                    }
                }

                const float listIconSize = 15.0f;
                const float listPreviewSize = 30.0f;
                ImVec2 iconPos(rowMin.x + 7.0f,
                               rowMin.y + (rowMax.y - rowMin.y - (showRichPreview ? listPreviewSize : listIconSize)) * 0.5f);
                bool drewPreview = false;
                if (showRichPreview) {
                    ImVec2 previewMin = iconPos;
                    ImVec2 previewMax(iconPos.x + listPreviewSize, iconPos.y + listPreviewSize);
                    if (category == FileCategory::Scene) {
                        drewPreview = DrawSceneLogoPreview(renderer, drawList, previewMin, previewMax, 4.0f);
                    } else if (category == FileCategory::Texture) {
                        drewPreview = DrawTexturePreview(renderer, drawList, cached, previewMin, previewMax, 4.0f);
                        DrawTexturePreviewErrorTooltip(renderer, cached, previewMin, previewMax);
                    } else if (category == FileCategory::Video) {
                        drewPreview = DrawVideoPreview(renderer, drawList, cached, previewMin, previewMax, 4.0f);
                    } else {
                        drewPreview = DrawModelPreview(renderer, drawList, cached, previewMin, previewMax, 3000, 4.0f);
                    }
                }
                if (!drewPreview) {
                    FileIcons::DrawIcon(renderer, drawList, category, iconPos, listIconSize, getCategoryColor(category), cached.folderHasItems);
                }

                ImU32 nameColor = IM_COL32(220, 224, 230, 255);
                switch (category) {
                    case FileCategory::Folder:   nameColor = IM_COL32(242, 214, 132, 255); break;
                    case FileCategory::Scene:    nameColor = IM_COL32(158, 204, 250, 255); break;
                    case FileCategory::Model:    nameColor = IM_COL32(152, 224, 170, 255); break;
                    case FileCategory::Material: nameColor = IM_COL32(236, 205, 132, 255); break;
                    case FileCategory::Texture:  nameColor = IM_COL32(220, 171, 226, 255); break;
                    case FileCategory::Video:    nameColor = IM_COL32(238, 164, 182, 255); break;
                    default: break;
                }

                float textY = rowMin.y + 3.0f;
                float visualWidth = drewPreview ? listPreviewSize : listIconSize;
                float nameX = iconPos.x + visualWidth + 8.0f;
                float rightPad = 10.0f;
                float metaWidth = ImGui::CalcTextSize(cached.metadata.c_str()).x;
                float metaX = rowMax.x - rightPad - metaWidth;
                float maxNameWidth = ImMax(48.0f, metaX - nameX - 12.0f);

                const std::string& displayName = GetTruncatedLabel(sFileBrowserTextLayoutCache, cached, maxNameWidth);
                drawList->AddText(ImVec2(nameX, textY), nameColor, displayName.c_str());
                if (metaX > nameX + 72.0f) {
                    drawList->AddText(ImVec2(metaX, textY), IM_COL32(156, 166, 180, 245), cached.metadata.c_str());
                }

                if (fileContextMenuKeyRequested && cached.selectionKey == primarySelectionKey) {
                    ImGui::OpenPopup("FileContextMenu");
                }
                if (ImGui::BeginPopupContextItem("FileContextMenu")) {
                if (ImGui::MenuItem("Open", "Enter")) {
                    openEntry(entry);
                }
                #ifdef _WIN32
                if (!entry.is_directory() && ImGui::MenuItem("Open With...")) {
                    openPathWithDialog(entry.path());
                }
                #endif
                if (entry.is_directory() && ImGui::BeginMenu("New")) {
                    drawCreateMenu(entry.path());
                    ImGui::EndMenu();
                }
                if (entry.is_directory() && ImGui::MenuItem("Import Assets Here...")) {
                    queueImportAssetsDialog(entry.path());
                }
                if (fileBrowser.isModelFile(entry)) {
                    bool isObj = fileBrowser.isOBJFile(entry);
                    bool isRaw = IsRawMeshPath(entry.path());
                    bool isMMesh = IsMMeshPath(entry.path());
                    if (ImGui::MenuItem("Import to Scene")) {
                        std::string defaultName = PathToUtf8(entry.path().stem());
                        if (isObj) {
                            pendingOBJPath = PathToUtf8(entry.path());
                            strncpy(importOBJName, defaultName.c_str(), sizeof(importOBJName) - 1);
                            showImportOBJDialog = true;
                        } else {
                            pendingModelPath = PathToUtf8(entry.path());
                            strncpy(importModelName, defaultName.c_str(), sizeof(importModelName) - 1);
                            showImportModelDialog = true;
                        }
                    }
                    if (ImGui::MenuItem("Quick Import")) {
                        if (isObj) {
                            importOBJToScene(PathToUtf8(entry.path()), "");
                        } else {
                            importModelToScene(PathToUtf8(entry.path()), "");
                        }
                    }
                    if (!isRaw && !isMMesh && ImGui::MenuItem("Convert to Raw Mesh")) {
                        convertModelToRawMesh(PathToUtf8(entry.path()));
                    }
                }
                if (fileBrowser.getFileCategory(entry) == FileCategory::Material) {
                    if (ImGui::MenuItem("Apply to Selected")) {
                        if (SceneObject* sel = getSelectedObject()) {
                            sel->materialPath = PathToUtf8(entry.path());
                            loadMaterialFromFile(*sel);
                        }
                    }
                }
                if (fileBrowser.getFileCategory(entry) == FileCategory::Script) {
                    if (ImGui::MenuItem("Compile Script")) {
                        compileScriptFile(entry.path());
                    }
                }
                if (fileBrowser.getFileCategory(entry) == FileCategory::Texture) {
                    if (hasSpriteEditorPackage() && ImGui::MenuItem("Open in Pixel Sprite Editor")) {
                        loadPixelSpriteDocument(entry.path());
                    }
                    if (ImGui::MenuItem("Create 2.5D Sprite")) {
                        addObject(ObjectType::Sprite25D, PathToUtf8(entry.path().stem()));
                        if (!sceneObjects.empty()) {
                            SceneObject& created = sceneObjects.back();
                            created.albedoTexturePath = PathToUtf8(entry.path());
                            created.material.textureFilter = MaterialProperties::TextureFilter::Point;
                            if (Texture* tex = renderer.getTexture(created.albedoTexturePath, MaterialProperties::TextureFilter::Point)) {
                                if (tex->GetWidth() > 0 && tex->GetHeight() > 0) {
                                    created.ui.size = glm::vec2(static_cast<float>(tex->GetWidth()),
                                                                static_cast<float>(tex->GetHeight()));
                                }
                            }
                            projectManager.currentProject.hasUnsavedChanges = true;
                        }
                    }
                    if (has2DWorldPackage() && ImGui::MenuItem("Create Sprite2D")) {
                        int canvasId = -1;
                        for (const auto& obj : sceneObjects) {
                            if (obj.hasUI && obj.ui.type == UIElementType::Canvas) {
                                canvasId = obj.id;
                                break;
                            }
                        }
                        if (canvasId < 0) {
                            addObject(ObjectType::Canvas, "Canvas");
                            if (!sceneObjects.empty()) {
                                canvasId = sceneObjects.back().id;
                            }
                        }
                        addObject(ObjectType::Sprite2D, PathToUtf8(entry.path().stem()));
                        if (!sceneObjects.empty()) {
                            SceneObject& created = sceneObjects.back();
                            created.albedoTexturePath = PathToUtf8(entry.path());
                            created.material.textureFilter = MaterialProperties::TextureFilter::Point;
                            if (Texture* tex = renderer.getTexture(created.albedoTexturePath, MaterialProperties::TextureFilter::Point)) {
                                if (tex->GetWidth() > 0 && tex->GetHeight() > 0) {
                                    created.ui.size = glm::vec2(static_cast<float>(tex->GetWidth()),
                                                                static_cast<float>(tex->GetHeight()));
                                }
                            }
                            if (canvasId >= 0) {
                                setParent(created.id, canvasId);
                            }
                            projectManager.currentProject.hasUnsavedChanges = true;
                        }
                    }
                    if (hasSpritesheetPackage() && ImGui::MenuItem("Import Sprite Sheet...")) {
                        pendingSpriteSheetPath = PathToUtf8(entry.path());
                        std::snprintf(importSpriteSheetName, sizeof(importSpriteSheetName), "%s",
                                      PathToUtf8(entry.path().stem()).c_str());
                        importSpriteSheetTarget = isProject25DPipeline()
                            ? SpriteSheetImportTarget::Sprite25D
                            : (has2DWorldPackage()
                                ? SpriteSheetImportTarget::Sprite2D
                                : SpriteSheetImportTarget::UIImage);
                        importSpriteSheetColumns = 4;
                        importSpriteSheetRows = 4;
                        importSpriteSheetFps = 12.0f;
                        showImportSpriteSheetDialog = true;
                    }
                }
                if (entry.path().extension() == ".modupak" && ImGui::MenuItem("Import ModuPAK...")) {
                    openModuPakImportDialog(entry.path());
                }
                if (entry.path().extension() == ".moduobj" && ImGui::MenuItem("Import ModuOBJ...")) {
                    openModuObjImportDialog(entry.path());
                }
                if (entry.path().extension() == ".moduobj" && ImGui::MenuItem("Edit Packaged Object")) {
                    openModuObjEditor(entry.path());
                }
                if (ImGui::MenuItem("Export Into ModuPAK...")) {
                    std::vector<fs::path> seed = fileBrowser.selectedFiles.empty()
                        ? std::vector<fs::path>{entry.path()}
                        : fileBrowser.selectedFiles;
                    openModuPakExportDialog(seed);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open in File Explorer")) {
                    openPathInFileManager(entry.path());
                }
                if (ImGui::MenuItem("Move to Parent Folder")) {
                    movePathIntoDirectory(entry.path(), entry.path().parent_path());
                }
                if (!fileBrowser.projectRoot.empty() &&
                    normalizePath(entry.path().parent_path()) != normalizePath(fileBrowser.projectRoot) &&
                    ImGui::MenuItem("Move to Project Root")) {
                    movePathIntoDirectory(entry.path(), fileBrowser.projectRoot);
                }
                if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                    copySelectionToClipboard(false);
                }
                if (ImGui::MenuItem("Cut", "Ctrl+X")) {
                    copySelectionToClipboard(true);
                }
                if (entry.is_directory() &&
                    ImGui::MenuItem("Paste Into Folder", "Ctrl+V", false, !fileClipboard.empty())) {
                    pasteClipboardIntoDirectory(entry.path());
                }
                if (ImGui::MenuItem("Rename", "F2")) {
                    pendingRenamePath = entry.path();
                    std::string baseName = PathToUtf8(pendingRenamePath.filename());
                    std::strncpy(renameName, baseName.c_str(), sizeof(renameName) - 1);
                    renameName[sizeof(renameName) - 1] = '\0';
                    showRenamePopup = true;
                    triggerRenamePopup = true;
                    addConsoleMessage("Rename request: " + PathToUtf8(pendingRenamePath), ConsoleMessageType::Info);
                }
                if (ImGui::MenuItem(entry.is_directory() ? "Delete Folder" : "Delete File", "Del")) {
                    pendingDeletePath = entry.path();
                    showDeletePopup = true;
                    triggerDeletePopup = true;
                    addConsoleMessage("Delete request: " + PathToUtf8(pendingDeletePath), ConsoleMessageType::Info);
                }
                ImGui::EndPopup();
            }

            if (entry.is_directory() && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_BROWSER_ENTRY")) {
                    handleMovePayloadToDirectory(payload, entry.path());
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
                    handleMovePayloadToDirectory(payload, entry.path());
                }
                ImGui::EndDragDropTarget();
            }

                if (DragPreview::BeginSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    std::string payloadPath = PathToUtf8(entry.path());
                    const char* payloadType = entry.is_directory() ? "FILE_BROWSER_ENTRY" : "FILE_PATH";
                    ImGui::SetDragDropPayload(payloadType, payloadPath.c_str(), payloadPath.size() + 1);
                    ImTextureID dragIcon = (ImTextureID)0;
                    ImVec2 dragUv0(0.0f, 1.0f), dragUv1(1.0f, 0.0f);
                    FileIcons::TryGetAtlasIcon(cached.category, cached.folderHasItems,
                                               dragIcon, dragUv0, dragUv1);
                    DragPreview::SubmitMeta(cached.filename.c_str(), dragIcon, payloadType,
                                            dragUv0, dragUv1);
                    DragPreview::EndSource();
                }

                ImGui::PopID();
            }
        }

        ImGui::PopStyleVar();
        FileIcons::FlushIconBatch(drawList, ImGui::GetWindowPos(),
                                  ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                         ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    }

    const bool fileMainBackgroundLeftClicked =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered();
    if (fileMainBackgroundLeftClicked) {
        fileBrowser.selectedFile.clear();
        fileBrowser.selectedFiles.clear();
        fileBrowser.selectedFileKeys.clear();
        fileBrowser.selectionAnchorIndex = -1;
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_BROWSER_ENTRY")) {
            handleMovePayloadToDirectory(payload, fileBrowser.currentPath);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
            handleMovePayloadToDirectory(payload, fileBrowser.currentPath);
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextWindow("FileBrowserEmptyContext", ImGuiPopupFlags_NoOpenOverItems | ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !fileClipboard.empty())) {
            pasteClipboardIntoDirectory(fileBrowser.currentPath);
        }
        if (ImGui::MenuItem("Open in File Explorer")) {
            openPathInFileManager(fileBrowser.currentPath);
        }
        if (ImGui::MenuItem("Import Assets...")) {
            queueImportAssetsDialog(fileBrowser.currentPath);
        }
        if (ImGui::MenuItem("Import ModuPAK...")) {
            openModuPakImportDialog();
        }
        if (ImGui::MenuItem("Import ModuOBJ...")) {
            openModuObjImportDialog();
        }
        if (ImGui::MenuItem("Export Into ModuPAK...")) {
            openModuPakExportDialog({});
        }
        if (ImGui::MenuItem("Refresh")) {
            fileBrowser.needsRefresh = true;
        }
        ImGui::Separator();
        const bool scriptActionsEnabled =
            projectManager.currentProject.isLoaded && !isPlaying && !specMode && !testMode;
        if (ImGui::MenuItem("Reload Scripts", nullptr, false, scriptActionsEnabled)) {
            requestScriptDomainReload(false);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(scriptActionsEnabled
                                  ? "Unload every script module and its state, then rebuild what is stale"
                                  : "Stop play/spec/test mode first");
        }
        if (ImGui::MenuItem("Reload Scripts (Clear Compiled Cache)", nullptr, false,
                            scriptActionsEnabled)) {
            triggerReloadScriptsPopup = true;
            showReloadScriptsPopup = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(scriptActionsEnabled
                                  ? "Delete every compiled script binary, then rebuild all of them from scratch"
                                  : "Stop play/spec/test mode first");
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("New")) {
            drawCreateMenu(fileBrowser.currentPath);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    if (fileKeysActive && !ImGui::IsAnyItemActive()) {
        const ImGuiIO& io = ImGui::GetIO();
        const int entryCount = static_cast<int>(fileBrowser.entries.size());
        int currentIndex = -1;
        if (!primarySelectionKey.empty()) {
            for (int i = 0; i < entryCount && i < static_cast<int>(fileBrowser.cachedEntries.size()); ++i) {
                if (fileBrowser.cachedEntries[static_cast<size_t>(i)].selectionKey == primarySelectionKey) {
                    currentIndex = i;
                    break;
                }
            }
        }

        auto selectAndReveal = [&](int index) {
            if (entryCount <= 0) {
                return;
            }
            index = std::clamp(index, 0, entryCount - 1);
            selectFileEntry(index);
            navSuppressHover = true;
            if (navRowStride > 0.0f) {
                const int rowIndex = (navColumns > 1) ? index / navColumns : index;
                const float rowTop = static_cast<float>(rowIndex) * navRowStride;
                const float rowBottom = rowTop + navRowStride;
                const float scrollY = ImGui::GetScrollY();
                const float viewHeight = ImGui::GetWindowHeight();
                if (rowTop < scrollY) {
                    ImGui::SetScrollY(rowTop);
                } else if (rowBottom > scrollY + viewHeight) {
                    ImGui::SetScrollY(rowBottom - viewHeight);
                }
            }
        };

        auto navKeyPressed = [](ImGuiKey key) {
            return ImGui::GetKeyPressedAmount(key, 0.32f, 0.09f) > 0;
        };

        if (entryCount > 0 && !io.KeyCtrl && !io.KeyAlt) {
            if (navKeyPressed(ImGuiKey_RightArrow)) {
                selectAndReveal(currentIndex < 0 ? 0 : currentIndex + 1);
            } else if (navKeyPressed(ImGuiKey_LeftArrow)) {
                selectAndReveal(currentIndex < 0 ? 0 : currentIndex - 1);
            } else if (navKeyPressed(ImGuiKey_DownArrow)) {
                selectAndReveal(currentIndex < 0 ? 0 : currentIndex + navColumns);
            } else if (navKeyPressed(ImGuiKey_UpArrow)) {
                selectAndReveal(currentIndex < 0 ? 0 : currentIndex - navColumns);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                selectAndReveal(0);
            } else if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
                selectAndReveal(entryCount - 1);
            }
        }

        if (currentIndex >= 0 &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
            openEntry(fileBrowser.entries[static_cast<size_t>(currentIndex)]);
        }
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            fileBrowser.navigateUp();
        }
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
            fileBrowser.navigateBack();
        }
        if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            fileBrowser.navigateForward();
        }

        if (!fileBrowser.selectedFile.empty() && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            pendingRenamePath = fileBrowser.selectedFile;
            std::string baseName = PathToUtf8(pendingRenamePath.filename());
            std::strncpy(renameName, baseName.c_str(), sizeof(renameName) - 1);
            renameName[sizeof(renameName) - 1] = '\0';
            showRenamePopup = true;
            triggerRenamePopup = true;
        }
        if (!fileBrowser.selectedFile.empty() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            pendingDeletePath = fileBrowser.selectedFile;
            showDeletePopup = true;
            triggerDeletePopup = true;
        }

        if (io.KeyCtrl && !io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_A, false) && entryCount > 0) {
                clearFileSelection();
                for (const auto& e : fileBrowser.entries) {
                    addFileSelection(e.path());
                }
                fileBrowser.selectedFile = fileBrowser.selectedFiles.empty()
                    ? fs::path()
                    : fileBrowser.selectedFiles.back();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                copySelectionToClipboard(false);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
                copySelectionToClipboard(true);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
                pasteClipboardIntoDirectory(fileBrowser.currentPath);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_D, false) && !fileBrowser.selectedFiles.empty()) {
                const std::vector<fs::path> sources = fileBrowser.selectedFiles;
                for (const fs::path& source : sources) {
                    importPathIntoDirectory(source, source.parent_path());
                }
            }
        }
    }

    ImGui::EndChild();
    ImGui::EndChild();

    if (settingsDirty) {
        saveEditorUserSettings();
    }

    auto resolveCardIcon = [&](const char* iconPath) -> CardModalIcon {
        if (rendererInitialized) {
            if (Texture* icon = renderer.getTexture(iconPath, MaterialProperties::TextureFilter::Point);
                icon && icon->GetID()) {
                return { static_cast<ImTextureID>(icon->GetID()), true };
            }
        }
        if (usingVulkan() && vulkanRendererInitialized && vulkanRenderer) {
            ImTextureID icon = vulkanRenderer->getOrCreateUIImage(iconPath);
            if (icon != static_cast<ImTextureID>(0)) return { icon, false };
        }
        return {};
    };

    if (triggerDeletePopup) {
        playEditorFeedbackPreview("Resources/Sounds/Info.mp3", 0.95f, false, EditorFeedbackSoundCategory::Other);
        ImGui::OpenPopup("Delete this item?");
        triggerDeletePopup = false;
    }
    if (beginCardModal("Delete this item?", 0.0f, &showDeletePopup,
                       resolveCardIcon("Resources/Engine-Root/Pop-up Confirmation Icons/File Deletion.png"))) {
        const std::string deleteMsg = "\"" + PathToUtf8(pendingDeletePath.filename()) +
                                      "\" will be deleted from your project.";
        cardModalText(deleteMsg.c_str());
        cardModalText(PathToUtf8(pendingDeletePath).c_str());
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
            showDeletePopup = false;
            ImGui::CloseCurrentPopup();
        }
        if (cardModalButton("Delete", CardButtonKind::Danger, 1, 2)) {
            std::error_code ec;
            fs::remove_all(pendingDeletePath, ec);
            if (fileBrowser.selectedFile == pendingDeletePath) {
                fileBrowser.selectedFile.clear();
            }
            removeFileSelection(pendingDeletePath);
            if (fileBrowser.selectedFile.empty() && !fileBrowser.selectedFiles.empty()) {
                fileBrowser.selectedFile = fileBrowser.selectedFiles.back();
            }
            fileBrowser.needsRefresh = true;
            if (ec) {
                addConsoleMessage("Delete failed: " + PathToUtf8(pendingDeletePath) + " (" + ec.message() + ")",
                                  ConsoleMessageType::Error);
            } else {
                addConsoleMessage("Deleted: " + PathToUtf8(pendingDeletePath), ConsoleMessageType::Success);
            }
            showDeletePopup = false;
            ImGui::CloseCurrentPopup();
        }
        endCardModal();
    }

    if (triggerReloadScriptsPopup) {
        playEditorFeedbackPreview("Resources/Sounds/Info.mp3", 0.95f, false, EditorFeedbackSoundCategory::Other);
        ImGui::OpenPopup("Reload scripts from scratch?");
        triggerReloadScriptsPopup = false;
    }
    if (beginCardModal("Reload scripts from scratch?", 0.0f, &showReloadScriptsPopup)) {
        cardModalText("Every compiled script binary will be deleted and rebuilt, and all "
                      "loaded script modules will be unloaded along with their state.");
        fs::path compiledScriptsDir = scriptAutoCompileConfigValid ? scriptAutoCompileConfig.outDir
                                                                   : fs::path();
        if (compiledScriptsDir.empty() && projectManager.currentProject.isLoaded) {
            compiledScriptsDir = projectManager.currentProject.projectPath / "Library" / "CompiledScripts";
        }
        cardModalText(PathToUtf8(compiledScriptsDir).c_str());
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
            showReloadScriptsPopup = false;
            ImGui::CloseCurrentPopup();
        }
        if (cardModalButton("Reload", CardButtonKind::Danger, 1, 2)) {
            requestScriptDomainReload(true);
            showReloadScriptsPopup = false;
            ImGui::CloseCurrentPopup();
        }
        endCardModal();
    }

    if (triggerRenamePopup) {
        playEditorFeedbackPreview("Resources/Sounds/Info.mp3", 0.95f, false, EditorFeedbackSoundCategory::Other);
        ImGui::OpenPopup("Rename Item");
        triggerRenamePopup = false;
    }
    if (beginCardModal("Rename Item", 0.0f, &showRenamePopup,
                       resolveCardIcon("Resources/Engine-Root/Pop-up Confirmation Icons/File Renaming.png"))) {
        const std::string renameMsg = "Choose a new name for \"" +
                                      PathToUtf8(pendingRenamePath.filename()) + "\".";
        cardModalText(renameMsg.c_str());
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool renameSubmitted = ImGui::InputText("##NewName", renameName, sizeof(renameName),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        bool canRename = std::strlen(renameName) > 0;
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
            showRenamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::BeginDisabled(!canRename);
        if (cardModalButton("Rename", CardButtonKind::Primary, 1, 2) || (renameSubmitted && canRename)) {
            fs::path newPath = pendingRenamePath.parent_path() / renameName;
            if (newPath == pendingRenamePath) {
                addConsoleMessage("Rename skipped: name unchanged", ConsoleMessageType::Info);
            } else if (fs::exists(newPath)) {
                addConsoleMessage("Rename failed: target exists " + PathToUtf8(newPath), ConsoleMessageType::Error);
            } else {
                std::error_code ec;
                fs::rename(pendingRenamePath, newPath, ec);
                if (ec) {
                    addConsoleMessage("Rename failed: " + PathToUtf8(pendingRenamePath) + " (" + ec.message() + ")",
                                      ConsoleMessageType::Error);
                } else {
                    if (fileBrowser.selectedFile == pendingRenamePath) {
                        fileBrowser.selectedFile = newPath;
                    }
                    const bool wasSelected =
                        fileBrowser.selectedFileKeys.find(fileSelectionKey(pendingRenamePath)) !=
                        fileBrowser.selectedFileKeys.end();
                    if (wasSelected) {
                        removeFileSelection(pendingRenamePath);
                        addFileSelection(newPath);
                    }
                    fileBrowser.needsRefresh = true;
                    addConsoleMessage("Renamed to: " + PathToUtf8(newPath), ConsoleMessageType::Success);
                }
            }
            showRenamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        endCardModal();
    }

    if (triggerImportAssetsPopup) {
        ImGui::OpenPopup("Import Assets");
        triggerImportAssetsPopup = false;
    }
    if (ImGui::BeginPopupModal("Import Assets", &showImportAssetsPopup, ImGuiWindowFlags_AlwaysAutoResize)) {
        fs::path targetDir = pendingImportTargetPath.empty() ? fileBrowser.currentPath : pendingImportTargetPath;
        ImGui::Text("Import assets into:");
        ImGui::TextDisabled("%s", PathToUtf8(targetDir).c_str());
        ImGui::Spacing();
#ifdef __ANDROID__
        ImGui::TextWrapped("Use the Android file explorer to import one or more files.");
#else
        ImGui::TextWrapped("Use your file explorer to import a single file or an entire folder's contents.");
#endif
        const bool nativePickerAvailable = supportsNativeImportPathPicker();
#ifdef __ANDROID__
        ImGui::BeginDisabled(!nativePickerAvailable || androidImportPickerPending);
        if (ImGui::Button(androidImportPickerPending ? "Waiting for Android Picker..." : "Select Files...",
                          ImVec2(220, 0))) {
            std::string pickerError;
            if (Modularity::AndroidRuntime::RequestFilePicker(true, pickerError)) {
                androidImportPickerPending = true;
                androidImportPickerTargetPath = targetDir;
            } else {
                addConsoleMessage("Android file picker failed: " + pickerError, ConsoleMessageType::Error);
            }
        }
        ImGui::EndDisabled();
        if (!nativePickerAvailable) {
            ImGui::TextDisabled("Android file picker unavailable in this APK.");
        } else if (androidImportPickerPending) {
            ImGui::TextDisabled("Waiting for selected files...");
        }
#else
        ImGui::BeginDisabled(!nativePickerAvailable);
        if (ImGui::Button("Select File...", ImVec2(180, 0))) {
            if (auto selectedPath = chooseImportFilePath(targetDir)) {
                if (importPathIntoDirectory(*selectedPath, targetDir)) {
                    showImportAssetsPopup = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Select Folder Contents...", ImVec2(220, 0))) {
            if (auto selectedFolder = chooseImportFolderPath(targetDir)) {
                if (importDirectoryContentsIntoDirectory(*selectedFolder, targetDir)) {
                    showImportAssetsPopup = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndDisabled();
        if (!nativePickerAvailable) {
            ImGui::TextDisabled("Native picker unavailable. Install zenity/kdialog or paste paths below.");
        }
#endif
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("Paste one or more source paths. Separate paths with new lines, semicolons, or commas.");
        ImGui::InputTextMultiline("##ImportAssetPaths", importAssetPaths, sizeof(importAssetPaths),
                                  ImVec2(520.0f, 120.0f));
        bool canImport = importAssetPaths[0] != '\0';
        ImGui::BeginDisabled(!canImport);
        if (ImGui::Button("Import", ImVec2(120, 0))) {
            auto trim = [](std::string value) {
                auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
                while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
                    value.erase(value.begin());
                }
                while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
                    value.pop_back();
                }
                return value;
            };

            int importedCount = 0;
            int failedCount = 0;
            std::string raw(importAssetPaths);
            size_t start = 0;
            for (size_t i = 0; i <= raw.size(); ++i) {
                const bool split = (i == raw.size()) || raw[i] == '\n' || raw[i] == '\r' || raw[i] == ';' || raw[i] == ',';
                if (!split) continue;
                std::string token = trim(raw.substr(start, i - start));
                start = i + 1;
                if (token.empty()) continue;

                std::error_code absEc;
                fs::path source = fs::absolute(fs::path(token), absEc);
                if (absEc) {
                    source = fs::path(token);
                }
                if (importPathIntoDirectory(source, targetDir)) {
                    ++importedCount;
                } else {
                    ++failedCount;
                }
            }

            if (importedCount > 0) {
                addConsoleMessage("Imported " + std::to_string(importedCount) + " asset(s).",
                                  ConsoleMessageType::Success);
            }
            if (failedCount > 0) {
                addConsoleMessage("Failed to import " + std::to_string(failedCount) + " path(s).",
                                  ConsoleMessageType::Warning);
            }
            if (importedCount > 0 || failedCount > 0) {
                showImportAssetsPopup = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            showImportAssetsPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (triggerImportLanguagePopup) {
        ImGui::OpenPopup(Loc::WindowRef("Translate Imported Scripts"));
        triggerImportLanguagePopup = false;
        importLanguageDontAskAgain = false;
    }
    if (ImGui::BeginPopupModal(Loc::Window("DIALOG_TRANSLATE_IMPORT_TITLE", "Translate Imported Scripts"),
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const ModuCPPLang::LanguagePack& projectLanguage =
            ModuCPPLang::FindLanguageOrCanonical(projectManager.effectiveModuCppLanguage());

        std::string sourceLanguages;
        for (const std::string& id : pendingLanguageTranslationSourceIds) {
            if (!sourceLanguages.empty()) sourceLanguages += ", ";
            sourceLanguages += languagePackDisplayName(id);
        }
        if (sourceLanguages.empty()) sourceLanguages = "another supported language";

        ImGui::TextWrapped(Loc::T("DIALOG_TRANSLATE_IMPORT_BODY", "%d imported script(s) are written in %s."),
                           static_cast<int>(pendingLanguageTranslationFiles.size()), sourceLanguages.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(Loc::T("DIALOG_TRANSLATE_IMPORT_QUESTION",
                                  "Translate them to this project's ModuCPP syntax language (%s)?"),
                           projectLanguage.displayName.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Loc::T("DIALOG_TRANSLATE_IMPORT_NOTE1",
                                         "They compile and behave identically either way."));
        ImGui::TextDisabled("%s", Loc::T("DIALOG_TRANSLATE_IMPORT_NOTE2",
                                         "Only language keywords change; class, field, method and asset names do not."));
        ImGui::Spacing();

        const int maxListed = 6;
        int listed = 0;
        for (const fs::path& file : pendingLanguageTranslationFiles) {
            if (listed >= maxListed) {
                ImGui::TextDisabled(Loc::T("DIALOG_TRANSLATE_IMPORT_MORE", "... and %d more"),
                                    static_cast<int>(pendingLanguageTranslationFiles.size()) - maxListed);
                break;
            }
            ImGui::BulletText("%s", PathToUtf8(file.filename()).c_str());
            ++listed;
        }

        ImGui::Spacing();
        ImGui::Checkbox(Loc::T("COMMON_DONT_ASK_AGAIN", "Don't ask again"), &importLanguageDontAskAgain);
        ImGui::Spacing();

        auto closeImportLanguagePopup = [&](bool translate) {
            if (translate) {
                runImportTranslation(pendingLanguageTranslationFiles, projectLanguage);
            } else {
                addConsoleMessage("Kept " + std::to_string(pendingLanguageTranslationFiles.size()) +
                                  " imported script(s) in their original ModuCPP syntax language.",
                                  ConsoleMessageType::Info);
            }
            if (importLanguageDontAskAgain) {
                projectManager.currentProject.languageSettings.importTranslatePolicy =
                    translate ? ProjectImportTranslatePolicy::Always : ProjectImportTranslatePolicy::Never;
                projectManager.currentProject.saveProjectFile();
            }
            pendingLanguageTranslationFiles.clear();
            pendingLanguageTranslationSourceIds.clear();
            ImGui::CloseCurrentPopup();
        };

        if (ImGui::Button(Loc::T("COMMON_TRANSLATE", "Translate"), ImVec2(150, 0))) {
            closeImportLanguagePopup(true);
        }
        ImGui::SameLine();
        if (ImGui::Button(Loc::T("COMMON_KEEP_ORIGINAL", "Keep Original"), ImVec2(150, 0))) {
            closeImportLanguagePopup(false);
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    const double __fbMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - __fbStart).count();
    if (__fbMs > Modularity::HardwareProfile::TimingBudget(100.0)) {
        std::fprintf(stderr, "[ModuTimer] fileBrowserPanel %.1f ms\n", __fbMs);
    }
}
#pragma endregion
