#include "../../EditorLocalization.h"
#include "Engine.h"
#include "MaterialAssetUtils.h"
#include "ModelLoader.h"
#include "../../SpritesheetFormat.h"
#include "../../DragPreviewOverlay.h"
#include "../../Modu2DStats.h"
#include "../../SceneAssetReferences.h"
#include "../SceneObjectIcons.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <functional>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <regex>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <future>
#include <chrono>
#include <future>
#include <type_traits>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace Loc = Modularity::Loc;

namespace ImGui {
    bool BufferingBar(const char* label, float value, const ImVec2& size_arg, const ImU32& bg_col, const ImU32& fg_col);
}

#pragma region Hierarchy Helpers
namespace {
    // Unity-style "missing reference" marker. Filesystem checks are cached per object and only run
    // for rows that are drawn, re-checked every few seconds or immediately when a path changes, and
    // capped per frame so opening a large scene cannot stall on thousands of stat calls.
    struct HierarchyMissingAssetState {
        size_t signature = 0;
        double checkedAt = -1.0;
        std::string firstMissing;  // empty = nothing missing
        int missingCount = 0;
    };
    std::unordered_map<int, HierarchyMissingAssetState> gHierarchyMissingAssets;
    int gHierarchyMissingCheckFrame = -1;
    int gHierarchyMissingChecksThisFrame = 0;

    const HierarchyMissingAssetState& GetHierarchyMissingAssetState(const SceneObject& obj) {
        size_t signature = 0x9e3779b97f4a7c15ull;
        ForEachSceneObjectAssetReference(obj,
            [&](const std::string& value, SceneAssetRefKind, const char*, const char*) {
                signature ^= std::hash<std::string>{}(value) + 0x9e3779b97f4a7c15ull +
                             (signature << 6) + (signature >> 2);
                return true;
            });

        HierarchyMissingAssetState& state = gHierarchyMissingAssets[obj.id];
        const double now = ImGui::GetTime();
        const bool pathsChanged = state.signature != signature;
        if (!pathsChanged && state.checkedAt >= 0.0 && now - state.checkedAt < 3.0) {
            return state;
        }

        const int frame = ImGui::GetFrameCount();
        if (frame != gHierarchyMissingCheckFrame) {
            gHierarchyMissingCheckFrame = frame;
            gHierarchyMissingChecksThisFrame = 0;
        }
        if (pathsChanged) {
            state = HierarchyMissingAssetState{};
            state.signature = signature;
        }
        if (gHierarchyMissingChecksThisFrame >= 24) {
            return state;
        }
        ++gHierarchyMissingChecksThisFrame;

        state.firstMissing.clear();
        state.missingCount = 0;
        ForEachSceneObjectAssetReference(obj,
            [&](const std::string& value, SceneAssetRefKind, const char*, const char* label) {
                if (SceneAssetReferenceMissing(value)) {
                    if (state.firstMissing.empty()) {
                        state.firstMissing = std::string(label) + ": " + value;
                    }
                    ++state.missingCount;
                }
                return true;
            });
        state.checkedAt = now;
        return state;
    }

    ImGuiID HierarchyExpandStorageKey(int objectId) {
        return static_cast<ImGuiID>(0x40000000u + static_cast<uint32_t>(objectId));
    }

    ImU32 GetHierarchyTypeColor(const SceneObject& obj) {
        if (!obj.scripts.empty()) return IM_COL32(255, 175, 90, 235);
        if (obj.hasCamera) return IM_COL32(110, 175, 235, 220);
        if (obj.hasLight) return IM_COL32(255, 200, 90, 220);
        if (obj.hasLight2D) return IM_COL32(255, 228, 108, 230);
        if (obj.hasShadowCaster2D) return IM_COL32(120, 150, 190, 230);
        if (obj.hasPostFX) return IM_COL32(200, 140, 230, 220);
        if (obj.hasUI) return IM_COL32(160, 210, 255, 220);
        if (obj.hasRenderer) {
            switch (obj.renderType) {
                case RenderType::OBJMesh:
                case RenderType::Model:
                case RenderType::Sprite:
                    return IM_COL32(120, 200, 150, 220);
                case RenderType::Mirror:
                    return IM_COL32(180, 200, 210, 220);
                case RenderType::Plane:
                    return IM_COL32(170, 180, 190, 220);
                case RenderType::Torus:
                    return IM_COL32(155, 215, 180, 220);
                case RenderType::Cube:
                case RenderType::Sphere:
                case RenderType::Capsule:
                case RenderType::None:
                default:
                    break;
            }
        }
        return IM_COL32(130, 150, 170, 220);
    }

    bool HierarchyNameMatchesFilter(const std::string& name, const std::string& lowerFilter) {
        if (lowerFilter.empty()) return true;
        const size_t filterSize = lowerFilter.size();
        if (filterSize > name.size()) return false;
        for (size_t start = 0; start + filterSize <= name.size(); ++start) {
            size_t i = 0;
            for (; i < filterSize; ++i) {
                const unsigned char c = static_cast<unsigned char>(name[start + i]);
                const char lower = static_cast<char>(std::tolower(c));
                if (lower != lowerFilter[i]) break;
            }
            if (i == filterSize) return true;
        }
        return false;
    }

    struct HierarchyNavRow {
        int id = -1;
        int parentId = -1;
        ImGuiID nodeId = 0;
        bool open = false;
        bool hasChildren = false;
    };

    struct HierarchyFrameCache {
        std::unordered_set<int> selectedIds;
        std::vector<HierarchyNavRow> navRows;
        int scrollToObjectId = -1;
        bool contextMenuKeyRequested = false;
    };

    int gHierarchyRenameRequestId = -1;

    bool gHierarchySuppressMouseHover = false;

    int gHierarchyNavCaptureSuppress = 0;

    struct HierarchyRowCacheEntry {
        int id = -1;
        int depth = 0;
        int parentId = -1;
        uint64_t ancestorHasNextMask = 0;
        bool isLast = false;
        bool hasChildren = false;
        bool expanded = false;
        ImU32 iconColor = 0;
        std::string name;
    };

    struct HierarchyPanelCache {
        std::vector<HierarchyRowCacheEntry> rows;
        std::string filter;
        uint64_t sceneFingerprint = 0;
        bool dirty = true;
    };

    struct ConsoleRowMetrics {
        std::string header;
        ImVec2 headerSize = ImVec2(0.0f, 0.0f);
        ImVec2 messageSize = ImVec2(0.0f, 0.0f);
        float rowWidth = 0.0f;
        float rowHeight = 0.0f;
        float textWidth = 0.0f;
    };

    struct ConsolePanelCache {
        size_t entryCount = 0;
        bool wrapText = false;
        bool iconsAvailable = false;
        ProjectConsoleTone tone = ProjectConsoleTone::Fun;
        float contentWidth = 0.0f;
        uint64_t fingerprint = 0;
        std::vector<ConsoleRowMetrics> rows;
    };

    HierarchyFrameCache gHierarchyFrameCache;
    HierarchyPanelCache gHierarchyPanelCache;
    ConsolePanelCache gConsolePanelCache;

    uint64_t hashCombine64(uint64_t seed, uint64_t value) {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }

    template <typename Entry>
    uint64_t hashConsoleEntryFingerprint(const Entry& entry) {
        uint64_t hash = std::hash<std::string>{}(entry.timestamp);
        hash = hashCombine64(hash, std::hash<std::string>{}(entry.message));
        hash = hashCombine64(hash, static_cast<uint64_t>(entry.type));
        return hash;
    }

    template <typename Entry>
    uint64_t computeConsoleLogFingerprint(const std::vector<Entry>& log) {
        uint64_t fingerprint = hashCombine64(0x6d6f64756c617265ULL, static_cast<uint64_t>(log.size()));
        if (log.empty()) {
            return fingerprint;
        }

        fingerprint = hashCombine64(fingerprint, hashConsoleEntryFingerprint(log.front()));
        if (log.size() > 1) {
            fingerprint = hashCombine64(fingerprint, hashConsoleEntryFingerprint(log.back()));
        }
        if (log.size() > 2) {
            fingerprint = hashCombine64(fingerprint, hashConsoleEntryFingerprint(log[log.size() / 2]));
        }
        return fingerprint;
    }

    struct SvgPathSpec {
        const char* d;
        bool stroke;
    };

    struct SvgIconSpec {
        float viewW;
        float viewH;
        const SvgPathSpec* paths;
        int pathCount;
    };

    struct SvgSubpath {
        std::vector<ImVec2> points;
        bool closed = false;
        bool stroke = false;
    };

    struct SvgIconCache {
        bool built = false;
        std::vector<SvgSubpath> subpaths;
    };

    static bool IsCommandChar(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    static void SkipSvgSeparators(const char*& s) {
        while (*s) {
            if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r' || *s == ',') {
                ++s;
                continue;
            }
            break;
        }
    }

    static bool ParseSvgNumber(const char*& s, float& out) {
        SkipSvgSeparators(s);
        if (!*s) return false;
        char* end = nullptr;
        out = strtof(s, &end);
        if (end == s) return false;
        s = end;
        return true;
    }

    static ImVec2 SvgLerp(const ImVec2& a, const ImVec2& b, float t) {
        return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    }

    static ImVec2 SvgCubic(const ImVec2& p0, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, float t) {
        ImVec2 a = SvgLerp(p0, p1, t);
        ImVec2 b = SvgLerp(p1, p2, t);
        ImVec2 c = SvgLerp(p2, p3, t);
        ImVec2 d = SvgLerp(a, b, t);
        ImVec2 e = SvgLerp(b, c, t);
        return SvgLerp(d, e, t);
    }

    static void AppendSvgCubic(std::vector<ImVec2>& pts, const ImVec2& p0, const ImVec2& p1,
                               const ImVec2& p2, const ImVec2& p3, int segments) {
        for (int i = 1; i <= segments; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(segments);
            pts.push_back(SvgCubic(p0, p1, p2, p3, t));
        }
    }

    static float SvgVectorAngle(const ImVec2& u, const ImVec2& v) {
        float dot = u.x * v.x + u.y * v.y;
        float det = u.x * v.y - u.y * v.x;
        return std::atan2(det, dot);
    }

    static void AppendSvgArc(std::vector<ImVec2>& pts, const ImVec2& start, const ImVec2& end,
                             float rx, float ry, float xAxisRotationDeg, bool largeArc, bool sweep) {
        if (rx == 0.0f || ry == 0.0f) {
            pts.push_back(end);
            return;
        }

        float phi = xAxisRotationDeg * (IM_PI / 180.0f);
        float cosPhi = std::cos(phi);
        float sinPhi = std::sin(phi);

        float dx2 = (start.x - end.x) * 0.5f;
        float dy2 = (start.y - end.y) * 0.5f;
        float x1p = cosPhi * dx2 + sinPhi * dy2;
        float y1p = -sinPhi * dx2 + cosPhi * dy2;

        rx = std::fabs(rx);
        ry = std::fabs(ry);

        float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
        if (lambda > 1.0f) {
            float s = std::sqrt(lambda);
            rx *= s;
            ry *= s;
        }

        float rx2 = rx * rx;
        float ry2 = ry * ry;
        float x1p2 = x1p * x1p;
        float y1p2 = y1p * y1p;

        float denom = (rx2 * y1p2 + ry2 * x1p2);
        float num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
        float coef = 0.0f;
        if (denom > 0.0f) {
            float sign = (largeArc == sweep) ? -1.0f : 1.0f;
            coef = sign * std::sqrt(std::max(0.0f, num / denom));
        }

        float cxp = coef * (rx * y1p / ry);
        float cyp = coef * (-ry * x1p / rx);

        float cx = cosPhi * cxp - sinPhi * cyp + (start.x + end.x) * 0.5f;
        float cy = sinPhi * cxp + cosPhi * cyp + (start.y + end.y) * 0.5f;

        ImVec2 v1((x1p - cxp) / rx, (y1p - cyp) / ry);
        ImVec2 v2((-x1p - cxp) / rx, (-y1p - cyp) / ry);

        float startAngle = std::atan2(v1.y, v1.x);
        float deltaAngle = SvgVectorAngle(v1, v2);
        if (!sweep && deltaAngle > 0.0f) deltaAngle -= 2.0f * IM_PI;
        if (sweep && deltaAngle < 0.0f) deltaAngle += 2.0f * IM_PI;

        float absDelta = std::fabs(deltaAngle);
        int segments = std::max(4, static_cast<int>(std::ceil(absDelta / (IM_PI / 8.0f))));
        for (int i = 1; i <= segments; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(segments);
            float angle = startAngle + deltaAngle * t;
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);
            float x = cx + cosPhi * rx * cosA - sinPhi * ry * sinA;
            float y = cy + sinPhi * rx * cosA + cosPhi * ry * sinA;
            pts.push_back(ImVec2(x, y));
        }
    }

    static void FinalizeSvgSubpath(std::vector<SvgSubpath>& out, std::vector<ImVec2>& current, bool closed, bool stroke) {
        if (current.size() < 2) {
            current.clear();
            return;
        }
        if (closed && current.size() > 2) {
            if (current.front().x != current.back().x || current.front().y != current.back().y) {
                current.push_back(current.front());
            }
        }
        SvgSubpath sub;
        sub.points = std::move(current);
        sub.closed = closed;
        sub.stroke = stroke;
        out.push_back(std::move(sub));
        current.clear();
    }

    static void ParseSvgPathData(const char* d, std::vector<SvgSubpath>& out, bool stroke) {
        const char* s = d;
        char cmd = 0;
        ImVec2 cur(0, 0);
        ImVec2 start(0, 0);
        ImVec2 lastCtrl(0, 0);
        bool hasCtrl = false;
        std::vector<ImVec2> current;

        while (*s) {
            SkipSvgSeparators(s);
            if (!*s) break;
            if (IsCommandChar(*s)) {
                cmd = *s++;
            } else if (!cmd) {
                break;
            }

            bool relative = (cmd >= 'a' && cmd <= 'z');
            char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

            if (upper == 'M') {
                float x, y;
                if (!ParseSvgNumber(s, x) || !ParseSvgNumber(s, y)) break;
                if (relative) {
                    cur.x += x;
                    cur.y += y;
                } else {
                    cur = ImVec2(x, y);
                }
                FinalizeSvgSubpath(out, current, false, stroke);
                current.push_back(cur);
                start = cur;
                hasCtrl = false;

                while (ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
                    ImVec2 p = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
                    cur = p;
                    current.push_back(cur);
                }
                continue;
            }

            if (upper == 'Z') {
                FinalizeSvgSubpath(out, current, true, stroke);
                cur = start;
                hasCtrl = false;
                continue;
            }

            if (upper == 'L') {
                float x, y;
                while (ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
                    cur = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
                    current.push_back(cur);
                }
                hasCtrl = false;
                continue;
            }

            if (upper == 'H') {
                float x;
                while (ParseSvgNumber(s, x)) {
                    cur.x = relative ? cur.x + x : x;
                    current.push_back(cur);
                }
                hasCtrl = false;
                continue;
            }

            if (upper == 'V') {
                float y;
                while (ParseSvgNumber(s, y)) {
                    cur.y = relative ? cur.y + y : y;
                    current.push_back(cur);
                }
                hasCtrl = false;
                continue;
            }

            if (upper == 'C') {
                float x1, y1, x2, y2, x, y;
                while (ParseSvgNumber(s, x1) && ParseSvgNumber(s, y1) &&
                       ParseSvgNumber(s, x2) && ParseSvgNumber(s, y2) &&
                       ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
                    ImVec2 p1 = relative ? ImVec2(cur.x + x1, cur.y + y1) : ImVec2(x1, y1);
                    ImVec2 p2 = relative ? ImVec2(cur.x + x2, cur.y + y2) : ImVec2(x2, y2);
                    ImVec2 p3 = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
                    AppendSvgCubic(current, cur, p1, p2, p3, 12);
                    cur = p3;
                    lastCtrl = p2;
                    hasCtrl = true;
                }
                continue;
            }

            if (upper == 'S') {
                float x2, y2, x, y;
                while (ParseSvgNumber(s, x2) && ParseSvgNumber(s, y2) &&
                       ParseSvgNumber(s, x) && ParseSvgNumber(s, y)) {
                    ImVec2 p1 = hasCtrl ? ImVec2(cur.x * 2.0f - lastCtrl.x, cur.y * 2.0f - lastCtrl.y) : cur;
                    ImVec2 p2 = relative ? ImVec2(cur.x + x2, cur.y + y2) : ImVec2(x2, y2);
                    ImVec2 p3 = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
                    AppendSvgCubic(current, cur, p1, p2, p3, 12);
                    cur = p3;
                    lastCtrl = p2;
                    hasCtrl = true;
                }
                continue;
            }

            if (upper == 'A') {
                float rx, ry, xRot, largeFlag, sweepFlag, x, y;
                while (ParseSvgNumber(s, rx) && ParseSvgNumber(s, ry) &&
                       ParseSvgNumber(s, xRot) && ParseSvgNumber(s, largeFlag) &&
                       ParseSvgNumber(s, sweepFlag) && ParseSvgNumber(s, x) &&
                       ParseSvgNumber(s, y)) {
                    ImVec2 end = relative ? ImVec2(cur.x + x, cur.y + y) : ImVec2(x, y);
                    AppendSvgArc(current, cur, end, rx, ry, xRot, largeFlag != 0.0f, sweepFlag != 0.0f);
                    cur = end;
                    hasCtrl = false;
                }
                continue;
            }

            hasCtrl = false;
        }

        FinalizeSvgSubpath(out, current, false, stroke);
    }

    static void BuildSvgIconCache(const SvgIconSpec& spec, SvgIconCache& cache) {
        if (cache.built) return;
        for (int i = 0; i < spec.pathCount; ++i) {
            ParseSvgPathData(spec.paths[i].d, cache.subpaths, spec.paths[i].stroke);
        }
        cache.built = true;
    }

    static ImVec2 SvgTransformPoint(const ImVec2& p, const ImVec2& min, const ImVec2& max,
                                    float viewW, float viewH, float scaleFactor) {
        float size = std::min(max.x - min.x, max.y - min.y) * scaleFactor;
        ImVec2 center = ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        float scale = size / std::max(viewW, viewH);
        ImVec2 offset = ImVec2(center.x - (viewW * scale) * 0.5f, center.y - (viewH * scale) * 0.5f);
        return ImVec2(offset.x + p.x * scale, offset.y + p.y * scale);
    }

    static void DrawSvgIcon(ImDrawList* drawList, const SvgIconSpec& spec, SvgIconCache& cache,
                            const ImVec2& min, const ImVec2& max, ImU32 color, float strokeScale, float scaleFactor) {
        BuildSvgIconCache(spec, cache);
        for (const SvgSubpath& sub : cache.subpaths) {
            if (sub.points.size() < 2) continue;
            drawList->PathClear();
            for (const ImVec2& p : sub.points) {
                drawList->PathLineTo(SvgTransformPoint(p, min, max, spec.viewW, spec.viewH, scaleFactor));
            }
            ImDrawFlags flags = sub.closed ? ImDrawFlags_Closed : 0;
            drawList->PathStroke(color, flags, strokeScale);
        }
    }

    static const SvgPathSpec kEmptyObjectSvgPaths[] = {
        { "M 10.005859 0.5 A 0.50083746 0.50083746 0 0 0 9.7539062 0.56445312 L 1.7539062 5.0644531 A 0.50083746 0.50083746 0 0 0 1.5 5.5 L 1.5 14.5 A 0.50083746 0.50083746 0 0 0 1.7539062 14.935547 L 9.7539062 19.435547 A 0.50083746 0.50083746 0 0 0 10.246094 19.435547 L 18.246094 14.935547 A 0.50083746 0.50083746 0 0 0 18.5 14.5 L 18.5 5.5 A 0.50083746 0.50083746 0 0 0 18.246094 5.0644531 L 10.246094 0.56445312 A 0.50083746 0.50083746 0 0 0 10.005859 0.5 z M 10 1.5742188 L 16.978516 5.5 L 10 9.4257812 L 3.0214844 5.5 L 10 1.5742188 z M 2.5 6.3554688 L 9.5 10.292969 L 9.5 18.144531 L 2.5 14.207031 L 2.5 6.3554688 z M 17.5 6.3554688 L 17.5 14.207031 L 10.5 18.144531 L 10.5 10.292969 L 17.5 6.3554688 z", true }
    };

    static const SvgIconSpec kEmptyObjectSvg = { 20.0f, 20.0f, kEmptyObjectSvgPaths, 1 };
    static SvgIconCache gEmptyObjectSvgCache;

    void DrawEmptyObjectIcon(ImDrawList* drawList, ImVec2 pos, float size, ImU32 color) {
        ImVec2 min(pos.x, pos.y);
        ImVec2 max(pos.x + size, pos.y + size);
        float stroke = std::max(1.0f, size * 0.055f);
        DrawSvgIcon(drawList, kEmptyObjectSvg, gEmptyObjectSvgCache, min, max, color, stroke, 0.9f);
    }

    void DrawHierarchyLines(ImDrawList* drawList, const ImVec2& itemMin, const ImVec2& itemMax,
                            const std::vector<bool>& ancestorHasNext, int depth, bool isLast) {
        if (depth <= 0) {
            return;
        }
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImVec4 trunkCol(base.x, base.y, base.z, 0.34f);
        ImVec4 branchCol(
            std::min(1.0f, base.x + 0.10f),
            std::min(1.0f, base.y + 0.10f),
            std::min(1.0f, base.z + 0.10f),
            0.62f);
        ImU32 trunkColor = ImGui::GetColorU32(trunkCol);
        ImU32 lineColor = ImGui::GetColorU32(branchCol);
        float indent = style.IndentSpacing;
        float rowTop = itemMin.y;
        float rowBottom = itemMax.y;
        float rowMid = (rowTop + rowBottom) * 0.5f;
        float baseX = itemMin.x - indent * depth;
        float lineThickness = 1.35f;
        float cornerRadius = std::min(indent * 0.28f, std::max(3.5f, (rowBottom - rowTop) * 0.22f));
        float branchEndX = itemMin.x + 4.0f;

        for (int i = 0; i < depth && i < static_cast<int>(ancestorHasNext.size()); ++i) {
            if (ancestorHasNext[i]) {
                float x = baseX + indent * (i + 0.5f);
                drawList->AddLine(ImVec2(x, rowTop - 1.0f), ImVec2(x, rowBottom + 1.0f), trunkColor, 1.0f);
            }
        }

        float connectorX = baseX + indent * (depth - 0.5f);
        float branchStartY = rowMid - cornerRadius;
        float vertEnd = isLast ? branchStartY : rowBottom + 1.0f;
        drawList->AddLine(ImVec2(connectorX, rowTop - 1.0f), ImVec2(connectorX, vertEnd), lineColor, lineThickness);

        ImVec2 curveStart(connectorX, branchStartY);
        ImVec2 curveEnd(connectorX + cornerRadius, rowMid);
        ImVec2 curveCp1(connectorX, branchStartY + cornerRadius * 0.55f);
        ImVec2 curveCp2(connectorX + cornerRadius * 0.55f, rowMid);
        drawList->AddBezierCubic(curveStart, curveCp1, curveCp2, curveEnd, lineColor, lineThickness);
        drawList->AddLine(curveEnd, ImVec2(branchEndX, rowMid), lineColor, lineThickness);

        float capRadius = 1.5f;
        drawList->AddCircleFilled(ImVec2(branchEndX, rowMid), capRadius, lineColor, 10);
        drawList->AddCircleFilled(ImVec2(connectorX, rowTop + 1.0f), 1.1f, trunkColor, 8);
    }

    void appendHierarchyCacheRowRecursive(const SceneObject& obj,
                                          const std::string& filter,
                                          uint64_t ancestorHasNextMask,
                                          bool isLast,
                                          int depth,
                                          ImGuiStorage* storage,
                                          std::unordered_map<int, size_t>& sceneObjectIndexById,
                                          const std::vector<SceneObject>& sceneObjects,
                                          std::vector<HierarchyRowCacheEntry>& rows) {
        if (!HierarchyNameMatchesFilter(obj.name, filter)) {
            return;
        }

        HierarchyRowCacheEntry row;
        row.id = obj.id;
        row.depth = depth;
        row.parentId = obj.parentId;
        row.ancestorHasNextMask = ancestorHasNextMask;
        row.isLast = isLast;
        row.hasChildren = !obj.childIds.empty();
        row.iconColor = GetHierarchyTypeColor(obj);
        row.name = obj.name;
        if (storage && row.hasChildren) {
            row.expanded = storage->GetInt(HierarchyExpandStorageKey(obj.id), 0) != 0;
        }
        rows.push_back(std::move(row));

        if (!rows.back().expanded || obj.childIds.empty()) {
            return;
        }

        std::vector<const SceneObject*> visibleChildren;
        visibleChildren.reserve(obj.childIds.size());
        for (int childId : obj.childIds) {
            auto idxIt = sceneObjectIndexById.find(childId);
            if (idxIt == sceneObjectIndexById.end() || idxIt->second >= sceneObjects.size()) {
                continue;
            }
            const SceneObject& child = sceneObjects[idxIt->second];
            if (HierarchyNameMatchesFilter(child.name, filter)) {
                visibleChildren.push_back(&child);
            }
        }

        const uint64_t childMask = isLast
            ? ancestorHasNextMask
            : (ancestorHasNextMask | (1ull << depth));
        for (size_t i = 0; i < visibleChildren.size(); ++i) {
            appendHierarchyCacheRowRecursive(*visibleChildren[i],
                                            filter,
                                            childMask,
                                            i + 1 == visibleChildren.size(),
                                            depth + 1,
                                            storage,
                                            sceneObjectIndexById,
                                            sceneObjects,
                                            rows);
        }
    }

}

#pragma endregion

#pragma region Hierarchy Panel
void Engine::renderHierarchyPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    const bool panelVisible = ImGui::Begin(Loc::Window("WINDOW_HIERARCHY", "Hierarchy"), &showHierarchy);
    ImGui::PopStyleVar();
    if (!panelVisible) {
        Modu2DStats::CountWindowSkipped();
        ImGui::End();
        return;
    }

    static char searchBuffer[128] = "";
    float animSpeed = 0.0f;
    if (uiAnimationMode == UIAnimationMode::Fluid) {
        animSpeed = 8.0f;
    } else if (uiAnimationMode == UIAnimationMode::Snappy) {
        animSpeed = 18.0f;
    }
    float animStep = (uiAnimationMode == UIAnimationMode::Off) ? 1.0f
        : (1.0f - std::exp(-animSpeed * ImGui::GetIO().DeltaTime));

    const bool hierarchyPanelFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool hierarchyKeysActive = hierarchyPanelFocused && !ImGui::GetIO().WantTextInput;
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            gHierarchySuppressMouseHover = false;
        }
    }
    gHierarchyFrameCache.contextMenuKeyRequested =
        hierarchyKeysActive && selectedObjectId >= 0 &&
        (ImGui::IsKeyPressed(ImGuiKey_Menu, false) ||
         (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)));

    // No padding of its own: the add button and the search box sit flush against the panel's
    // own padding, and the strip is exactly one control tall, so nothing is indented or spaced
    // out behind them.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 0.0f));
    const float hierarchyHeaderHeight = ImGui::GetFrameHeight();
    ImGui::BeginChild("HierarchyHeader", ImVec2(-1.0f, hierarchyHeaderHeight), false, ImGuiWindowFlags_NoScrollbar);

    const float headerButtonSize = ImGui::GetFrameHeight();
    if (ImGui::Button("+", ImVec2(headerButtonSize, headerButtonSize))) {
        ImGui::OpenPopup("HierarchyCreatePopup");
    }
    if (hierarchyKeysActive && ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        ImGui::OpenPopup("HierarchyCreatePopup");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##Search", "Search...", searchBuffer, sizeof(searchBuffer));

    if (ImGui::BeginPopup("HierarchyCreatePopup")) {
        renderSceneObjectCreateMenu();
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);

    std::string filter = searchBuffer;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    auto importDroppedModel = [&](const fs::path& path, int parentId) {
        std::error_code ec;
        fs::directory_entry entry(path, ec);
        if (ec) {
            return;
        }
        if (!fileBrowser.isModelFile(entry)) {
            return;
        }
        size_t beforeCount = sceneObjects.size();
        if (fileBrowser.isOBJFile(entry)) {
            importOBJToScene(path.string(), "");
        } else {
            importModelToScene(path.string(), "");
        }
        if (sceneObjects.size() > beforeCount && parentId >= 0) {
            int newId = sceneObjects.back().id;
            setParent(newId, parentId);
        }
    };

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT")) {
            int draggedId = *(const int*)payload->Data;
            setParent(draggedId, -1);
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
            const char* path = static_cast<const char*>(payload->Data);
            importDroppedModel(fs::path(path), -1);
        }
        ImGui::EndDragDropTarget();
    }

    const bool hierarchyTouch =
        (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_IsTouchScreen) != 0;
    const ImVec2 listFramePadding = hierarchyTouch ? ImVec2(8.0f, 9.0f) : ImVec2(6.0f, 3.0f);
    const ImVec2 listItemSpacing = hierarchyTouch ? ImVec2(8.0f, 8.0f) : ImVec2(6.0f, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, listFramePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, listItemSpacing);
    ImGui::BeginChild("HierarchyList", ImVec2(-1.0f, 0.0f), false);

    gHierarchyFrameCache.navRows.clear();
    gHierarchyFrameCache.navRows.reserve(sceneObjects.size());
    gHierarchyFrameCache.selectedIds.clear();
    gHierarchyFrameCache.selectedIds.reserve(selectedObjectIds.size());
    for (int id : selectedObjectIds) {
        gHierarchyFrameCache.selectedIds.insert(id);
    }

    refreshSceneObjectIndexCache();
    std::vector<size_t> rootIndices;
    rootIndices.reserve(sceneObjects.size());
    for (size_t i = 0; i < sceneObjects.size(); i++) {
        int parentId = sceneObjects[i].parentId;
        if (parentId == -1 || sceneObjectIndexById.find(parentId) == sceneObjectIndexById.end()) {
            rootIndices.push_back(i);
        }
    }

    hierarchyVisibleOrder.clear();
    hierarchyVisibleOrder.reserve(sceneObjects.size());
    std::vector<bool> ancestorHasNext;
    std::unordered_set<int> renderPath;
    renderPath.reserve(sceneObjects.size());
    for (size_t i = 0; i < rootIndices.size(); ++i) {
        const bool isLastRoot = (i + 1 == rootIndices.size());
        renderObjectNode(sceneObjects[rootIndices[i]], filter, ancestorHasNext, renderPath, isLastRoot, 0, animStep);
    }

    {
        ImGuiWindow* listWindow = ImGui::GetCurrentWindow();
        listWindow->DC.CursorMaxPos.y = ImGui::GetCursorScreenPos().y;
        if (ImGui::GetScrollMaxY() <= 0.0f && ImGui::GetScrollY() > 0.0f) {
            ImGui::SetScrollY(0.0f);
        }
    }

    if (hierarchyKeysActive && !gHierarchyFrameCache.navRows.empty()) {
        auto& rows = gHierarchyFrameCache.navRows;
        const int rowCount = static_cast<int>(rows.size());
        int currentIndex = -1;
        for (int i = 0; i < rowCount; ++i) {
            if (rows[static_cast<size_t>(i)].id == selectedObjectId) {
                currentIndex = i;
                break;
            }
        }

        auto selectRow = [&](int index, bool extend) {
            index = std::clamp(index, 0, rowCount - 1);
            const int id = rows[static_cast<size_t>(index)].id;
            setPrimarySelection(id, extend);
            hierarchyRangeAnchorId = extend ? hierarchyRangeAnchorId : id;
            gHierarchyFrameCache.scrollToObjectId = id;
            gHierarchySuppressMouseHover = true;
        };

        auto navKeyPressed = [](ImGuiKey key) {
            return ImGui::GetKeyPressedAmount(key, 0.32f, 0.09f) > 0;
        };

        const bool shift = ImGui::GetIO().KeyShift;
        if (navKeyPressed(ImGuiKey_DownArrow)) {
            selectRow(currentIndex < 0 ? 0 : currentIndex + 1, shift);
        } else if (navKeyPressed(ImGuiKey_UpArrow)) {
            selectRow(currentIndex < 0 ? 0 : currentIndex - 1, shift);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            selectRow(0, shift);
        } else if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            selectRow(rowCount - 1, shift);
        } else if (currentIndex >= 0) {
            const HierarchyNavRow row = rows[static_cast<size_t>(currentIndex)];
            if (navKeyPressed(ImGuiKey_RightArrow)) {
                if (row.hasChildren && !row.open) {
                    ImGui::GetStateStorage()->SetInt(row.nodeId, 1);
                } else if (row.hasChildren && currentIndex + 1 < rowCount &&
                           rows[static_cast<size_t>(currentIndex + 1)].parentId == row.id) {
                    selectRow(currentIndex + 1, false);
                }
            } else if (navKeyPressed(ImGuiKey_LeftArrow)) {
                if (row.hasChildren && row.open) {
                    ImGui::GetStateStorage()->SetInt(row.nodeId, 0);
                } else if (row.parentId != -1) {
                    for (int i = 0; i < rowCount; ++i) {
                        if (rows[static_cast<size_t>(i)].id == row.parentId) {
                            selectRow(i, false);
                            break;
                        }
                    }
                }
            } else if (ImGui::IsKeyPressed(ImGuiKey_F, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                focusViewportOnSelection();
            } else if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
                gHierarchyRenameRequestId = selectedObjectId;
            }
        }
    }

    const bool hierarchyBackgroundLeftClicked =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered();
    if (hierarchyBackgroundLeftClicked) {
        clearSelection();
    }

    if (ImGui::BeginPopupContextWindow("HierarchyBackground",
            ImGuiPopupFlags_MouseButtonRight |
            ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::BeginMenu("Create"))
        {
            renderSceneObjectCreateMenu();
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);

    static bool showHierarchyRenamePopup = false;
    static int hierarchyRenameTargetId = -1;
    static char hierarchyRenameBuffer[256] = "";
    if (gHierarchyRenameRequestId >= 0) {
        if (SceneObject* target = findObjectById(gHierarchyRenameRequestId)) {
            hierarchyRenameTargetId = gHierarchyRenameRequestId;
            strncpy(hierarchyRenameBuffer, target->name.c_str(), sizeof(hierarchyRenameBuffer) - 1);
            hierarchyRenameBuffer[sizeof(hierarchyRenameBuffer) - 1] = '\0';
            showHierarchyRenamePopup = true;
            ImGui::OpenPopup("Rename SceneOBJ");
        }
        gHierarchyRenameRequestId = -1;
    }
    if (beginCardModal("Rename SceneOBJ", 0.0f, &showHierarchyRenamePopup)) {
        cardModalText("Choose a new name for this SceneOBJ.");
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool renameSubmitted = ImGui::InputText("##HierarchyRenameName", hierarchyRenameBuffer,
                                                      sizeof(hierarchyRenameBuffer),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        const bool canRename = hierarchyRenameBuffer[0] != '\0';
        if (cardModalButton("Cancel", CardButtonKind::Neutral, 0, 2)) {
            showHierarchyRenamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::BeginDisabled(!canRename);
        if (cardModalButton("Rename", CardButtonKind::Primary, 1, 2) || (renameSubmitted && canRename)) {
            if (SceneObject* target = findObjectById(hierarchyRenameTargetId)) {
                if (target->name != hierarchyRenameBuffer) {
                    recordState("Rename SceneOBJ");
                    target->name = hierarchyRenameBuffer;
                    projectManager.currentProject.hasUnsavedChanges = true;
                }
            }
            showHierarchyRenamePopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        endCardModal();
    }

    ImGui::End();
}

void Engine::renderObjectNode(SceneObject& obj, const std::string& filter,
                              std::vector<bool>& ancestorHasNext, std::unordered_set<int>& renderPath,
                              bool isLast, int depth, float animStep) {
    if (!HierarchyNameMatchesFilter(obj.name, filter)) {
        return;
    }
    if (depth > 256 || !renderPath.insert(obj.id).second) {
        return;
    }
    // Held separately because a context menu further down can mutate sceneObjects and leave
    // `obj` dangling before the cleanup at the end of this function runs.
    const int objectId = obj.id;

    hierarchyVisibleOrder.push_back(obj.id);

    bool hasChildren = !obj.childIds.empty();
    bool isSelected = gHierarchyFrameCache.selectedIds.find(obj.id) != gHierarchyFrameCache.selectedIds.end();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;

    ImGuiID nodeId = ImGui::GetID((void*)(intptr_t)obj.id);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    bool nodeOpen = ImGui::TreeNodeEx((void*)(intptr_t)obj.id, flags, "%s", "");
    ImGui::PopItemFlag();

    if (gHierarchyNavCaptureSuppress == 0) {
        HierarchyNavRow navRow;
        navRow.id = obj.id;
        navRow.parentId = obj.parentId;
        navRow.nodeId = nodeId;
        navRow.open = nodeOpen;
        navRow.hasChildren = hasChildren;
        gHierarchyFrameCache.navRows.push_back(navRow);
    }
    if (gHierarchyFrameCache.scrollToObjectId == obj.id) {
        gHierarchyFrameCache.scrollToObjectId = -1;
        const ImRect viewRect = ImGui::GetCurrentWindow()->InnerRect;
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        if (rowMin.y < viewRect.Min.y) {
            ImGui::SetScrollHereY(0.0f);
        } else if (rowMax.y > viewRect.Max.y) {
            ImGui::SetScrollHereY(1.0f);
        }
    }

    ImVec2 itemMin = ImGui::GetItemRectMin();
    ImVec2 itemMax = ImGui::GetItemRectMax();
    UIAnimationState& animState = editorUiAnimationStates[nodeId];
    bool hovered = ImGui::IsItemHovered() && !gHierarchySuppressMouseHover;
    bool active = ImGui::IsItemActive();
    float openTarget = nodeOpen ? 1.0f : 0.0f;
    if (uiAnimationMode == UIAnimationMode::Off) {
        animState.hover = hovered ? 1.0f : 0.0f;
        animState.active = active ? 1.0f : 0.0f;
        animState.sliderValue = openTarget;
    } else {
        float hoverTarget = hovered ? 1.0f : 0.0f;
        float activeTarget = active ? 1.0f : 0.0f;
        animState.hover += (hoverTarget - animState.hover) * animStep;
        animState.active += (activeTarget - animState.active) * animStep;
        animState.sliderValue += (openTarget - animState.sliderValue) * animStep;
    }
    float hoverT = std::clamp(animState.hover, 0.0f, 1.0f);
    float activeT = std::clamp(animState.active, 0.0f, 1.0f);
    float openT = std::clamp(animState.sliderValue, 0.0f, 1.0f);
    float glow = std::min(1.0f, hoverT * 0.7f + activeT * 1.0f);
    if (glow > 0.001f) {
        ImVec2 pad(4.0f + 10.0f * hoverT + 14.0f * activeT, 2.0f + 6.0f * hoverT);
        ImVec2 glowMin(itemMin.x - pad.x, itemMin.y - pad.y);
        ImVec2 glowMax(itemMax.x + pad.x, itemMax.y + pad.y);
        ImVec4 glowCol = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        glowCol.w *= 0.35f * glow;
        ImGui::GetWindowDrawList()->AddRectFilled(glowMin, glowMax, ImGui::GetColorU32(glowCol), 6.0f);
    }
    DrawHierarchyLines(ImGui::GetWindowDrawList(), itemMin, itemMax, ancestorHasNext, depth, isLast);

    float lineHeight = itemMax.y - itemMin.y;
    float iconScale = 1.0f + 0.08f * hoverT + 0.12f * activeT;
    float iconSize = std::max(8.0f, (lineHeight - 6.0f) * iconScale);
    float labelStart = itemMin.x + ImGui::GetTreeNodeToLabelSpacing();
    ImVec2 iconPos(labelStart, itemMin.y + (lineHeight - iconSize) * 0.5f);
    const float rowDim = IsObjectEnabledInHierarchy(obj) ? 1.0f : 0.42f;
    ImU32 iconColor = GetHierarchyTypeColor(obj);
    if (rowDim < 1.0f) {
        ImVec4 dimmed = ImGui::ColorConvertU32ToFloat4(iconColor);
        dimmed.x *= rowDim;
        dimmed.y *= rowDim;
        dimmed.z *= rowDim;
        iconColor = ImGui::ColorConvertFloat4ToU32(dimmed);
    }
    Texture* componentIcon = obj.editorIconPath.empty()
        ? nullptr
        : renderer.getTexture(obj.editorIconPath,
                              MaterialProperties::TextureFilter::Bilinear);
    if (!componentIcon || !componentIcon->GetID()) {
        componentIcon = renderer.getTexture(SceneObjectComponentIconPath(obj),
                                            MaterialProperties::TextureFilter::Bilinear);
    }
    ImU32 iconTintU32 = ImGui::ColorConvertFloat4ToU32(
        ImVec4(obj.editorIconTint.r * rowDim,
               obj.editorIconTint.g * rowDim,
               obj.editorIconTint.b * rowDim,
               obj.editorIconTint.a));
    if (componentIcon && componentIcon->GetID()) {
        float texIconSize = std::max(8.0f, (lineHeight - 2.0f) * iconScale);
        ImVec2 texIconPos(labelStart, itemMin.y + (lineHeight - texIconSize) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)(intptr_t)componentIcon->GetID(),
            texIconPos, ImVec2(texIconPos.x + texIconSize, texIconPos.y + texIconSize),
            ImVec2(0, 1), ImVec2(1, 0), iconTintU32);
    } else {
        DrawEmptyObjectIcon(ImGui::GetWindowDrawList(), iconPos, iconSize, iconColor);
    }
    ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    textCol.x = std::min(1.0f, textCol.x + 0.15f * hoverT + 0.2f * activeT);
    textCol.y = std::min(1.0f, textCol.y + 0.15f * hoverT + 0.2f * activeT);
    textCol.z = std::min(1.0f, textCol.z + 0.15f * hoverT + 0.2f * activeT);
    textCol.w = std::min(1.0f, textCol.w + 0.2f * hoverT + 0.35f * activeT);
    const HierarchyMissingAssetState& missingAssets = GetHierarchyMissingAssetState(obj);
    if (!missingAssets.firstMissing.empty()) {
        textCol.x = std::min(1.0f, 0.95f + 0.05f * hoverT);
        textCol.y = std::min(1.0f, 0.36f + 0.15f * hoverT);
        textCol.z = std::min(1.0f, 0.36f + 0.15f * hoverT);
        if (hovered && ImGui::BeginTooltip()) {
            const ImVec4 missingTitleColor(0.95f, 0.45f, 0.45f, 1.0f);
            if (missingAssets.missingCount == 1) {
                ImGui::TextColored(missingTitleColor, "Missing asset");
            } else {
                ImGui::TextColored(missingTitleColor, "Missing assets (%d)", missingAssets.missingCount);
            }
            ImGui::TextUnformatted(missingAssets.firstMissing.c_str());
            ImGui::TextDisabled("Export leaves broken references out and reports them.");
            ImGui::EndTooltip();
        }
    }
    if (rowDim < 1.0f) {
        textCol.x *= rowDim;
        textCol.y *= rowDim;
        textCol.z *= rowDim;
        textCol.w *= 0.75f;
    }
    float fontSize = ImGui::GetFontSize() * (1.0f + 0.06f * hoverT + 0.1f * activeT);
    ImVec2 textPos(iconPos.x + iconSize + 6.0f, itemMin.y + (lineHeight - fontSize) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), fontSize, textPos,
                                        ImGui::GetColorU32(textCol), obj.name.c_str());

    if (obj.hasParallaxLayer2D && obj.parallaxLayer2D.enabled) {
        std::string badgeText = "L" + std::to_string(obj.parallaxLayer2D.order);
        ImVec2 badgeTextSize = ImGui::CalcTextSize(badgeText.c_str());
        ImVec2 labelTextSize = ImGui::CalcTextSize(obj.name.c_str());
        float badgePadX = 6.0f;
        float badgePadY = 2.0f;
        float rightEdge = itemMax.x - 8.0f;
        if (hierarchyShowTexturePreview) {
            float previewSize = std::max(12.0f, lineHeight - 4.0f);
            rightEdge -= (previewSize + 8.0f);
        }
        ImVec2 badgeMin(rightEdge - badgeTextSize.x - badgePadX * 2.0f,
                        itemMin.y + (lineHeight - (badgeTextSize.y + badgePadY * 2.0f)) * 0.5f);
        ImVec2 badgeMax(rightEdge, badgeMin.y + badgeTextSize.y + badgePadY * 2.0f);
        float minAllowedX = textPos.x + labelTextSize.x + 8.0f;
        if (badgeMin.x > minAllowedX) {
            ImU32 badgeBg = ImGui::GetColorU32(ImVec4(0.20f, 0.40f, 0.62f, 0.78f));
            ImU32 badgeBorder = ImGui::GetColorU32(ImVec4(0.52f, 0.74f, 0.96f, 0.88f));
            ImU32 badgeFg = ImGui::GetColorU32(ImVec4(0.92f, 0.96f, 1.0f, 0.96f));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(badgeMin, badgeMax, badgeBg, 4.0f);
            dl->AddRect(badgeMin, badgeMax, badgeBorder, 4.0f, 0, 1.0f);
            dl->AddText(ImVec2(badgeMin.x + badgePadX, badgeMin.y + badgePadY), badgeFg, badgeText.c_str());
        }
    }

    const ImVec2 leftDragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    const float leftDragDistanceSq = leftDragDelta.x * leftDragDelta.x + leftDragDelta.y * leftDragDelta.y;
    const float leftDragThreshold = ImGui::GetIO().MouseDragThreshold;
    const bool hierarchyRowReleasedAsClick =
        ImGui::IsItemHovered()
        && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
        && leftDragDistanceSq <= leftDragThreshold * leftDragThreshold
        && !ImGui::IsItemToggledOpen();
    const bool hierarchyRowDoubleClicked =
        ImGui::IsItemHovered()
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && !ImGui::IsItemToggledOpen();

    if (hierarchyRowReleasedAsClick) {
        const bool shift = ImGui::GetIO().KeyShift;
        const bool ctrl = ImGui::GetIO().KeyCtrl;

        if (shift && !hierarchyVisibleOrder.empty()) {
            int anchorId = (hierarchyRangeAnchorId >= 0) ? hierarchyRangeAnchorId : selectedObjectId;
            if (anchorId < 0) {
                anchorId = obj.id;
            }

            // Only needed on shift-click, so the rows drawn so far are searched here instead of
            // every row paying a hash-map insert on every frame.
            auto findOrderIndex = [&](int id) -> int {
                auto it = std::find(hierarchyVisibleOrder.begin(), hierarchyVisibleOrder.end(), id);
                if (it == hierarchyVisibleOrder.end()) return -1;
                return static_cast<int>(it - hierarchyVisibleOrder.begin());
            };

            const int anchorIndex = findOrderIndex(anchorId);
            const int currentIndex = findOrderIndex(obj.id);
            if (anchorIndex >= 0 && currentIndex >= 0) {
                if (!ctrl) {
                    selectedObjectIds.clear();
                }
                int rangeStart = std::min(anchorIndex, currentIndex);
                int rangeEnd = std::max(anchorIndex, currentIndex);
                for (int i = rangeStart; i <= rangeEnd; ++i) {
                    int rangeId = hierarchyVisibleOrder[static_cast<size_t>(i)];
                    if (std::find(selectedObjectIds.begin(), selectedObjectIds.end(), rangeId) == selectedObjectIds.end()) {
                        selectedObjectIds.push_back(rangeId);
                    }
                }
                selectedObjectId = obj.id;
            } else {
                setPrimarySelection(obj.id, ctrl);
            }
        } else if (ctrl) {
            auto selectedIt = std::find(selectedObjectIds.begin(), selectedObjectIds.end(), obj.id);
            if (selectedIt == selectedObjectIds.end()) {
                selectedObjectIds.push_back(obj.id);
                selectedObjectId = obj.id;
            } else {
                selectedObjectIds.erase(selectedIt);
                if (selectedObjectId == obj.id) {
                    selectedObjectId = selectedObjectIds.empty() ? -1 : selectedObjectIds.back();
                }
            }
            hierarchyRangeAnchorId = obj.id;
        } else {
            setPrimarySelection(obj.id, false);
        }
    }

    if (hierarchyRowDoubleClicked) {
        setPrimarySelection(obj.id, false);
        focusViewportOnSelection();
    }

    if (DragPreview::BeginSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload("SCENE_OBJECT", &obj.id, sizeof(int));
        Texture* dragIconTex = obj.editorIconPath.empty()
            ? nullptr
            : renderer.getTexture(obj.editorIconPath, MaterialProperties::TextureFilter::Bilinear);
        if (!dragIconTex || !dragIconTex->GetID()) {
            dragIconTex = renderer.getTexture(SceneObjectComponentIconPath(obj),
                                              MaterialProperties::TextureFilter::Bilinear);
        }
        ImTextureID dragIcon = (dragIconTex && dragIconTex->GetID())
            ? (ImTextureID)(intptr_t)dragIconTex->GetID()
            : (ImTextureID)0;
        DragPreview::SubmitMeta(obj.name.c_str(), dragIcon, "SCENE_OBJECT");
        DragPreview::EndSource();
    }

    auto importDroppedModel = [&](const fs::path& path, int parentId) {
        std::error_code ec;
        fs::directory_entry entry(path, ec);
        if (ec || !fileBrowser.isModelFile(entry)) {
            return;
        }
        size_t beforeCount = sceneObjects.size();
        if (fileBrowser.isOBJFile(entry)) {
            importOBJToScene(path.string(), "");
        } else {
            importModelToScene(path.string(), "");
        }
        if (sceneObjects.size() > beforeCount && parentId >= 0) {
            int newId = sceneObjects.back().id;
            setParent(newId, parentId);
        }
    };

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT")) {
            int draggedId = *(const int*)payload->Data;
            if (draggedId != obj.id) {
                auto nextSiblingId = [&]() -> int {
                    if (obj.parentId != -1) {
                        SceneObject* parent = findObjectById(obj.parentId);
                        if (!parent) return -1;
                        auto it = std::find(parent->childIds.begin(), parent->childIds.end(), obj.id);
                        if (it == parent->childIds.end()) return -1;
                        ++it;
                        while (it != parent->childIds.end()) {
                            if (*it != draggedId) return *it;
                            ++it;
                        }
                        return -1;
                    }

                    bool found = false;
                    for (const auto& candidate : sceneObjects) {
                        if (candidate.parentId != -1) continue;
                        if (candidate.id == draggedId) continue;
                        if (found) return candidate.id;
                        if (candidate.id == obj.id) found = true;
                    }
                    return -1;
                };

                float mouseY = ImGui::GetMousePos().y;
                float upperThreshold = itemMin.y + lineHeight * 0.25f;
                float lowerThreshold = itemMax.y - lineHeight * 0.25f;
                if (mouseY <= upperThreshold) {
                    setParent(draggedId, obj.parentId, obj.id);
                } else if (mouseY >= lowerThreshold) {
                    setParent(draggedId, obj.parentId, nextSiblingId());
                } else {
                    setParent(draggedId, obj.id, -1);
                }
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_PATH")) {
            const char* path = static_cast<const char*>(payload->Data);
            std::error_code ec;
            fs::directory_entry entry(path, ec);
            if (!ec) {
                if (fileBrowser.isModelFile(entry)) {
                    importDroppedModel(fs::path(path), obj.id);
                } else if (fileBrowser.getFileCategory(entry) == FileCategory::Texture) {
                    applyTextureAssetToObject(obj, entry.path());
                } else if (fileBrowser.getFileCategory(entry) == FileCategory::Material) {
                    obj.materialPath = entry.path().string();
                    loadMaterialFromFile(obj);
                    projectManager.currentProject.hasUnsavedChanges = true;
                    addConsoleMessage("Applied material to " + obj.name, ConsoleMessageType::Success);
                } else if (fileBrowser.getFileCategory(entry) == FileCategory::Script) {
                    auto alreadyAssigned = std::any_of(obj.scripts.begin(), obj.scripts.end(),
                        [&](const ScriptComponent& sc) { return sc.path == path; });
                    if (!alreadyAssigned) {
                        ScriptComponent sc;
                        sc.path = path;
                        sc.lastBinaryPath.clear();
                        sc.lastBinaryVerified = false;
                        std::string ext = fs::path(path).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (ext == ".cs") {
                            sc.language = ScriptLanguage::CSharp;
                            sc.managedType = fs::path(path).stem().string();
                        } else if (ext == ".c") {
                            sc.language = ScriptLanguage::C;
                        } else {
                            sc.language = ScriptLanguage::Cpp;
                        }
                        obj.scripts.push_back(sc);
                        markRuntimeScriptBindingsDirty();
                        projectManager.currentProject.hasUnsavedChanges = true;
                        playEditorFeedbackPreview("Resources/Sounds/Drag Script Assign Check Successful.mp3", 0.95f, false, EditorFeedbackSoundCategory::Other);
                        const std::string targetName = obj.name.empty() ? "Object" : obj.name;
                        showEditorToast("Script assigned to " + targetName + " Successful.",
                                        ConsoleMessageType::Success,
                                        1.65);
                        addConsoleMessage("Assigned script to " + obj.name, ConsoleMessageType::Success);
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (gHierarchyFrameCache.contextMenuKeyRequested && obj.id == selectedObjectId) {
        ImGui::OpenPopup(nodeId);
        gHierarchyFrameCache.contextMenuKeyRequested = false;
    }

    // Menu items here can add or remove SceneOBJs, which reallocates `sceneObjects` and leaves
    // `obj` dangling for everything below. Work from the id instead of the reference, and bail
    // out of this node if the vector moved.
    const int contextObjectId = objectId;
    const int contextObjectParentId = obj.parentId;
    const SceneObject* sceneDataBeforeMenu = sceneObjects.data();
    const size_t sceneCountBeforeMenu = sceneObjects.size();

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Rename", "F2")) {
            setPrimarySelection(contextObjectId);
            gHierarchyRenameRequestId = contextObjectId;
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            setPrimarySelection(contextObjectId);
            duplicateSelected();
        }
        // Creates the prefab asset the engine actually instantiates and spawns.
        // "Export As ModuOBJ..." below writes the other .moduobj - a zip package with
        // bundled dependencies, for sending to someone else.
        if (ImGui::MenuItem("Create Packaged Object")) {
            if (std::find(selectedObjectIds.begin(), selectedObjectIds.end(), contextObjectId) == selectedObjectIds.end()) {
                setPrimarySelection(contextObjectId);
            }
            createModuObjAssetFromSelection();
        }
        if (ImGui::MenuItem("Export As ModuOBJ...")) {
            if (std::find(selectedObjectIds.begin(), selectedObjectIds.end(), contextObjectId) == selectedObjectIds.end()) {
                setPrimarySelection(contextObjectId);
            }
            openModuObjExportDialog();
        }
        if (ImGui::MenuItem("Delete", "Del")) {
            setPrimarySelection(contextObjectId);
            deleteSelected();
        }
        if (ImGui::MenuItem("Frame in Viewport", "F")) {
            setPrimarySelection(contextObjectId);
            focusViewportOnSelection();
        }
        ImGui::Separator();
        // The full create menu, parented to the object that was right-clicked. This replaces
        // the Canvas-only "Create UI Child" submenu - a UI element dropped on a Canvas still
        // lands under it, and every other SceneOBJ type can now be created as a child too.
        if (ImGui::BeginMenu("Create Child")) {
            renderSceneObjectCreateMenu(contextObjectId);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Clear Parent") && contextObjectParentId != -1) {
            setParent(contextObjectId, -1);
        }
        ImGui::EndPopup();
    }

    // Reparenting to the root reorders the vector in place, so also check that this slot still
    // holds the same object. `obj` is only read once the buffer is known not to have moved.
    const bool sceneVectorMoved = (sceneObjects.data() != sceneDataBeforeMenu ||
                                   sceneObjects.size() != sceneCountBeforeMenu);
    if (sceneVectorMoved || obj.id != contextObjectId) {
        // The menu added, removed or moved SceneOBJs. `obj` may now point at a freed or
        // shifted element, so stop drawing this node and let the next frame redraw the tree.
        if (nodeOpen) {
            ImGui::TreePop();
        }
        renderPath.erase(contextObjectId);
        return;
    }

    if (hierarchyShowTexturePreview) {
        const std::string* previewPath = nullptr;
        if (!obj.albedoTexturePath.empty()) {
            previewPath = &obj.albedoTexturePath;
        } else if (obj.useOverlay && !obj.overlayTexturePath.empty()) {
            previewPath = &obj.overlayTexturePath;
        }

        if (previewPath) {
            Texture* previewTex = renderer.getTexture(*previewPath, obj.material.textureFilter);
            if (previewTex && previewTex->GetID()) {
                ImGuiStyle& style = ImGui::GetStyle();
                ImVec2 itemMin = ImGui::GetItemRectMin();
                ImVec2 itemMax = ImGui::GetItemRectMax();
                float lineHeight = itemMax.y - itemMin.y;
                float previewSize = std::max(12.0f, lineHeight - 4.0f);
                float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                float previewX = rightEdge - previewSize - style.WindowPadding.x;
                float previewY = itemMin.y + (lineHeight - previewSize) * 0.5f;
                ImVec2 pMin(previewX, previewY);
                ImVec2 pMax(previewX + previewSize, previewY + previewSize);
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)(intptr_t)previewTex->GetID(),
                    pMin, pMax, ImVec2(0, 1), ImVec2(1, 0));
            }
        }
    }

    size_t visibleChildCount = 0;
    if (hasChildren) {
        for (int childId : obj.childIds) {
            auto idxIt = sceneObjectIndexById.find(childId);
            if (idxIt == sceneObjectIndexById.end()) continue;
            size_t idx = idxIt->second;
            if (idx >= sceneObjects.size()) continue;
            SceneObject& child = sceneObjects[idx];
            if (HierarchyNameMatchesFilter(child.name, filter)) {
                ++visibleChildCount;
            }
        }
    }

    const bool shouldAnimateChildren = visibleChildCount > 0 && (nodeOpen || openT > 0.001f);
    if (shouldAnimateChildren) {
        const float revealT = std::clamp(openT, 0.0f, 1.0f);
        const float revealHeightT = std::clamp((revealT - 0.045f) / 0.955f, 0.0f, 1.0f);
        const float revealAlphaT = std::clamp((revealT - 0.11f) / 0.89f, 0.0f, 1.0f);
        const float easedReveal = revealHeightT * revealHeightT * (3.0f - 2.0f * revealHeightT);
        const float easedAlpha = revealAlphaT * revealAlphaT * (3.0f - 2.0f * revealAlphaT);
        const bool treePushed = nodeOpen;
        if (!treePushed) {
            ImGui::Indent();
        }

        const ImVec2 layoutCursor = ImGui::GetCursorPos();
        const ImVec2 renderCursor = ImGui::GetCursorScreenPos();
        const float cursorMaxYBefore = ImGui::GetCurrentWindow()->DC.CursorMaxPos.y;
        const float estimatedHeight = std::max(lineHeight * 0.9f,
                                               lineHeight * static_cast<float>(visibleChildCount) * 1.1f);
        const float cachedHeight = std::max(animState.contentExtent, estimatedHeight);
        const float reservedHeight = cachedHeight * easedReveal;
        const float slideOffset = (1.0f - easedReveal) * std::min(std::max(lineHeight * 0.9f, cachedHeight * 0.18f),
                                                                  lineHeight * 3.0f);
        const ImVec2 clipMin(renderCursor.x - 24.0f, itemMax.y - 1.0f);
        const ImVec2 clipMax(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x + 24.0f,
                             renderCursor.y + reservedHeight + lineHeight);

        if (reservedHeight > 0.75f && easedAlpha > 0.001f) {
            ImGui::PushClipRect(clipMin, clipMax, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, easedAlpha);
            ImGui::SetCursorScreenPos(ImVec2(renderCursor.x, renderCursor.y - slideOffset));
            ImGui::BeginGroup();

            ancestorHasNext.push_back(!isLast);
            if (!treePushed) ++gHierarchyNavCaptureSuppress;
            // A descendant's context menu can create or delete SceneOBJs mid-walk, which
            // reallocates sceneObjects and would leave `obj` (and this loop's iterator over
            // its childIds) dangling. Walk a copy of the ids and stop as soon as the vector
            // moves; the next frame redraws the new tree.
            const std::vector<int> childIdsSnapshot = obj.childIds;
            const SceneObject* sceneDataBeforeChildren = sceneObjects.data();
            const size_t sceneCountBeforeChildren = sceneObjects.size();
            size_t visibleChildIndex = 0;
            for (int childId : childIdsSnapshot) {
                auto idxIt = sceneObjectIndexById.find(childId);
                if (idxIt == sceneObjectIndexById.end()) continue;
                size_t idx = idxIt->second;
                if (idx >= sceneObjects.size()) continue;
                SceneObject& child = sceneObjects[idx];
                if (child.id != childId) continue;
                if (!HierarchyNameMatchesFilter(child.name, filter)) continue;
                const bool childLast = (visibleChildIndex + 1 == visibleChildCount);
                ++visibleChildIndex;
                renderObjectNode(child, filter, ancestorHasNext, renderPath, childLast, depth + 1, animStep);
                if (sceneObjects.data() != sceneDataBeforeChildren ||
                    sceneObjects.size() != sceneCountBeforeChildren) {
                    break;
                }
            }
            if (!treePushed) --gHierarchyNavCaptureSuppress;
            ancestorHasNext.pop_back();

            ImGui::EndGroup();
            const ImVec2 renderedMin = ImGui::GetItemRectMin();
            const ImVec2 renderedMax = ImGui::GetItemRectMax();
            const float renderedHeight = std::max(0.0f, renderedMax.y - renderedMin.y);
            if (renderedHeight > 0.0f) {
                if (uiAnimationMode == UIAnimationMode::Off || animState.contentExtent <= 0.0f) {
                    animState.contentExtent = renderedHeight;
                } else {
                    animState.contentExtent += (renderedHeight - animState.contentExtent) * std::min(1.0f, animStep * 0.85f);
                }
            }
            ImGui::PopStyleVar();
            ImGui::PopClipRect();
        }

        ImGui::SetCursorPos(layoutCursor);
        ImGui::Dummy(ImVec2(0.0f, reservedHeight));

        ImGuiWindow* currentWindow = ImGui::GetCurrentWindow();
        currentWindow->DC.CursorMaxPos.y = ImMax(cursorMaxYBefore,
                                                 renderCursor.y + reservedHeight);

        if (treePushed) {
            ImGui::TreePop();
        } else {
            ImGui::Unindent();
        }
    } else if (nodeOpen) {
        ImGui::TreePop();
    }
    renderPath.erase(objectId);
}

#pragma endregion

#pragma region Console Panel
void Engine::renderConsolePanel() {
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (!mainViewport) return;
    const EditorChromeMetrics& chrome = getEditorChromeMetrics(uiChromeScale);
    const bool floatingWindow = projectManager.currentProject.isLoaded &&
        projectManager.currentProject.consoleSettings.mode == ProjectConsoleMode::FloatingWindow;

    ImVec2 anchorMin = mainViewport->WorkPos;
    ImVec2 anchorMax = ImVec2(mainViewport->WorkPos.x + mainViewport->WorkSize.x,
                              mainViewport->WorkPos.y + mainViewport->WorkSize.y);
    if (ImGuiWindow* viewportWindow = ImGui::FindWindowByName(Loc::WindowRef("Viewport"))) {
        if (viewportWindow->WasActive) {
            anchorMin = viewportWindow->InnerRect.Min;
            anchorMax = viewportWindow->InnerRect.Max;
        }
    }

    static bool autoScroll = true;
    static float consolePopoutAnim = 0.0f;

    const float margin = chrome.consoleMargin;
    const ImVec2 tabSize = chrome.consoleTabSize;
    ImVec2 tabPos(anchorMax.x - tabSize.x - margin, anchorMax.y - tabSize.y - margin);
    tabPos.x = ImMax(anchorMin.x + 4.0f, tabPos.x);
    tabPos.y = ImMax(anchorMin.y + 4.0f, tabPos.y);

    if (!floatingWindow) {
        ImGuiWindowFlags tabFlags = ImGuiWindowFlags_NoDecoration |
                                    ImGuiWindowFlags_NoDocking |
                                    ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::SetNextWindowPos(tabPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(tabSize, ImGuiCond_Always);
        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        if (ImGui::Begin("ConsoleTab##ViewportMini", nullptr, tabFlags)) {
            const bool tabActive = consolePanelExpanded || consolePopoutAnim > 0.02f;
            if (tabActive) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button("Console", ImGui::GetContentRegionAvail())) {
                consolePanelExpanded = !consolePanelExpanded;
            }
            if (tabActive) {
                ImGui::PopStyleColor(3);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    const float targetAnim = (floatingWindow || consolePanelExpanded) ? 1.0f : 0.0f;
    const float blend = 1.0f - std::exp(-12.0f * ImGui::GetIO().DeltaTime);
    consolePopoutAnim += (targetAnim - consolePopoutAnim) * blend;
    if (std::fabs(consolePopoutAnim - targetAnim) < 0.001f) {
        consolePopoutAnim = targetAnim;
    }
    const float openAmount = std::clamp(consolePopoutAnim, 0.0f, 1.0f);
    auto withScaledAlpha = [&](ImVec4 color) -> ImVec4 {
        color.w = std::clamp(color.w * openAmount, 0.0f, 1.0f);
        return color;
    };
    auto withScaledAlphaU32 = [&](ImU32 color) -> ImU32 {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
        c.w = std::clamp(c.w * openAmount, 0.0f, 1.0f);
        return ImGui::ColorConvertFloat4ToU32(c);
    };

    if (!floatingWindow && !consolePanelExpanded && openAmount <= 0.001f) {
        return;
    }

    ImVec2 miniSize(ImMin(chrome.consoleMiniSize.x, anchorMax.x - anchorMin.x - margin * 2.0f),
                    ImMin(chrome.consoleMiniSize.y, anchorMax.y - anchorMin.y - tabSize.y - margin * 3.0f));
    if (!floatingWindow && (miniSize.x < 260.0f || miniSize.y < 140.0f)) {
        return;
    }

    ImVec2 miniPos(anchorMax.x - miniSize.x - margin, tabPos.y - miniSize.y - 8.0f);
    miniPos.y += (1.0f - openAmount) * 22.0f;
    miniPos.y = ImMax(anchorMin.y + margin, miniPos.y);

    bool keepOpen = consolePanelExpanded;
    bool* openPtr = floatingWindow ? &showConsole : (consolePanelExpanded ? &keepOpen : nullptr);
    ImGuiWindowFlags miniFlags = ImGuiWindowFlags_NoCollapse;
    if (!floatingWindow) {
        miniFlags |= ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoResize;
        if (openAmount < 0.98f) {
            miniFlags |= ImGuiWindowFlags_NoInputs;
        }
        ImGui::SetNextWindowPos(miniPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(miniSize, ImGuiCond_Always);
        ImGui::SetNextWindowViewport(mainViewport->ID);
    } else {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 320.0f), ImGuiCond_FirstUseEver);
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    if (!floatingWindow) {
        ImGui::SetNextWindowBgAlpha(style.Colors[ImGuiCol_WindowBg].w * openAmount);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, openAmount);
        ImGui::PushStyleColor(ImGuiCol_Border, withScaledAlpha(style.Colors[ImGuiCol_Border]));
        ImGui::PushStyleColor(ImGuiCol_BorderShadow, withScaledAlpha(style.Colors[ImGuiCol_BorderShadow]));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, withScaledAlpha(style.Colors[ImGuiCol_ChildBg]));
    }
    const char* consoleWindowName = floatingWindow
        ? Loc::Window("WINDOW_CONSOLE", "Console")
        : Loc::Window("WINDOW_CONSOLE", "Console##MiniLogPanel");
    if (!ImGui::Begin(consoleWindowName, openPtr, miniFlags)) {
        ImGui::End();
        if (!floatingWindow) {
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            if (openPtr) consolePanelExpanded = keepOpen;
        }
        return;
    }
    if (!floatingWindow && openPtr) {
        consolePanelExpanded = keepOpen;
    }

    const ProjectConsoleTone consoleTone = projectManager.currentProject.isLoaded
        ? projectManager.currentProject.consoleSettings.tone
        : ProjectConsoleTone::Fun;
    auto typeLabel = [consoleTone](ConsoleMessageType type) -> const char* {
        if (consoleTone == ProjectConsoleTone::Concise) {
            switch (type) {
                case ConsoleMessageType::Warning: return "WARN";
                case ConsoleMessageType::Error: return "ERROR";
                case ConsoleMessageType::Success: return "OK";
                case ConsoleMessageType::Info:
                default: return "INFO";
            }
        }
        switch (type) {
            case ConsoleMessageType::Warning: return "Heads up!";
            case ConsoleMessageType::Error: return "Uh-oh!";
            case ConsoleMessageType::Success: return "All done!";
            case ConsoleMessageType::Info:
            default: return "Just so you know";
        }
    };

    bool settingsChanged = false;
    if (ImGui::Button("Clear")) {
        consoleLog.clear();
        latestErrorMessage.clear();
        latestErrorTimestamp.clear();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine();
    if (ImGui::Checkbox("Wrap Text", &consoleWrapText)) {
        settingsChanged = true;
    }

    // Console rows are painted straight onto the draw list rather than submitted as
    // ImGui text items, which is fast but leaves nothing to select or drag over. These
    // two are the only way text gets out of here: the whole log, or one row from the
    // right-click menu on it.
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        std::string all;
        for (const ConsoleEntry& entry : consoleLog) {
            all += "[" + entry.timestamp + "] " + typeLabel(entry.type) + "\n";
            all += entry.message + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
        showEditorToast("Copied " + std::to_string(consoleLog.size()) + " console message(s).",
                        ConsoleMessageType::Success, 1.4);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy every message to the clipboard.\nRight-click a row to copy just that one.");
    }

    ImGui::Separator();

    Texture* infoLogo = nullptr;
    Texture* warningLogo = nullptr;
    Texture* errorLogo = nullptr;
    Texture* successLogo = nullptr;
    if (rendererInitialized) {
        infoLogo = renderer.getTexture("Resources/Engine-Root/Info Logo.png");
        warningLogo = renderer.getTexture("Resources/Engine-Root/Warning Logo.png");
        errorLogo = renderer.getTexture("Resources/Engine-Root/Error Logo.png");
        successLogo = renderer.getTexture("Resources/Engine-Root/Modu-Logo.png");
    }


    auto typeColor = [&](ConsoleMessageType type) -> ImVec4 {
        switch (type) {
            case ConsoleMessageType::Warning: return withScaledAlpha(ImVec4(1.0f, 0.84f, 0.35f, 1.0f));
            case ConsoleMessageType::Error: return withScaledAlpha(ImVec4(1.0f, 0.46f, 0.46f, 1.0f));
            case ConsoleMessageType::Success: return withScaledAlpha(ImVec4(0.50f, 0.95f, 0.55f, 1.0f));
            case ConsoleMessageType::Info:
            default:
                return withScaledAlpha(ImVec4(0.65f, 0.84f, 1.0f, 1.0f));
        }
    };

    auto rowColor = [&](ConsoleMessageType type) -> ImU32 {
        switch (type) {
            case ConsoleMessageType::Warning: return withScaledAlphaU32(IM_COL32(74, 60, 20, 195));
            case ConsoleMessageType::Error: return withScaledAlphaU32(IM_COL32(88, 28, 28, 205));
            case ConsoleMessageType::Success: return withScaledAlphaU32(IM_COL32(20, 70, 30, 190));
            case ConsoleMessageType::Info:
            default:
                return withScaledAlphaU32(IM_COL32(30, 52, 78, 185));
        }
    };

    auto typeIcon = [&](ConsoleMessageType type) -> Texture* {
        switch (type) {
            case ConsoleMessageType::Warning: return warningLogo;
            case ConsoleMessageType::Error: return errorLogo;
            case ConsoleMessageType::Success: return successLogo;
            case ConsoleMessageType::Info:
            default:
                return infoLogo;
        }
    };

    ImGui::BeginChild("ConsoleOutput", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    const bool shouldScroll = autoScroll && (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f);
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const uint64_t logFingerprint = computeConsoleLogFingerprint(consoleLog);
    const bool cacheNeedsRefresh =
        gConsolePanelCache.entryCount != consoleLog.size() ||
        gConsolePanelCache.wrapText != consoleWrapText ||
        gConsolePanelCache.iconsAvailable != rendererInitialized ||
        gConsolePanelCache.tone != consoleTone ||
        std::abs(gConsolePanelCache.contentWidth - contentWidth) > 0.5f ||
        gConsolePanelCache.fingerprint != logFingerprint;

    if (cacheNeedsRefresh) {
        gConsolePanelCache.entryCount = consoleLog.size();
        gConsolePanelCache.wrapText = consoleWrapText;
        gConsolePanelCache.iconsAvailable = rendererInitialized;
        gConsolePanelCache.tone = consoleTone;
        gConsolePanelCache.contentWidth = contentWidth;
        gConsolePanelCache.fingerprint = logFingerprint;
        gConsolePanelCache.rows.clear();
        gConsolePanelCache.rows.resize(consoleLog.size());

        const float topPad = 4.0f;
        const float bottomPad = 4.0f;
        const float lineGap = 2.0f;
        const float minTextWidth = 32.0f;

        for (size_t i = 0; i < consoleLog.size(); ++i) {
            const ConsoleEntry& log = consoleLog[i];
            ConsoleRowMetrics& row = gConsolePanelCache.rows[i];
            row.header = "[" + log.timestamp + "] " + typeLabel(log.type);

            const bool hasIcon = rendererInitialized;
            const float textLeft = hasIcon ? 36.0f : 10.0f;
            row.textWidth = ImMax(minTextWidth, contentWidth - textLeft - 10.0f);
            row.headerSize = ImGui::CalcTextSize(row.header.c_str(), nullptr, false, FLT_MAX);

            if (consoleWrapText) {
                row.messageSize = ImGui::CalcTextSize(log.message.c_str(), nullptr, false, row.textWidth);
                row.rowWidth = ImMax(contentWidth, textLeft + row.textWidth + 10.0f);
            } else {
                row.messageSize = ImGui::CalcTextSize(log.message.c_str(), nullptr, false, FLT_MAX);
                row.rowWidth = ImMax(contentWidth, textLeft + ImMax(row.headerSize.x, row.messageSize.x) + 10.0f);
                row.textWidth = ImMax(minTextWidth, row.rowWidth - textLeft - 10.0f);
            }

            row.rowHeight = ImMax(26.0f, topPad + ImGui::GetTextLineHeight() + lineGap + row.messageSize.y + bottomPad);
        }
    }

    if (consoleLog.empty()) {
        ImGui::TextDisabled("No console messages yet.");
    } else {
        const float iconSize = 16.0f;
        const float topPad = 4.0f;
        const float lineGap = 2.0f;
        for (size_t i = 0; i < consoleLog.size(); ++i) {
            const ConsoleEntry& log = consoleLog[i];
            const ConsoleRowMetrics& row = gConsolePanelCache.rows[i];
            ImGui::PushID(static_cast<int>(i));

            Texture* icon = typeIcon(log.type);
            const bool hasIcon = icon && icon->GetID();
            float rowWidth = row.rowWidth;
            const float textLeft = hasIcon ? 36.0f : 10.0f;
            const float textWidth = row.textWidth;
            const float rowHeight = row.rowHeight;

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(rowWidth, rowHeight));
            // The Dummy is the only real item on this row, so it carries the copy menu.
            // It has to come before the spacer Dummy at the end of the loop, which
            // would otherwise be the item a context popup attaches to.
            if (ImGui::BeginPopupContextItem("##consoleRow")) {
                if (ImGui::MenuItem("Copy Message")) {
                    ImGui::SetClipboardText(log.message.c_str());
                }
                if (ImGui::MenuItem("Copy With Timestamp")) {
                    const std::string full = row.header + "\n" + log.message;
                    ImGui::SetClipboardText(full.c_str());
                }
                ImGui::EndPopup();
            }
            const ImVec2 rowMax = ImVec2(rowMin.x + rowWidth, rowMin.y + rowHeight);

            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(rowMin, rowMax, rowColor(log.type), 6.0f);
            draw->PushClipRect(rowMin, rowMax, true);

            if (hasIcon) {
                const ImVec2 iconMin(rowMin.x + 8.0f, rowMin.y + (rowHeight - iconSize) * 0.5f);
                const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
                draw->AddImage((ImTextureID)(intptr_t)icon->GetID(), iconMin, iconMax,
                               ImVec2(0, 1), ImVec2(1, 0), withScaledAlphaU32(IM_COL32_WHITE));
            }

            const float textX = rowMin.x + textLeft;
            draw->AddText(ImVec2(textX, rowMin.y + topPad),
                          ImGui::ColorConvertFloat4ToU32(typeColor(log.type)),
                          row.header.c_str());
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                          ImVec2(textX, rowMin.y + topPad + ImGui::GetTextLineHeight() + lineGap),
                          withScaledAlphaU32(ImGui::GetColorU32(ImGuiCol_Text)),
                          log.message.c_str(), nullptr,
                          consoleWrapText ? textWidth : 0.0f);

            draw->PopClipRect();
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::PopID();
        }
    }

    if (shouldScroll) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
    }

    ImGui::EndChild();
    if (settingsChanged) {
        saveEditorUserSettings();
    }

    ImGui::End();
    if (!floatingWindow) {
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }
}

void Engine::renderLatestErrorBar() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) return;
    const EditorChromeMetrics& chrome = getEditorChromeMetrics(uiChromeScale);

    const bool hasMessage = !consoleLog.empty();
    const ConsoleEntry* latest = hasMessage ? &consoleLog.back() : nullptr;
    const ConsoleMessageType latestType = latest ? latest->type : ConsoleMessageType::Info;

    Texture* errorLogo = nullptr;
    Texture* infoLogo = nullptr;
    Texture* warningLogo = nullptr;
    Texture* successLogo = nullptr;
    if (rendererInitialized) {
        errorLogo = renderer.getTexture("Resources/Engine-Root/Error Logo.png");
        infoLogo = renderer.getTexture("Resources/Engine-Root/Info Logo.png");
        warningLogo = renderer.getTexture("Resources/Engine-Root/Warning Logo.png");
        successLogo = renderer.getTexture("Resources/Engine-Root/Modu-Logo.png");
    }

    auto typeLabel = [](ConsoleMessageType type) -> const char* {
        switch (type) {
            case ConsoleMessageType::Warning: return "Warning";
            case ConsoleMessageType::Error: return "Error";
            case ConsoleMessageType::Success: return "Success";
            case ConsoleMessageType::Info:
            default:
                return "Info";
        }
    };

    auto typeAccentColor = [](ConsoleMessageType type) -> ImVec4 {
        switch (type) {
            case ConsoleMessageType::Warning: return ImVec4(1.0f, 0.84f, 0.35f, 1.0f);
            case ConsoleMessageType::Error: return ImVec4(1.0f, 0.46f, 0.46f, 1.0f);
            case ConsoleMessageType::Success: return ImVec4(0.50f, 0.95f, 0.55f, 1.0f);
            case ConsoleMessageType::Info:
            default:
                return ImVec4(0.70f, 0.88f, 1.0f, 1.0f);
        }
    };

    auto typeBackgroundColor = [](ConsoleMessageType type) -> ImVec4 {
        switch (type) {
            case ConsoleMessageType::Warning: return ImVec4(0.22f, 0.19f, 0.10f, 0.86f);
            case ConsoleMessageType::Error: return ImVec4(0.22f, 0.12f, 0.12f, 0.86f);
            case ConsoleMessageType::Success: return ImVec4(0.12f, 0.20f, 0.14f, 0.84f);
            case ConsoleMessageType::Info:
            default:
                return ImVec4(0.12f, 0.16f, 0.20f, 0.84f);
        }
    };

    auto typeBorderColor = [](ConsoleMessageType type) -> ImVec4 {
        switch (type) {
            case ConsoleMessageType::Warning: return ImVec4(0.82f, 0.66f, 0.26f, 0.62f);
            case ConsoleMessageType::Error: return ImVec4(0.86f, 0.32f, 0.32f, 0.64f);
            case ConsoleMessageType::Success: return ImVec4(0.30f, 0.72f, 0.42f, 0.60f);
            case ConsoleMessageType::Info:
            default:
                return ImVec4(0.34f, 0.54f, 0.74f, 0.56f);
        }
    };

    auto typeIcon = [&](ConsoleMessageType type) -> Texture* {
        switch (type) {
            case ConsoleMessageType::Warning: return warningLogo;
            case ConsoleMessageType::Error: return errorLogo;
            case ConsoleMessageType::Success: return successLogo;
            case ConsoleMessageType::Info:
            default:
                return infoLogo;
        }
    };

    Texture* icon = typeIcon(latestType);
    ImVec4 accentColor = typeAccentColor(latestType);
    ImVec4 bgColor = typeBackgroundColor(latestType);
    ImVec4 borderColor = typeBorderColor(latestType);

    std::string bodyText;
    if (latest) {
        bodyText = "[" + latest->timestamp + "] " + latest->message;
    } else {
        bodyText = "No console messages yet.";
    }

    const ImGuiWindow* dockHost = ImGui::FindWindowByName("DockSpace");
    ImVec2 hostPos = dockHost ? dockHost->Pos : viewport->WorkPos;
    ImVec2 hostSize = dockHost ? dockHost->Size : viewport->WorkSize;

    const float reserveHeight = chrome.bottomReserveHeight;
    const float barHeight = ImMax(16.0f, reserveHeight);
    const float stripTop = hostPos.y + hostSize.y - reserveHeight;

    ImVec2 barMin(hostPos.x, stripTop);
    ImVec2 barMax(hostPos.x + hostSize.x, barMin.y + barHeight);
    if (barMax.x - barMin.x <= 40.0f || barMax.y - barMin.y <= 8.0f) return;

    const ImGuiWindowFlags stripFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(barMin);
    ImGui::SetNextWindowSize(ImVec2(barMax.x - barMin.x, barMax.y - barMin.y));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    const bool stripVisible = ImGui::Begin("##ConsoleStatusStrip", nullptr, stripFlags);
    ImDrawList* draw = stripVisible ? ImGui::GetWindowDrawList() : nullptr;
    if (!draw) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    const ImU32 bgU32 = ImGui::ColorConvertFloat4ToU32(bgColor);
    const ImU32 borderU32 = ImGui::ColorConvertFloat4ToU32(borderColor);
    const ImU32 accentU32 = ImGui::ColorConvertFloat4ToU32(accentColor);
    const ImU32 textU32 = ImGui::GetColorU32(ImGuiCol_Text);

    draw->AddRectFilled(barMin, barMax, bgU32, 0.0f);
    draw->AddLine(ImVec2(barMin.x, barMin.y), ImVec2(barMax.x, barMin.y), borderU32, 1.0f);

    float cursorX = barMin.x + chrome.consoleMargin * 0.5f;
    const float fontY = barMin.y + (barHeight - ImGui::GetFontSize()) * 0.5f;

    if (icon && icon->GetID()) {
        const float iconSize = ImClamp(barHeight - 6.0f, 10.0f, 16.0f);
        const ImVec2 iconMin(cursorX, barMin.y + (barHeight - iconSize) * 0.5f);
        const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
        draw->AddImage((ImTextureID)(intptr_t)icon->GetID(), iconMin, iconMax,
                       ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE);
        cursorX += iconSize + 6.0f;
    }

    const char* label = latest ? typeLabel(latestType) : "Status";
    std::string labelText = std::string(label) + ":";
    draw->AddText(ImVec2(cursorX, fontY), accentU32, labelText.c_str());
    cursorX += ImGui::CalcTextSize(labelText.c_str()).x + chrome.consoleMargin * 0.5f;

    draw->PushClipRect(ImVec2(cursorX, barMin.y + 1.0f), ImVec2(barMax.x - chrome.consoleMargin * 0.5f, barMax.y - 1.0f), true);
    draw->AddText(ImVec2(cursorX, fontY), textU32, bodyText.c_str());
    draw->PopClipRect();

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void Engine::renderEditorToast() {
    if (!editorToast.visible || editorToast.message.empty()) {
        return;
    }

    const double now = glfwGetTime();
    const float appearSeconds = 0.18f;
    const float disappearSeconds = 0.22f;
    const double totalLifetime = appearSeconds + editorToast.holdSeconds + disappearSeconds;
    const double elapsed = now - editorToast.startTime;
    if (!editorToast.sticky && elapsed >= totalLifetime) {
        editorToast.visible = false;
        editorToast.message.clear();
        editorToast.detail.clear();
        return;
    }

    auto smoothstep = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    const float appearT = smoothstep(static_cast<float>(elapsed / appearSeconds));
    const float disappearT = editorToast.sticky
                                 ? 0.0f
                                 : smoothstep(static_cast<float>((elapsed - appearSeconds - editorToast.holdSeconds) /
                                                                 disappearSeconds));
    const float visibility = appearT * (1.0f - disappearT);
    if (visibility <= 0.001f) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
        return;
    }

    auto accentColor = [](ConsoleMessageType type) -> ImVec4 {
        switch (type) {
            case ConsoleMessageType::Warning: return ImVec4(0.98f, 0.82f, 0.32f, 1.0f);
            case ConsoleMessageType::Error: return ImVec4(0.98f, 0.46f, 0.46f, 1.0f);
            case ConsoleMessageType::Success: return ImVec4(0.47f, 0.91f, 0.57f, 1.0f);
            case ConsoleMessageType::Info:
            default: return ImVec4(0.70f, 0.88f, 1.0f, 1.0f);
        }
    };

    auto backgroundColor = [](ConsoleMessageType type) -> ImVec4 {
        switch (type) {
            case ConsoleMessageType::Warning: return ImVec4(0.20f, 0.15f, 0.05f, 0.94f);
            case ConsoleMessageType::Error: return ImVec4(0.24f, 0.08f, 0.08f, 0.94f);
            case ConsoleMessageType::Success: return ImVec4(0.08f, 0.19f, 0.10f, 0.94f);
            case ConsoleMessageType::Info:
            default: return ImVec4(0.08f, 0.16f, 0.22f, 0.94f);
        }
    };

    auto borderColor = [](ConsoleMessageType type) -> ImVec4 {
        switch (type) {
            case ConsoleMessageType::Warning: return ImVec4(0.80f, 0.62f, 0.18f, 0.90f);
            case ConsoleMessageType::Error: return ImVec4(0.84f, 0.24f, 0.24f, 0.92f);
            case ConsoleMessageType::Success: return ImVec4(0.23f, 0.72f, 0.35f, 0.92f);
            case ConsoleMessageType::Info:
            default: return ImVec4(0.30f, 0.55f, 0.76f, 0.90f);
        }
    };

    ImDrawList* draw = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(viewport));
    if (!draw) {
        return;
    }

    ImFont* font = ImGui::GetFont();
    if (!font) {
        return;
    }

    const float scale = 0.86f + 0.14f * appearT - 0.12f * disappearT;
    const float fontSize = ImGui::GetFontSize() * scale;
    const float detailFontSize = fontSize * 0.88f;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, editorToast.message.c_str());
    const float padX = 16.0f * scale;
    const float padY = 10.0f * scale;

    const bool progressCard = editorToast.progressMode;
    const float barHeight = progressCard ? ImMax(6.0f, 7.0f * scale) : 0.0f;
    const float barGap = progressCard ? 7.0f * scale : 0.0f;
    ImVec2 detailSize(0.0f, 0.0f);
    if (progressCard && !editorToast.detail.empty()) {
        detailSize = font->CalcTextSizeA(detailFontSize, FLT_MAX, 0.0f, editorToast.detail.c_str());
    }

    float contentWidth = textSize.x;
    float contentHeight = textSize.y;
    if (progressCard) {
        contentWidth = ImMax(contentWidth, detailSize.x);
        contentWidth = ImMax(contentWidth, 240.0f * scale);
        if (detailSize.y > 0.0f) contentHeight += 3.0f * scale + detailSize.y;
        contentHeight += barGap + barHeight;
    }

    const float width = contentWidth + padX * 2.0f;
    const float height = contentHeight + padY * 2.0f;
    const float slideY = (1.0f - appearT) * 26.0f + disappearT * 22.0f;

    const float bottomY = viewport->WorkPos.y + viewport->WorkSize.y - getEditorBottomStatusReserveHeight(uiChromeScale) - 16.0f;
    const float originX = progressCard
                              ? (viewport->WorkPos.x + viewport->WorkSize.x - width - 18.0f)
                              : (viewport->WorkPos.x + viewport->WorkSize.x * 0.5f - width * 0.5f);
    const ImVec2 min(originX, bottomY - height - slideY);
    const ImVec2 max(min.x + width, min.y + height);

    ImVec4 bg = backgroundColor(editorToast.type);
    ImVec4 border = borderColor(editorToast.type);
    ImVec4 accent = accentColor(editorToast.type);
    bg.w *= visibility;
    border.w *= visibility;
    accent.w *= visibility;
    ImVec4 textColor = ImVec4(0.96f, 0.98f, 1.0f, visibility);

    draw->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 3.0f),
                        ImVec2(max.x + 1.0f, max.y + 3.0f),
                        ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.20f * visibility)),
                        12.0f);
    draw->AddRectFilled(min, max, ImGui::GetColorU32(bg), 12.0f);
    draw->AddRect(min, max, ImGui::GetColorU32(border), 12.0f, 0, 1.5f);
    draw->AddRectFilled(min,
                        ImVec2(min.x + ImMax(4.0f * scale, 3.0f), max.y),
                        ImGui::GetColorU32(accent),
                        12.0f,
                        ImDrawFlags_RoundCornersLeft);
    draw->AddText(font,
                  fontSize,
                  ImVec2(min.x + padX, min.y + padY),
                  ImGui::GetColorU32(textColor),
                  editorToast.message.c_str());

    if (!progressCard) {
        return;
    }

    float cursorY = min.y + padY + textSize.y;
    if (detailSize.y > 0.0f) {
        cursorY += 3.0f * scale;
        ImVec4 detailColor = ImVec4(0.80f, 0.86f, 0.94f, 0.85f * visibility);
        draw->AddText(font,
                      detailFontSize,
                      ImVec2(min.x + padX, cursorY),
                      ImGui::GetColorU32(detailColor),
                      editorToast.detail.c_str());
        cursorY += detailSize.y;
    }

    cursorY += barGap;
    const ImVec2 trackMin(min.x + padX, cursorY);
    const ImVec2 trackMax(max.x - padX, cursorY + barHeight);
    if (trackMax.x <= trackMin.x) {
        return;
    }

    const float radius = barHeight * 0.5f;
    draw->AddRectFilled(trackMin, trackMax,
                        ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.35f * visibility)),
                        radius);

    const float trackWidth = trackMax.x - trackMin.x;
    if (editorToast.progress < 0.0f) {
        const float chunk = trackWidth * 0.32f;
        const float period = 1.4f;
        const float phase = static_cast<float>(std::fmod(now, period)) / period;
        const float travel = trackWidth - chunk;
        const float pos = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
        const float sweepX = trackMin.x + travel * smoothstep(pos);
        draw->AddRectFilled(ImVec2(sweepX, trackMin.y),
                            ImVec2(sweepX + chunk, trackMax.y),
                            ImGui::GetColorU32(accent), radius);
    } else {
        const float filled = trackWidth * std::clamp(editorToast.progress, 0.0f, 1.0f);
        if (filled > 0.5f) {
            draw->AddRectFilled(trackMin, ImVec2(trackMin.x + filled, trackMax.y),
                                ImGui::GetColorU32(accent), radius);
        }
    }
}

#pragma endregion

#pragma region Mesh Builder Panel
void Engine::renderMeshBuilderPanel() {
    if (!hasMeshBuilderPackage()) {
        showMeshBuilder = false;
        return;
    }
    ImGui::Begin(Loc::Window("WINDOW_MESH_BUILDER", "Mesh Builder (Legacy)"), &showMeshBuilder);
    ImGui::TextDisabled("Primary workflow moved to the viewport toolbar RMesh edit mode.");
    ImGui::Separator();

    auto recalcBounds = [this]() {
        if (!meshBuilder.hasMesh || meshBuilder.mesh.positions.empty()) return;
        meshBuilder.mesh.boundsMin = glm::vec3(FLT_MAX);
        meshBuilder.mesh.boundsMax = glm::vec3(-FLT_MAX);
        for (const auto& p : meshBuilder.mesh.positions) {
            meshBuilder.mesh.boundsMin.x = std::min(meshBuilder.mesh.boundsMin.x, p.x);
            meshBuilder.mesh.boundsMin.y = std::min(meshBuilder.mesh.boundsMin.y, p.y);
            meshBuilder.mesh.boundsMin.z = std::min(meshBuilder.mesh.boundsMin.z, p.z);
            meshBuilder.mesh.boundsMax.x = std::max(meshBuilder.mesh.boundsMax.x, p.x);
            meshBuilder.mesh.boundsMax.y = std::max(meshBuilder.mesh.boundsMax.y, p.y);
            meshBuilder.mesh.boundsMax.z = std::max(meshBuilder.mesh.boundsMax.z, p.z);
        }
    };

    ImGui::InputText("Mesh Path", meshBuilderPath, sizeof(meshBuilderPath));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::string err;
        if (!meshBuilder.load(meshBuilderPath, err)) {
            addConsoleMessage("MeshBuilder load failed: " + err, ConsoleMessageType::Error);
        } else {
            addConsoleMessage("Loaded raw mesh: " + meshBuilder.loadedPath, ConsoleMessageType::Success);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        std::string err;
        std::string path = strlen(meshBuilderPath) ? meshBuilderPath : meshBuilder.loadedPath;
        if (!meshBuilder.save(path, err)) {
            addConsoleMessage("MeshBuilder save failed: " + err, ConsoleMessageType::Error);
        } else {
            addConsoleMessage("Saved raw mesh: " + meshBuilder.loadedPath, ConsoleMessageType::Success);
            strncpy(meshBuilderPath, meshBuilder.loadedPath.c_str(), sizeof(meshBuilderPath) - 1);
            meshBuilderPath[sizeof(meshBuilderPath) - 1] = '\0';
        }
    }

    if (ImGui::Button("Load Selected File")) {
        if (!fileBrowser.selectedFile.empty() && IsMeshEditablePath(fileBrowser.selectedFile)) {
            strncpy(meshBuilderPath, fileBrowser.selectedFile.string().c_str(), sizeof(meshBuilderPath) - 1);
            meshBuilderPath[sizeof(meshBuilderPath) - 1] = '\0';
            std::string err;
            if (!meshBuilder.load(meshBuilderPath, err)) {
                addConsoleMessage("MeshBuilder load failed: " + err, ConsoleMessageType::Error);
            } else {
                addConsoleMessage("Loaded raw mesh: " + meshBuilder.loadedPath, ConsoleMessageType::Success);
            }
        } else {
            addConsoleMessage("Select a .rmesh or .mmesh file in the browser to load", ConsoleMessageType::Warning);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Recompute Normals")) {
        meshBuilder.recomputeNormals();
    }
    ImGui::SameLine();
    if (ImGui::Button("Flip Faces")) {
        meshBuilder.flipFaces();
        addConsoleMessage("Flipped raw mesh face winding", ConsoleMessageType::Success);
    }

    ImGui::Separator();

    if (!meshBuilder.hasMesh) {
        ImGui::TextDisabled("No mesh loaded.");
        ImGui::End();
        return;
    }

    ImGui::Text("Vertices: %zu", meshBuilder.mesh.positions.size());
    ImGui::Text("Faces: %zu", meshBuilder.mesh.faces.size());
    ImGui::Text("Bounds Min: (%.3f, %.3f, %.3f)", meshBuilder.mesh.boundsMin.x, meshBuilder.mesh.boundsMin.y, meshBuilder.mesh.boundsMin.z);
    ImGui::Text("Bounds Max: (%.3f, %.3f, %.3f)", meshBuilder.mesh.boundsMax.x, meshBuilder.mesh.boundsMax.y, meshBuilder.mesh.boundsMax.z);
    if (meshBuilder.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1,0.7f,0.2f,1),"*modified");
    }

    ImGui::SeparatorText("Vertices");
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Selected", &meshBuilder.selectedVertex);
    if (meshBuilder.selectedVertex < 0 || meshBuilder.selectedVertex >= (int)meshBuilder.mesh.positions.size()) {
        meshBuilder.selectedVertex = meshBuilder.mesh.positions.empty() ? -1 : 0;
    }

    if (meshBuilder.selectedVertex >= 0 && meshBuilder.selectedVertex < (int)meshBuilder.mesh.positions.size()) {
        glm::vec3& pos = meshBuilder.mesh.positions[meshBuilder.selectedVertex];
        float edit[3] = { pos.x, pos.y, pos.z };
        if (ImGui::InputFloat3("Position", edit, "%.4f")) {
            pos = glm::vec3(edit[0], edit[1], edit[2]);
            recalcBounds();
            meshBuilder.recomputeNormals();
            meshBuilder.dirty = true;
        }
        if (meshBuilder.mesh.hasUVs && meshBuilder.selectedVertex < (int)meshBuilder.mesh.uvs.size()) {
            glm::vec2& uv = meshBuilder.mesh.uvs[meshBuilder.selectedVertex];
            float uvEdit[2] = { uv.x, uv.y };
            if (ImGui::InputFloat2("UV", uvEdit, "%.4f")) {
                uv = glm::vec2(uvEdit[0], uvEdit[1]);
                meshBuilder.dirty = true;
            }
        }
    }

    ImGui::SeparatorText("Add Face / Fill");
    ImGui::InputTextWithHint("Indices", "e.g. 0,1,2 or 0 1 2 3", meshBuilderFaceInput, sizeof(meshBuilderFaceInput));
    ImGui::SameLine();
    if (ImGui::Button("Add Face")) {
        std::vector<uint32_t> indices;
        std::string token;
        std::stringstream ss(meshBuilderFaceInput);
        while (std::getline(ss, token, ',')) {
            std::stringstream inner(token);
            std::string sub;
            while (inner >> sub) {
                try {
                    uint32_t idx = static_cast<uint32_t>(std::stoul(sub));
                    indices.push_back(idx);
                } catch (...) {}
            }
        }
        if (indices.empty()) {
            addConsoleMessage("Enter vertex indices separated by commas or spaces", ConsoleMessageType::Warning);
        } else {
            std::string err;
            if (!meshBuilder.addFace(indices, err)) {
                addConsoleMessage("Add face failed: " + err, ConsoleMessageType::Error);
            } else {
                addConsoleMessage("Added face with " + std::to_string(indices.size()) + " verts", ConsoleMessageType::Success);
            }
        }
    }

    ImGui::SeparatorText("Faces (first 16)");
    int maxFaces = std::min<int>(16, meshBuilder.mesh.faces.size());
    for (int i = 0; i < maxFaces; i++) {
        const auto& f = meshBuilder.mesh.faces[i];
        ImGui::Text("%d: %u, %u, %u", i, f.x, f.y, f.z);
    }

    ImGui::End();
}

#pragma endregion

#pragma region Dialogs
void Engine::renderLegacySceneLayoutModal() {
    if (!showLegacySceneLayoutDialog) {
        legacySceneLayoutDialogOpened = false;
        return;
    }

    if (!legacySceneLayoutDialogOpened) {
        ImGui::OpenPopup("Old Scene Layout Detected");
        legacySceneLayoutDialogOpened = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 220.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Old Scene Layout Detected", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted("Hey! This is an old scene with an old layout.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Would you like to move this current old scene into a compatibility folder and convert this scene to the new layout?");
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float buttonWidth = 150.0f;
        if (ImGui::Button("Convert Scene", ImVec2(buttonWidth, 0.0f))) {
            legacySceneSaveChoice = LegacySceneSaveChoice::SaveModular;
            if (executeSceneSave(pendingSceneSaveRequest.destinationSceneName,
                                 SceneSerializer::SavePreference::PreferModular,
                                 true)) {
                showLegacySceneLayoutDialog = false;
                legacySceneLayoutDialogOpened = false;
                ImGui::CloseCurrentPopup();
                continuePendingScenePostAction();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep Old Layout For Now", ImVec2(210.0f, 0.0f))) {
            legacySceneSaveChoice = LegacySceneSaveChoice::KeepLegacy;
            if (executeSceneSave(pendingSceneSaveRequest.destinationSceneName,
                                 SceneSerializer::SavePreference::ForceLegacyFlat,
                                 false)) {
                showLegacySceneLayoutDialog = false;
                legacySceneLayoutDialogOpened = false;
                ImGui::CloseCurrentPopup();
                continuePendingScenePostAction();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            showLegacySceneLayoutDialog = false;
            legacySceneLayoutDialogOpened = false;
            resetPendingSceneSaveRequest();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void Engine::renderPlayModeSaveModal() {
    if (!showPlayModeSaveDialog) {
        playModeSaveDialogOpened = false;
        return;
    }

    if (!playModeSaveDialogOpened) {
        ImGui::OpenPopup(Loc::WindowRef("Save While In Play Mode"));
        playModeSaveDialogOpened = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580.0f, 235.0f), ImGuiCond_Appearing);

    const PendingSceneSaveRequest pending = pendingSceneSaveRequest;
    auto closeModal = [&]() {
        showPlayModeSaveDialog = false;
        playModeSaveDialogOpened = false;
        ImGui::CloseCurrentPopup();
    };
    auto rememberChoice = [&](PlayModeSaveChoice choice) {
        if (!playModeSaveDontAskAgain) return;
        playModeSaveChoice = choice;
        saveEditorUserSettings();
    };
    auto runPendingSave = [&]() {
        performSceneSaveRequest(pending.destinationSceneName,
                                pending.postAction,
                                pending.postActionPayload,
                                pending.allowLegacyUpgradePrompt);
    };

    if (ImGui::BeginPopupModal(Loc::Window("DIALOG_PLAY_MODE_SAVE", "Save While In Play Mode"),
                               nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(Loc::T("DIALOG_PLAY_MODE_SAVE_BODY",
                                      "You're about to save a scene while being in Play Mode.\n"
                                      "What action should you take?"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", Loc::T("DIALOG_PLAY_MODE_SAVE_HINT",
                                         "Saving writes the current running scene to disk,\n"
                                         "including every change your scripts have already made to it.\n"
                                         "Leaving Play Mode first restores the scene as you authored it."));
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Checkbox(Loc::Widget("DIALOG_PLAY_MODE_SAVE_DONT_ASK", "Don't ask again"),
                        &playModeSaveDontAskAgain);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button(Loc::Widget("DIALOG_PLAY_MODE_SAVE_ANYWAY", "Save Anyway"),
                          ImVec2(140.0f, 0.0f))) {
            rememberChoice(PlayModeSaveChoice::SaveAnyway);
            closeModal();
            runPendingSave();
        }
        ImGui::SameLine();
        if (ImGui::Button(Loc::Widget("DIALOG_PLAY_MODE_SAVE_EXIT_FIRST", "Exit Play Mode, then save"),
                          ImVec2(220.0f, 0.0f))) {
            rememberChoice(PlayModeSaveChoice::ExitPlayMode);
            closeModal();
            togglePlayMode();
            runPendingSave();
        }
        ImGui::SameLine();
        if (ImGui::Button(Loc::Widget("DIALOG_PLAY_MODE_SAVE_CANCEL", "Cancel"),
                          ImVec2(100.0f, 0.0f))) {
            closeModal();
            resetPendingSceneSaveRequest();
        }

        ImGui::EndPopup();
    }
}

void Engine::renderDialogs() {
    renderLegacySceneLayoutModal();
    renderPlayModeSaveModal();

    if (showNewSceneDialog) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(350, 130), ImGuiCond_Appearing);

        if (ImGui::Begin("New Scene", &showNewSceneDialog,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
            ImGui::Text("Scene Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##NewSceneName", newSceneName, sizeof(newSceneName));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float buttonWidth = 80;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth * 2 - 20);

            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
                showNewSceneDialog = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Create", ImVec2(buttonWidth, 0))) {
                if (strlen(newSceneName) > 0) {
                    createNewScene(newSceneName);
                    showNewSceneDialog = false;
                    memset(newSceneName, 0, sizeof(newSceneName));
                }
            }
        }
        ImGui::End();
    }

    if (showCompilePopup && !compileInProgress && compilePopupHideTime > 0.0 &&
        glfwGetTime() >= compilePopupHideTime) {
        showCompilePopup = false;
        compilePopupOpened = false;
        compilePopupHideTime = 0.0;
    }

    if (showCompilePopup && compileUiToast) {
        ImGuiIO& io = ImGui::GetIO();
        const double now = glfwGetTime();
        const bool finished = !compileInProgress && compileCompletionStart > 0.0;

        float alpha = 1.0f;
        if (finished && compilePopupHideTime > 0.0) {
            const float remaining = static_cast<float>(compilePopupHideTime - now);
            alpha = std::clamp(remaining / 0.45f, 0.0f, 1.0f);
        }

        float progress = 1.0f;
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            progress = compileInProgress ? compileProgress : 1.0f;
        }
        const std::string label = compileCurrentLabel.empty() ? std::string("Scripts") : compileCurrentLabel;

        const ImU32 accent = compileInProgress
            ? IM_COL32(232, 184, 36, static_cast<int>(255 * alpha))
            : (lastCompileSuccess ? IM_COL32(78, 196, 120, static_cast<int>(255 * alpha))
                                  : IM_COL32(224, 96, 96, static_cast<int>(255 * alpha)));

        const float margin = 16.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(312.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.94f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(13.0f, 11.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.11f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.36f, alpha));
        const ImGuiWindowFlags toastFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("##CompileToast", nullptr, toastFlags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float lineH = ImGui::GetTextLineHeight();
            const ImVec2 dotPos = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(ImVec2(dotPos.x + 5.0f, dotPos.y + lineH * 0.5f), 5.0f, accent, 16);
            ImGui::Dummy(ImVec2(18.0f, 0.0f));
            ImGui::SameLine();

            const char* headline = compileInProgress ? "Compiling"
                                  : (lastCompileSuccess ? "Compiled" : "Compile failed");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.96f, 1.0f, alpha));
            ImGui::TextUnformatted(headline);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.70f, 0.84f, alpha));
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopStyleColor();
            if (compileBatchTotal > 1) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.58f, 0.70f, alpha));
                ImGui::Text("%d/%d", std::min(compileBatchCompleted + 1, compileBatchTotal), compileBatchTotal);
                ImGui::PopStyleColor();
            }

            ImGui::Dummy(ImVec2(0.0f, 5.0f));
            const ImVec2 barMin = ImGui::GetCursorScreenPos();
            const float barW = ImGui::GetContentRegionAvail().x;
            const ImVec2 barMax(barMin.x + barW, barMin.y + 4.0f);
            dl->AddRectFilled(barMin, barMax, IM_COL32(38, 42, 52, static_cast<int>(255 * alpha)), 2.0f);
            const float fillT = compileInProgress ? std::clamp(progress, 0.03f, 1.0f) : 1.0f;
            dl->AddRectFilled(barMin, ImVec2(barMin.x + barW * fillT, barMax.y), accent, 2.0f);
            ImGui::Dummy(ImVec2(barW, 4.0f));
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    } else if (showCompilePopup) {
        if (!compilePopupOpened) {
            ImGui::OpenPopup("Script Compile");
            compilePopupOpened = true;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        struct CompileWindowIcon {
            ImTextureID id = static_cast<ImTextureID>(0);
            bool flipY = false;
        };
        const bool hasVulkanUiImages = usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
        auto resolveCompileWindowIcon = [&](const char* iconPath) -> CompileWindowIcon {
            if (!iconPath || !*iconPath) {
                return {};
            }
            if (rendererInitialized) {
                if (Texture* icon = renderer.getTexture(iconPath, MaterialProperties::TextureFilter::Point);
                    icon && icon->GetID()) {
                    return { static_cast<ImTextureID>(icon->GetID()), true };
                }
            }
            if (hasVulkanUiImages && vulkanRenderer) {
                ImTextureID icon = vulkanRenderer->getOrCreateUIImage(iconPath);
                if (icon != static_cast<ImTextureID>(0)) {
                    return { icon, false };
                }
            }
            return {};
        };
        auto getCompileStateIcon = [&](bool success, bool warning) -> CompileWindowIcon {
            if (!success) {
                return resolveCompileWindowIcon("Resources/Engine-Root/Compiler window/Script Failed.png");
            }
            if (warning) {
                return resolveCompileWindowIcon("Resources/Engine-Root/Compiler window/Script Warning.png");
            }
            return resolveCompileWindowIcon("Resources/Engine-Root/Compiler window/Script Completed.png");
        };

        const double now = glfwGetTime();
        const int historyCount = static_cast<int>(compileHistory.size());
        const int visibleHistoryRows = std::clamp(historyCount, 1, 5);
        const float historyHeight = static_cast<float>(visibleHistoryRows) * 20.0f + 2.0f;
        const bool compileFinished = !compileInProgress && compileCompletionStart > 0.0;
        const float completionBlend = compileFinished
            ? std::clamp(static_cast<float>((now - compileCompletionStart) / 0.24), 0.0f, 1.0f)
            : 0.0f;
        const float jobsBlend = compileInProgress ? 1.0f : std::max(0.0f, 1.0f - completionBlend);
        const float logBlend = compileFinished ? completionBlend : 0.0f;
        const float targetWidth = std::clamp(io.DisplaySize.x * 0.78f, 640.0f, 1600.0f);
        const float targetMinHeight = compileFinished && !lastCompileLog.empty() ? 320.0f : 180.0f;
        const float targetLogHeight =
            logBlend > 0.0f ? std::clamp(io.DisplaySize.y * 0.28f, 160.0f, 420.0f) * logBlend + 12.0f : 0.0f;
        const float targetHeight =
            std::clamp(96.0f + historyHeight * jobsBlend + targetLogHeight, targetMinHeight, io.DisplaySize.y * 0.72f);
        static ImVec2 compilePopupSize = ImVec2(targetWidth, targetHeight);
        const float popupLerp = std::clamp(io.DeltaTime * 12.0f, 0.0f, 1.0f);
        compilePopupSize.x = ImLerp(compilePopupSize.x, targetWidth, popupLerp);
        compilePopupSize.y = ImLerp(compilePopupSize.y, targetHeight, popupLerp);
        ImGui::SetNextWindowPos(center, compileInProgress ? ImGuiCond_Always : ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (compileInProgress) {
            ImGui::SetNextWindowSize(compilePopupSize, ImGuiCond_Always);
        } else {
            ImGui::SetNextWindowSize(compilePopupSize, ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 180.0f),
                                                ImVec2(io.DisplaySize.x * 0.96f, io.DisplaySize.y * 0.90f));
        }
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
        if (compileInProgress) flags |= ImGuiWindowFlags_NoResize;
        bool allowClose = !compileInProgress;
        if (ImGui::BeginPopupModal("Script Compile", allowClose ? &showCompilePopup : nullptr, flags)) {
            float progress = 1.0f;
            std::string stageText;
            {
                std::lock_guard<std::mutex> lock(compileMutex);
                progress = compileInProgress ? compileProgress : 1.0f;
                stageText = compileInProgress ? compileStage : (lastCompileSuccess ? "Done" : "Failed");
            }
            const char* stageLabel = stageText.empty() ? "Working..." : stageText.c_str();
            if (progress <= 0.0f) progress = 0.02f;

            ImGui::SameLine();
            ImGui::TextUnformatted("Script Compiler");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextUnformatted(lastCompileStatus.empty() ? "Idle" : lastCompileStatus.c_str());
            if (!compileCurrentLabel.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", compileCurrentLabel.c_str());
                if (compileBatchTotal > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d/%d", std::min(compileBatchCompleted + 1, std::max(1, compileBatchTotal)), std::max(1, compileBatchTotal));
                }
            }
            if (compileInProgress) {
                ImGui::Spacing();
                ImGui::BufferingBar("##CompileLoadingBar",
                                    progress,
                                    ImVec2(ImGui::GetContentRegionAvail().x, 10.0f),
                                    ImGui::GetColorU32(ImVec4(0.20f, 0.24f, 0.32f, 1.0f)),
                                    ImGui::GetColorU32(ImVec4(0.90f, 0.72f, 0.14f, 1.0f)));
                ImGui::TextDisabled("%s", stageLabel);
                if (compileInProgress && !compileRequestQueue.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("+%zu queued", compileRequestQueue.size());
                }
            } else if (compileFinished) {
                ImGui::Spacing();
                ImDrawList* barDraw = ImGui::GetWindowDrawList();
                ImVec2 barMin = ImGui::GetCursorScreenPos();
                ImVec2 barMax(barMin.x + ImGui::GetContentRegionAvail().x, barMin.y + 10.0f);
                barDraw->AddRectFilled(barMin, barMax, ImGui::GetColorU32(ImVec4(0.18f, 0.21f, 0.28f, 1.0f)), 4.0f);
                barDraw->AddRectFilled(barMin,
                                       ImVec2(barMin.x + (barMax.x - barMin.x) * completionBlend, barMax.y),
                                       ImGui::GetColorU32(ImVec4(0.28f, 0.72f, 0.40f, 1.0f)),
                                       4.0f);
                ImGui::Dummy(ImVec2(0.0f, 12.0f));
                ImGui::TextDisabled("%s", stageLabel);
            }

            if (compileInProgress || jobsBlend > 0.01f) {
                ImGui::Separator();
                ImGui::TextDisabled("Jobs");
                ImGui::BeginChild("CompileHistoryList", ImVec2(0.0f, std::max(20.0f, historyHeight * jobsBlend)), false);
                const float rowAlpha = compileInProgress ? 1.0f : jobsBlend;
                if (historyCount == 0 && !compileInProgress) {
                    ImGui::TextDisabled("No compile results yet.");
                } else {
                    if (compileInProgress) {
                        ImVec2 rowMin = ImGui::GetCursorScreenPos();
                        ImVec2 rowSize(ImGui::GetContentRegionAvail().x, 18.0f);
                        ImGui::InvisibleButton("##CompileActiveRow", rowSize);
                        ImDrawList* rowDraw = ImGui::GetWindowDrawList();
                        const CompileWindowIcon activeIcon = resolveCompileWindowIcon("Resources/Engine-Root/Compiler window/Script Warning.png");
                        if (activeIcon.id != static_cast<ImTextureID>(0)) {
                            const ImVec2 iconMin(rowMin.x + 2.0f, rowMin.y + 4.0f);
                            const ImVec2 iconMax(iconMin.x + 10.0f, iconMin.y + 10.0f);
                            const ImVec2 uvMin = activeIcon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
                            const ImVec2 uvMax = activeIcon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
                            rowDraw->AddImage(activeIcon.id, iconMin, iconMax, uvMin, uvMax, IM_COL32(255, 255, 255, static_cast<int>(200.0f * rowAlpha)));
                        }
                        rowDraw->AddText(ImVec2(rowMin.x + 16.0f, rowMin.y + 1.0f),
                                         ImGui::GetColorU32(ImVec4(0.95f, 0.97f, 1.0f, rowAlpha)),
                                         compileCurrentLabel.empty() ? "Current compile" : compileCurrentLabel.c_str());
                        const std::string runningStatus = lastCompileStatus.empty() ? "Working..." : lastCompileStatus;
                        const ImVec2 statusSize = ImGui::CalcTextSize(runningStatus.c_str());
                        rowDraw->AddText(ImVec2(rowMin.x + rowSize.x - statusSize.x - 4.0f, rowMin.y + 1.0f),
                                         ImGui::GetColorU32(ImVec4(0.78f, 0.90f, 1.0f, rowAlpha)),
                                         runningStatus.c_str());
                        rowDraw->AddLine(ImVec2(rowMin.x, rowMin.y + rowSize.y + 1.0f),
                                         ImVec2(rowMin.x + rowSize.x, rowMin.y + rowSize.y + 1.0f),
                                         ImGui::GetColorU32(ImVec4(0.24f, 0.27f, 0.34f, 0.80f * rowAlpha)));
                    }

                    for (size_t index = 0; index < compileHistory.size(); ++index) {
                        const auto& item = compileHistory[index];
                        const float fade = std::clamp(static_cast<float>((now - item.completedAt) / 0.22), 0.0f, 1.0f) * rowAlpha;
                        const float rowHeight = 18.0f;
                        ImVec2 rowMin = ImGui::GetCursorScreenPos();
                        ImVec2 rowSize(ImGui::GetContentRegionAvail().x, rowHeight);
                        std::string rowId = "##CompileHistoryRow" + std::to_string(index);
                        ImGui::InvisibleButton(rowId.c_str(), rowSize);
                        ImDrawList* rowDraw = ImGui::GetWindowDrawList();
                        const CompileWindowIcon icon = getCompileStateIcon(item.success, item.warning);
                        if (icon.id != static_cast<ImTextureID>(0)) {
                            const ImVec2 iconMin(rowMin.x + 2.0f, rowMin.y + 4.0f);
                            const ImVec2 iconMax(iconMin.x + 10.0f, iconMin.y + 10.0f);
                            const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
                            const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
                            rowDraw->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax, IM_COL32(255, 255, 255, static_cast<int>(fade * 255.0f)));
                        }

                        const ImU32 titleColor = ImGui::GetColorU32(ImVec4(0.96f, 0.98f, 1.0f, fade));
                        rowDraw->AddText(ImVec2(rowMin.x + 16.0f, rowMin.y + 1.0f),
                                         titleColor,
                                         item.displayLabel.c_str());

                        const ImVec2 statusSize = ImGui::CalcTextSize(item.statusLabel.c_str());
                        const ImVec2 statusPos(rowMin.x + rowSize.x - statusSize.x - 4.0f, rowMin.y + 1.0f);
                        rowDraw->AddText(statusPos,
                                         item.success
                                             ? ImGui::GetColorU32(ImVec4(0.60f, 0.95f, 0.72f, fade))
                                             : ImGui::GetColorU32(ImVec4(0.98f, 0.62f, 0.62f, fade)),
                                         item.statusLabel.c_str());

                        rowDraw->AddLine(ImVec2(rowMin.x, rowMin.y + rowSize.y + 1.0f),
                                         ImVec2(rowMin.x + rowSize.x, rowMin.y + rowSize.y + 1.0f),
                                         ImGui::GetColorU32(ImVec4(0.24f, 0.27f, 0.34f, 0.80f * fade)));
                    }
                }
                ImGui::EndChild();
            }

            if (compileFinished && !lastCompileLog.empty()) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextDisabled("Log");
                const float closeRowReserve =
                    allowClose ? (ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f) : 8.0f;
                const float logHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - closeRowReserve);
                ImGui::BeginChild("CompileLog", ImVec2(0.0f, logHeight), true);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(lastCompileLog.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }

            ImGui::Spacing();
            if (allowClose && ImGui::Button("Close", ImVec2(80, 0))) {
                showCompilePopup = false;
                ImGui::CloseCurrentPopup();
                compilePopupOpened = false;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(3);
    } else {
        compilePopupOpened = false;
    }

    if (showSaveSceneAsDialog) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(350, 130), ImGuiCond_Appearing);

        if (ImGui::Begin("Save Scene As", &showSaveSceneAsDialog,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
            ImGui::Text("Scene Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##SaveSceneAsName", saveSceneAsName, sizeof(saveSceneAsName));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float buttonWidth = 80;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth * 2 - 20);

            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
                showSaveSceneAsDialog = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Save", ImVec2(buttonWidth, 0))) {
                if (strlen(saveSceneAsName) > 0) {
                    requestSceneSave(saveSceneAsName,
                                     PendingScenePostAction::None,
                                     "",
                                     true);
                    showSaveSceneAsDialog = false;
                    memset(saveSceneAsName, 0, sizeof(saveSceneAsName));
                }
            }
        }
        ImGui::End();
    }

    // OBJ Import dialog
    if (showImportOBJDialog) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400, 160), ImGuiCond_Appearing);

        if (ImGui::Begin("Import OBJ Model", &showImportOBJDialog,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
            ImGui::Text("File: %s", fs::path(pendingOBJPath).filename().string().c_str());
            ImGui::TextDisabled("%s", pendingOBJPath.c_str());

            ImGui::Spacing();

            ImGui::Text("Object Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ImportOBJName", importOBJName, sizeof(importOBJName));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float buttonWidth = 80;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth * 2 - 20);

            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
                showImportOBJDialog = false;
                pendingOBJPath.clear();
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.4f, 1.0f));
            if (ImGui::Button("Import", ImVec2(buttonWidth, 0))) {
                importOBJToScene(pendingOBJPath, importOBJName);
                showImportOBJDialog = false;
                pendingOBJPath.clear();
                memset(importOBJName, 0, sizeof(importOBJName));
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::End();
    }

    // General model import dialog (Assimp-backed)
    if (showImportModelDialog) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420, 180), ImGuiCond_Appearing);

        if (ImGui::Begin("Import Model", &showImportModelDialog,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
            ImGui::Text("File: %s", fs::path(pendingModelPath).filename().string().c_str());
            ImGui::TextDisabled("%s", pendingModelPath.c_str());

            ImGui::Spacing();

            ImGui::Text("Object Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ImportModelName", importModelName, sizeof(importModelName));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float buttonWidth = 80;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth * 2 - 20);

            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
                showImportModelDialog = false;
                pendingModelPath.clear();
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.4f, 1.0f));
            if (ImGui::Button("Import", ImVec2(buttonWidth, 0))) {
                importModelToScene(pendingModelPath, importModelName);
                showImportModelDialog = false;
                pendingModelPath.clear();
                memset(importModelName, 0, sizeof(importModelName));
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::End();
    }

    if (showImportSpriteSheetDialog && hasSpritesheetPackage()) {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(460, 260), ImGuiCond_Appearing);

        if (ImGui::Begin("Import Sprite Sheet", &showImportSpriteSheetDialog,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking)) {
            ImGui::Text("File: %s", fs::path(pendingSpriteSheetPath).filename().string().c_str());
            ImGui::TextDisabled("%s", pendingSpriteSheetPath.c_str());
            int texW = 0;
            int texH = 0;
            if (!pendingSpriteSheetPath.empty()) {
                if (Texture* tex = renderer.getTexture(pendingSpriteSheetPath)) {
                    texW = tex->GetWidth();
                    texH = tex->GetHeight();
                }
            }
            if (texW > 0 && texH > 0) {
                ImGui::TextDisabled("Texture Size: %d x %d", texW, texH);
            }

            ImGui::Spacing();
            ImGui::Text("Object Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ImportSpriteSheetName", importSpriteSheetName, sizeof(importSpriteSheetName));

            ImGui::Spacing();
            ImGui::DragInt("Columns", &importSpriteSheetColumns, 1.0f, 1, 1024);
            ImGui::DragInt("Rows", &importSpriteSheetRows, 1.0f, 1, 1024);
            importSpriteSheetColumns = std::max(1, importSpriteSheetColumns);
            importSpriteSheetRows = std::max(1, importSpriteSheetRows);
            ImGui::DragFloat("FPS", &importSpriteSheetFps, 0.1f, 1.0f, 120.0f, "%.1f");
            importSpriteSheetFps = std::clamp(importSpriteSheetFps, 1.0f, 120.0f);

            auto spriteSheetTargetLabel = [](SpriteSheetImportTarget target) {
                switch (target) {
                    case SpriteSheetImportTarget::Sprite25D: return "2.5D Sprite";
                    case SpriteSheetImportTarget::Sprite2D: return "Sprite2D";
                    case SpriteSheetImportTarget::UIImage:
                    default:
                        return "UI Image";
                }
            };

            if (!has2DWorldPackage() && importSpriteSheetTarget == SpriteSheetImportTarget::Sprite2D) {
                importSpriteSheetTarget = isProject25DPipeline()
                    ? SpriteSheetImportTarget::Sprite25D
                    : SpriteSheetImportTarget::UIImage;
            }

            if (ImGui::BeginCombo("Create As", spriteSheetTargetLabel(importSpriteSheetTarget))) {
                const bool imageSelected = importSpriteSheetTarget == SpriteSheetImportTarget::UIImage;
                if (ImGui::Selectable("UI Image", imageSelected)) {
                    importSpriteSheetTarget = SpriteSheetImportTarget::UIImage;
                }
                if (imageSelected) ImGui::SetItemDefaultFocus();

                const bool sprite25DSelected = importSpriteSheetTarget == SpriteSheetImportTarget::Sprite25D;
                if (ImGui::Selectable("2.5D Sprite", sprite25DSelected)) {
                    importSpriteSheetTarget = SpriteSheetImportTarget::Sprite25D;
                }

                if (has2DWorldPackage()) {
                    const bool sprite2DSelected = importSpriteSheetTarget == SpriteSheetImportTarget::Sprite2D;
                    if (ImGui::Selectable("Sprite2D", sprite2DSelected)) {
                        importSpriteSheetTarget = SpriteSheetImportTarget::Sprite2D;
                    }
                } else {
                    ImGui::BeginDisabled();
                    ImGui::Selectable("Sprite2D (requires moduengine.2d-world)", false);
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float buttonWidth = 80;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonWidth * 2 - 20);

            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
                showImportSpriteSheetDialog = false;
                pendingSpriteSheetPath.clear();
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.4f, 1.0f));
            if (ImGui::Button("Import", ImVec2(buttonWidth, 0))) {
                std::string baseName = importSpriteSheetName;
                if (baseName.empty()) {
                    baseName = fs::path(pendingSpriteSheetPath).stem().string();
                }

                ObjectType type = ObjectType::UIImage;
                switch (importSpriteSheetTarget) {
                    case SpriteSheetImportTarget::Sprite25D:
                        type = ObjectType::Sprite25D;
                        break;
                    case SpriteSheetImportTarget::Sprite2D:
                        type = ObjectType::Sprite2D;
                        break;
                    case SpriteSheetImportTarget::UIImage:
                    default:
                        type = ObjectType::UIImage;
                        break;
                }

                const bool needsCanvasParent =
                    (type == ObjectType::UIImage || type == ObjectType::Sprite2D);
                int canvasId = -1;
                if (needsCanvasParent) {
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
                }

                addObject(type, baseName);
                if (!sceneObjects.empty()) {
                    SceneObject& created = sceneObjects.back();
                    created.albedoTexturePath = pendingSpriteSheetPath;
                    created.material.textureFilter = MaterialProperties::TextureFilter::Point;
                    created.ui.spriteSheetEnabled = true;
                    created.ui.spriteSheetColumns = std::max(1, importSpriteSheetColumns);
                    created.ui.spriteSheetRows = std::max(1, importSpriteSheetRows);
                    created.ui.spriteSheetFrame = 0;
                    created.ui.spriteSheetFps = importSpriteSheetFps;
                    created.ui.spriteSheetLoop = true;
                    created.ui.spriteCustomFramesEnabled = false;
                    created.ui.spriteSourceWidth = texW;
                    created.ui.spriteSourceHeight = texH;
                    created.ui.spriteCustomFrames.clear();
                    if (texW > 0 && texH > 0) {
                        created.ui.size.x = std::max(1.0f, static_cast<float>(texW) / static_cast<float>(created.ui.spriteSheetColumns));
                        created.ui.size.y = std::max(1.0f, static_cast<float>(texH) / static_cast<float>(created.ui.spriteSheetRows));
                    }
                    if (needsCanvasParent && canvasId >= 0) {
                        setParent(created.id, canvasId);
                    }
                    projectManager.currentProject.hasUnsavedChanges = true;
                    addConsoleMessage("Imported sprite sheet: " + pendingSpriteSheetPath, ConsoleMessageType::Success);
                }

                showImportSpriteSheetDialog = false;
                pendingSpriteSheetPath.clear();
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::End();
    }
}
