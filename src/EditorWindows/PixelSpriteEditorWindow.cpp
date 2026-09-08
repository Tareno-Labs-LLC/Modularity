#include "../EditorLocalization.h"
#include "Engine.h"
#include "ThirdParty/ModuGUI/imgui.h"
#include "ThirdParty/glfw/deps/stb_image_write.h"
#include "../../include/ThirdParty/stb_image.h"
#include "../SpritesheetFormat.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <queue>
#include <sstream>

namespace Loc = Modularity::Loc;

namespace {
struct PixelRgba {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    unsigned char a = 255;
};

PixelRgba ToRgba8(const glm::vec4& color) {
    return PixelRgba{
        static_cast<unsigned char>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<unsigned char>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<unsigned char>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<unsigned char>(std::clamp(color.a, 0.0f, 1.0f) * 255.0f + 0.5f)
    };
}

bool Matches(const std::vector<unsigned char>& pixels, int width, int height, int x, int y, const PixelRgba& color) {
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    const size_t idx = static_cast<size_t>((y * width + x) * 4);
    return pixels[idx + 0] == color.r &&
           pixels[idx + 1] == color.g &&
           pixels[idx + 2] == color.b &&
           pixels[idx + 3] == color.a;
}

void SetPixel(std::vector<unsigned char>& pixels, int width, int height, int x, int y, const PixelRgba& color) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const size_t idx = static_cast<size_t>((y * width + x) * 4);
    pixels[idx + 0] = color.r;
    pixels[idx + 1] = color.g;
    pixels[idx + 2] = color.b;
    pixels[idx + 3] = color.a;
}

glm::ivec4 NormalizeRect(glm::ivec2 a, glm::ivec2 b) {
    const int minX = std::min(a.x, b.x);
    const int minY = std::min(a.y, b.y);
    const int maxX = std::max(a.x, b.x);
    const int maxY = std::max(a.y, b.y);
    return glm::ivec4(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

glm::ivec4 ClampSpriteClipRect(glm::ivec4 rect, int width, int height) {
    rect.z = std::max(1, rect.z);
    rect.w = std::max(1, rect.w);
    rect.x = std::clamp(rect.x, 0, std::max(0, width - 1));
    rect.y = std::clamp(rect.y, 0, std::max(0, height - 1));
    rect.z = std::min(rect.z, std::max(1, width - rect.x));
    rect.w = std::min(rect.w, std::max(1, height - rect.y));
    return rect;
}

void FloodFill(std::vector<unsigned char>& pixels, int width, int height, int startX, int startY, const PixelRgba& target, const PixelRgba& replacement) {
    if (target.r == replacement.r && target.g == replacement.g &&
        target.b == replacement.b && target.a == replacement.a) {
        return;
    }
    if (!Matches(pixels, width, height, startX, startY, target)) {
        return;
    }

    std::queue<glm::ivec2> pending;
    pending.push(glm::ivec2(startX, startY));
    while (!pending.empty()) {
        glm::ivec2 p = pending.front();
        pending.pop();
        if (!Matches(pixels, width, height, p.x, p.y, target)) {
            continue;
        }
        SetPixel(pixels, width, height, p.x, p.y, replacement);
        pending.push(glm::ivec2(p.x + 1, p.y));
        pending.push(glm::ivec2(p.x - 1, p.y));
        pending.push(glm::ivec2(p.x, p.y + 1));
        pending.push(glm::ivec2(p.x, p.y - 1));
    }
}

void EnsureSpriteClipNames(std::vector<std::string>& names, size_t count) {
    if (names.size() < count) {
        for (size_t i = names.size(); i < count; ++i) {
            names.push_back("Rect_" + std::to_string(i));
        }
    } else if (names.size() > count) {
        names.resize(count);
    }
}

void EnsureSpriteClipScales(std::vector<glm::vec2>& scales, size_t count) {
    if (scales.size() < count) {
        scales.resize(count, glm::vec2(1.0f));
    } else if (scales.size() > count) {
        scales.resize(count);
    }
    for (glm::vec2& scale : scales) {
        scale.x = std::max(0.01f, scale.x);
        scale.y = std::max(0.01f, scale.y);
    }
}

size_t PixelBufferSize(int width, int height) {
    return static_cast<size_t>(std::max(1, width) * std::max(1, height) * 4);
}

PixelRgba GetPixel(const std::vector<unsigned char>& pixels, int width, int height, int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return {};
    }
    const size_t idx = static_cast<size_t>((y * width + x) * 4);
    return PixelRgba{
        pixels[idx + 0],
        pixels[idx + 1],
        pixels[idx + 2],
        pixels[idx + 3]
    };
}

PixelRgba AlphaComposite(const PixelRgba& dst, const PixelRgba& src) {
    const float srcA = src.a / 255.0f;
    const float dstA = dst.a / 255.0f;
    const float outA = srcA + dstA * (1.0f - srcA);
    if (outA <= 0.0f) {
        return PixelRgba{0, 0, 0, 0};
    }

    auto blendChannel = [&](unsigned char dstC, unsigned char srcC) -> unsigned char {
        const float srcV = srcC / 255.0f;
        const float dstV = dstC / 255.0f;
        const float outV = (srcV * srcA + dstV * dstA * (1.0f - srcA)) / outA;
        return static_cast<unsigned char>(std::clamp(outV, 0.0f, 1.0f) * 255.0f + 0.5f);
    };

    return PixelRgba{
        blendChannel(dst.r, src.r),
        blendChannel(dst.g, src.g),
        blendChannel(dst.b, src.b),
        static_cast<unsigned char>(std::clamp(outA, 0.0f, 1.0f) * 255.0f + 0.5f)
    };
}

void EnsureSpriteLayers(std::vector<PixelSpriteLayerState>& layers, int width, int height) {
    if (layers.empty()) {
        PixelSpriteLayerState layer;
        layer.name = "Layer_0";
        layer.visible = true;
        layer.pixels.assign(PixelBufferSize(width, height), 0);
        layers.push_back(std::move(layer));
    }
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].name.empty()) {
            layers[i].name = "Layer_" + std::to_string(i);
        }
        if (layers[i].pixels.size() != PixelBufferSize(width, height)) {
            layers[i].pixels.resize(PixelBufferSize(width, height), 0);
        }
    }
}

void RebuildCompositePixels(const std::vector<PixelSpriteLayerState>& layers,
                            int width,
                            int height,
                            std::vector<unsigned char>& outPixels) {
    outPixels.assign(PixelBufferSize(width, height), 0);
    for (const PixelSpriteLayerState& layer : layers) {
        if (!layer.visible || layer.pixels.size() != PixelBufferSize(width, height)) {
            continue;
        }
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const PixelRgba dst = GetPixel(outPixels, width, height, x, y);
                const PixelRgba src = GetPixel(layer.pixels, width, height, x, y);
                SetPixel(outPixels, width, height, x, y, AlphaComposite(dst, src));
            }
        }
    }
}

char ToHexNibble(int value) {
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + (value - 10)));
}

int FromHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string EncodePixelBufferHex(const std::vector<unsigned char>& pixels) {
    std::string encoded;
    encoded.resize(pixels.size() * 2);
    for (size_t i = 0; i < pixels.size(); ++i) {
        encoded[i * 2 + 0] = ToHexNibble((pixels[i] >> 4) & 0xF);
        encoded[i * 2 + 1] = ToHexNibble(pixels[i] & 0xF);
    }
    return encoded;
}

bool DecodePixelBufferHex(const std::string& encoded, std::vector<unsigned char>& outPixels) {
    if ((encoded.size() & 1u) != 0u) {
        return false;
    }
    outPixels.resize(encoded.size() / 2);
    for (size_t i = 0; i < outPixels.size(); ++i) {
        const int hi = FromHexNibble(encoded[i * 2 + 0]);
        const int lo = FromHexNibble(encoded[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            outPixels.clear();
            return false;
        }
        outPixels[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

ImU32 CheckerColor(bool darkTheme, bool oddCell) {
    if (darkTheme) {
        return oddCell ? IM_COL32(88, 88, 88, 255) : IM_COL32(58, 58, 58, 255);
    }
    return oddCell ? IM_COL32(214, 214, 214, 255) : IM_COL32(244, 244, 244, 255);
}

bool RectContainsPixel(const glm::ivec4& rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.z && y < rect.y + rect.w;
}

void PlotBrush(std::vector<unsigned char>& pixels, int width, int height, int x, int y, int brushSize, const PixelRgba& color) {
    const int clampedBrush = std::max(1, brushSize);
    const int offset = clampedBrush / 2;
    for (int oy = 0; oy < clampedBrush; ++oy) {
        for (int ox = 0; ox < clampedBrush; ++ox) {
            SetPixel(pixels, width, height, x + ox - offset, y + oy - offset, color);
        }
    }
}

void DrawLineStroke(std::vector<unsigned char>& pixels,
                    int width,
                    int height,
                    glm::ivec2 a,
                    glm::ivec2 b,
                    int brushSize,
                    const PixelRgba& color) {
    int x0 = a.x;
    int y0 = a.y;
    const int x1 = b.x;
    const int y1 = b.y;
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        PlotBrush(pixels, width, height, x0, y0, brushSize, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int err2 = err * 2;
        if (err2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (err2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void DrawRectangleStroke(std::vector<unsigned char>& pixels,
                         int width,
                         int height,
                         glm::ivec2 a,
                         glm::ivec2 b,
                         int brushSize,
                         const PixelRgba& color) {
    const glm::ivec4 rect = NormalizeRect(a, b);
    for (int x = rect.x; x < rect.x + rect.z; ++x) {
        PlotBrush(pixels, width, height, x, rect.y, brushSize, color);
        PlotBrush(pixels, width, height, x, rect.y + rect.w - 1, brushSize, color);
    }
    for (int y = rect.y; y < rect.y + rect.w; ++y) {
        PlotBrush(pixels, width, height, rect.x, y, brushSize, color);
        PlotBrush(pixels, width, height, rect.x + rect.z - 1, y, brushSize, color);
    }
}

void DrawEllipseStroke(std::vector<unsigned char>& pixels,
                       int width,
                       int height,
                       glm::ivec2 a,
                       glm::ivec2 b,
                       int brushSize,
                       const PixelRgba& color) {
    const glm::ivec4 rect = NormalizeRect(a, b);
    const float rx = std::max(0.5f, rect.z * 0.5f);
    const float ry = std::max(0.5f, rect.w * 0.5f);
    const float cx = rect.x + (rect.z - 1) * 0.5f;
    const float cy = rect.y + (rect.w - 1) * 0.5f;
    const int samples = std::max(24, static_cast<int>(std::ceil(std::max(rx, ry) * 10.0f)));
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        const float angle = t * 6.28318530718f;
        const int px = static_cast<int>(std::round(cx + std::cos(angle) * (rx - 0.5f)));
        const int py = static_cast<int>(std::round(cy + std::sin(angle) * (ry - 0.5f)));
        PlotBrush(pixels, width, height, px, py, brushSize, color);
    }
}

void DrawRoundedRectangleStroke(std::vector<unsigned char>& pixels,
                                int width,
                                int height,
                                glm::ivec2 a,
                                glm::ivec2 b,
                                int brushSize,
                                const PixelRgba& color) {
    const glm::ivec4 rect = NormalizeRect(a, b);
    if (rect.z <= 3 || rect.w <= 3) {
        DrawRectangleStroke(pixels, width, height, a, b, brushSize, color);
        return;
    }

    const int radius = std::clamp(std::min(rect.z, rect.w) / 4, 1, 8);
    for (int x = rect.x + radius; x < rect.x + rect.z - radius; ++x) {
        PlotBrush(pixels, width, height, x, rect.y, brushSize, color);
        PlotBrush(pixels, width, height, x, rect.y + rect.w - 1, brushSize, color);
    }
    for (int y = rect.y + radius; y < rect.y + rect.w - radius; ++y) {
        PlotBrush(pixels, width, height, rect.x, y, brushSize, color);
        PlotBrush(pixels, width, height, rect.x + rect.z - 1, y, brushSize, color);
    }

    const glm::vec2 centers[4] = {
        glm::vec2(rect.x + radius, rect.y + radius),
        glm::vec2(rect.x + rect.z - radius - 1, rect.y + radius),
        glm::vec2(rect.x + rect.z - radius - 1, rect.y + rect.w - radius - 1),
        glm::vec2(rect.x + radius, rect.y + rect.w - radius - 1)
    };
    const float angleStarts[4] = { 3.14159265359f, 4.71238898038f, 0.0f, 1.57079632679f };
    const int samples = std::max(6, radius * 4);
    for (int corner = 0; corner < 4; ++corner) {
        for (int i = 0; i <= samples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(samples);
            const float angle = angleStarts[corner] + t * 1.57079632679f;
            const int px = static_cast<int>(std::round(centers[corner].x + std::cos(angle) * radius));
            const int py = static_cast<int>(std::round(centers[corner].y + std::sin(angle) * radius));
            PlotBrush(pixels, width, height, px, py, brushSize, color);
        }
    }
}

glm::ivec4 FindConnectedRegionBounds(const std::vector<unsigned char>& pixels, int width, int height, int startX, int startY) {
    if (startX < 0 || startY < 0 || startX >= width || startY >= height) {
        return glm::ivec4(0, 0, 1, 1);
    }

    const size_t startIndex = static_cast<size_t>((startY * width + startX) * 4);
    const PixelRgba target{
        pixels[startIndex + 0],
        pixels[startIndex + 1],
        pixels[startIndex + 2],
        pixels[startIndex + 3]
    };

    std::queue<glm::ivec2> pending;
    std::vector<unsigned char> visited(static_cast<size_t>(width * height), 0);
    pending.push(glm::ivec2(startX, startY));
    visited[static_cast<size_t>(startY * width + startX)] = 1;

    int minX = startX;
    int minY = startY;
    int maxX = startX;
    int maxY = startY;

    while (!pending.empty()) {
        const glm::ivec2 p = pending.front();
        pending.pop();
        if (!Matches(pixels, width, height, p.x, p.y, target)) {
            continue;
        }
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);

        const glm::ivec2 neighbors[4] = {
            glm::ivec2(p.x + 1, p.y),
            glm::ivec2(p.x - 1, p.y),
            glm::ivec2(p.x, p.y + 1),
            glm::ivec2(p.x, p.y - 1)
        };
        for (const glm::ivec2& neighbor : neighbors) {
            if (neighbor.x < 0 || neighbor.y < 0 || neighbor.x >= width || neighbor.y >= height) {
                continue;
            }
            const size_t visitedIndex = static_cast<size_t>(neighbor.y * width + neighbor.x);
            if (visited[visitedIndex]) {
                continue;
            }
            visited[visitedIndex] = 1;
            pending.push(neighbor);
        }
    }

    return glm::ivec4(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

}

bool Engine::loadPixelSpriteDocument(const fs::path& imagePath) {
    if (!hasSpriteEditorPackage()) {
        addConsoleMessage("Pixel Sprite Editor is unavailable because moduengine.sprite-editor is not installed.",
                          ConsoleMessageType::Warning);
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* data = stbi_load(imagePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!data || width <= 0 || height <= 0) {
        if (data) stbi_image_free(data);
        addConsoleMessage("Failed to open sprite image: " + imagePath.string(), ConsoleMessageType::Error);
        return false;
    }

    pixelSpriteDocument = PixelSpriteDocument{};
    pixelSpriteDocument.imagePath = imagePath;
    pixelSpriteDocument.sidecarPath = imagePath;
    pixelSpriteDocument.sidecarPath += ".spritesheet";
    pixelSpriteDocument.name = imagePath.filename().string();
    pixelSpriteDocument.width = width;
    pixelSpriteDocument.height = height;
    pixelSpriteDocument.pixels.assign(data, data + static_cast<size_t>(width * height * 4));
    pixelSpriteDocument.loaded = true;
    stbi_image_free(data);

    if (fs::exists(pixelSpriteDocument.sidecarPath)) {
        std::ifstream sidecar(pixelSpriteDocument.sidecarPath);
        std::ostringstream buffer;
        buffer << sidecar.rdbuf();
        const SpritesheetParseResult parsed = ParseSpritesheet(buffer.str());
        pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher =
            parsed.document.expectedMinimumModuEngineVersionOrHigher;
        pixelSpriteDocument.strictValidation = parsed.document.strictValidation;
        pixelSpriteDocument.spriteFrames = parsed.document.rects;
        pixelSpriteDocument.spriteFrameNames = parsed.document.names;
        pixelSpriteDocument.spriteFrameScales = parsed.document.scales;
        bool hadDecodedLayerPixels = false;
        pixelSpriteDocument.layers.clear();
        for (const SpritesheetLayer& parsedLayer : parsed.document.layers) {
            PixelSpriteLayerState layer;
            layer.name = parsedLayer.name;
            layer.visible = parsedLayer.visible;
            if (!parsedLayer.pixelData.empty()) {
                std::vector<unsigned char> decoded;
                if (DecodePixelBufferHex(parsedLayer.pixelData, decoded) && decoded.size() == PixelBufferSize(width, height)) {
                    layer.pixels = std::move(decoded);
                    hadDecodedLayerPixels = true;
                }
            }
            pixelSpriteDocument.layers.push_back(std::move(layer));
        }
        if (!hadDecodedLayerPixels) {
            pixelSpriteDocument.layers.clear();
            PixelSpriteLayerState baseLayer;
            baseLayer.name = "Layer_0";
            baseLayer.visible = true;
            baseLayer.pixels = pixelSpriteDocument.pixels;
            pixelSpriteDocument.layers.push_back(std::move(baseLayer));
        }
        for (const SpritesheetParseMessage& message : parsed.messages) {
            addConsoleMessage(message.text, ConsoleMessageType::Warning);
        }
    } else {
        PixelSpriteLayerState baseLayer;
        baseLayer.name = "Layer_0";
        baseLayer.visible = true;
        baseLayer.pixels = pixelSpriteDocument.pixels;
        pixelSpriteDocument.layers.push_back(std::move(baseLayer));
    }
    EnsureSpriteClipNames(pixelSpriteDocument.spriteFrameNames, pixelSpriteDocument.spriteFrames.size());
    EnsureSpriteClipScales(pixelSpriteDocument.spriteFrameScales, pixelSpriteDocument.spriteFrames.size());
    EnsureSpriteLayers(pixelSpriteDocument.layers, width, height);
    RebuildCompositePixels(pixelSpriteDocument.layers, width, height, pixelSpriteDocument.pixels);

    pixelSpriteUndoStack.clear();
    pixelSpriteRedoStack.clear();
    PixelSpriteHistoryState initialState;
    initialState.label = "Open Image";
    initialState.width = pixelSpriteDocument.width;
    initialState.height = pixelSpriteDocument.height;
    initialState.pixels = pixelSpriteDocument.pixels;
    initialState.selectionActive = pixelSpriteDocument.selectionActive;
    initialState.selectionStart = pixelSpriteDocument.selectionStart;
    initialState.selectionEnd = pixelSpriteDocument.selectionEnd;
    initialState.expectedMinimumModuEngineVersionOrHigher = pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher;
    initialState.strictValidation = pixelSpriteDocument.strictValidation;
    initialState.spriteFrames = pixelSpriteDocument.spriteFrames;
    initialState.spriteFrameNames = pixelSpriteDocument.spriteFrameNames;
    initialState.spriteFrameScales = pixelSpriteDocument.spriteFrameScales;
    initialState.layers = pixelSpriteDocument.layers;
    initialState.activeLayer = pixelSpriteDocument.activeLayer;
    initialState.activeFrame = pixelSpriteDocument.activeFrame;
    pixelSpriteUndoStack.push_back(std::move(initialState));
    showPixelSpriteEditorWindow = true;
    pixelSpriteCanvasPan = ImVec2(0.0f, 0.0f);
    pixelSpriteCanvasTargetPan = ImVec2(0.0f, 0.0f);
    pixelSpriteCanvasStateInitialized = false;
    pixelSpriteCanvasCenterPending = true;
    pixelSpriteZoom = 12.0f;
    pixelSpriteTargetZoom = 12.0f;
    pixelSpriteFloatingSelectionActive = false;
    pixelSpriteFloatingSelectionPixels.clear();
    pixelSpriteFloatingSelectionSize = glm::ivec2(0);
    pixelSpriteFloatingSelectionPosition = glm::ivec2(0);
    pixelSpriteFloatingSelectionLayer = -1;
    pixelSpriteRecentColors.clear();
    saveEditorUserSettings();
    return true;
}

bool Engine::savePixelSpriteDocument() {
    if (!pixelSpriteDocument.loaded || pixelSpriteDocument.imagePath.empty()) {
        return false;
    }

    if (pixelSpriteFloatingSelectionActive &&
        pixelSpriteFloatingSelectionLayer >= 0 &&
        pixelSpriteFloatingSelectionLayer < static_cast<int>(pixelSpriteDocument.layers.size())) {
        PixelSpriteLayerState& targetLayer = pixelSpriteDocument.layers[pixelSpriteFloatingSelectionLayer];
        for (int y = 0; y < pixelSpriteFloatingSelectionSize.y; ++y) {
            for (int x = 0; x < pixelSpriteFloatingSelectionSize.x; ++x) {
                const PixelRgba color = GetPixel(pixelSpriteFloatingSelectionPixels,
                                                 pixelSpriteFloatingSelectionSize.x,
                                                 pixelSpriteFloatingSelectionSize.y,
                                                 x,
                                                 y);
                SetPixel(targetLayer.pixels,
                         pixelSpriteDocument.width,
                         pixelSpriteDocument.height,
                         pixelSpriteFloatingSelectionPosition.x + x,
                         pixelSpriteFloatingSelectionPosition.y + y,
                         color);
            }
        }
        pixelSpriteFloatingSelectionActive = false;
        pixelSpriteFloatingSelectionPixels.clear();
        pixelSpriteFloatingSelectionSize = glm::ivec2(0);
        pixelSpriteFloatingSelectionPosition = glm::ivec2(0);
        pixelSpriteFloatingSelectionLayer = -1;
    }

    EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);
    RebuildCompositePixels(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height, pixelSpriteDocument.pixels);

    fs::create_directories(pixelSpriteDocument.imagePath.parent_path());
    if (!stbi_write_png(pixelSpriteDocument.imagePath.string().c_str(),
                        pixelSpriteDocument.width,
                        pixelSpriteDocument.height,
                        4,
                        pixelSpriteDocument.pixels.data(),
                        pixelSpriteDocument.width * 4)) {
        addConsoleMessage("Failed to save sprite image: " + pixelSpriteDocument.imagePath.string(), ConsoleMessageType::Error);
        return false;
    }

    std::ofstream sidecar(pixelSpriteDocument.sidecarPath);
    if (sidecar.is_open()) {
        EnsureSpriteClipNames(pixelSpriteDocument.spriteFrameNames, pixelSpriteDocument.spriteFrames.size());
        EnsureSpriteClipScales(pixelSpriteDocument.spriteFrameScales, pixelSpriteDocument.spriteFrames.size());
        EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);
        SpritesheetDocument sidecarDocument;
        sidecarDocument.linkedSpriteName = pixelSpriteDocument.imagePath.lexically_relative(projectManager.currentProject.projectPath).generic_string();
        if (sidecarDocument.linkedSpriteName.empty() || sidecarDocument.linkedSpriteName == ".") {
            sidecarDocument.linkedSpriteName = pixelSpriteDocument.imagePath.generic_string();
        }
        sidecarDocument.spriteVersion = 1;
        sidecarDocument.expectedMinimumModuEngineVersionOrHigher =
            pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher.empty() ? "V6.7" : pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher;
        sidecarDocument.expectLayers = std::max(1, static_cast<int>(pixelSpriteDocument.layers.size()));
        sidecarDocument.expectRects = static_cast<int>(pixelSpriteDocument.spriteFrames.size());
        sidecarDocument.strictValidation = pixelSpriteDocument.strictValidation;
        sidecarDocument.rects = pixelSpriteDocument.spriteFrames;
        sidecarDocument.names = pixelSpriteDocument.spriteFrameNames;
        sidecarDocument.scales = pixelSpriteDocument.spriteFrameScales;
        sidecarDocument.layers.clear();
        sidecarDocument.layers.reserve(pixelSpriteDocument.layers.size());
        for (const PixelSpriteLayerState& layer : pixelSpriteDocument.layers) {
            SpritesheetLayer serializedLayer;
            serializedLayer.name = layer.name;
            serializedLayer.visible = layer.visible;
            serializedLayer.pixelData = EncodePixelBufferHex(layer.pixels);
            sidecarDocument.layers.push_back(std::move(serializedLayer));
        }
        sidecar << WriteSpritesheet(sidecarDocument);
    }

    renderer.invalidateTexture(pixelSpriteDocument.imagePath.string());
    if (vulkanRenderer) {
        vulkanRenderer->invalidateImagePath(pixelSpriteDocument.imagePath.string());
    }
    pixelSpriteDocument.dirty = false;
    projectManager.currentProject.hasUnsavedChanges = true;
    addConsoleMessage("Saved sprite image: " + pixelSpriteDocument.imagePath.string(), ConsoleMessageType::Success);
    return true;
}

void Engine::renderPixelSpriteEditorWindow() {
    if (!showPixelSpriteEditorWindow) return;
    if (!hasSpriteEditorPackage()) {
        showPixelSpriteEditorWindow = false;
        return;
    }

    if (!pixelSpriteDocument.loaded) {
        pixelSpriteDocument = PixelSpriteDocument{};
        pixelSpriteDocument.width = 16;
        pixelSpriteDocument.height = 16;
        pixelSpriteDocument.pixels.assign(static_cast<size_t>(16 * 16 * 4), 0);
        pixelSpriteDocument.loaded = true;
        EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);
        RebuildCompositePixels(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height, pixelSpriteDocument.pixels);
        pixelSpriteUndoStack.clear();
        pixelSpriteRedoStack.clear();
        PixelSpriteHistoryState initialState;
        initialState.label = "New Image";
        initialState.width = pixelSpriteDocument.width;
        initialState.height = pixelSpriteDocument.height;
        initialState.pixels = pixelSpriteDocument.pixels;
        initialState.selectionActive = pixelSpriteDocument.selectionActive;
        initialState.selectionStart = pixelSpriteDocument.selectionStart;
        initialState.selectionEnd = pixelSpriteDocument.selectionEnd;
        initialState.expectedMinimumModuEngineVersionOrHigher = pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher;
        initialState.strictValidation = pixelSpriteDocument.strictValidation;
        initialState.spriteFrames = pixelSpriteDocument.spriteFrames;
        initialState.spriteFrameNames = pixelSpriteDocument.spriteFrameNames;
        initialState.spriteFrameScales = pixelSpriteDocument.spriteFrameScales;
        initialState.layers = pixelSpriteDocument.layers;
        initialState.activeLayer = pixelSpriteDocument.activeLayer;
        initialState.activeFrame = pixelSpriteDocument.activeFrame;
        pixelSpriteUndoStack.push_back(std::move(initialState));
        pixelSpriteRecentColors.clear();
    }
    EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);
    RebuildCompositePixels(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height, pixelSpriteDocument.pixels);
    EnsureSpriteClipScales(pixelSpriteDocument.spriteFrameScales, pixelSpriteDocument.spriteFrames.size());
    pixelSpriteDocument.activeLayer = std::clamp(pixelSpriteDocument.activeLayer, 0, std::max(0, static_cast<int>(pixelSpriteDocument.layers.size()) - 1));
    pixelSpriteDocument.activeFrame = std::clamp(pixelSpriteDocument.activeFrame, 0, std::max(0, static_cast<int>(pixelSpriteDocument.spriteFrames.size()) - 1));

    if (mainDockspaceId != 0) {
        ImGui::SetNextWindowDockID(mainDockspaceId, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(Loc::Window("WINDOW_PIXEL_SPRITE_EDITOR", "Pixel Sprite Editor"), &showPixelSpriteEditorWindow, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    auto ensureProjectAssetPath = [&]() {
        if (!pixelSpriteDocument.imagePath.empty() || !projectManager.currentProject.isLoaded) {
            return;
        }
        fs::path base = projectManager.currentProject.projectPath / "Assets" / "Sprites";
        fs::create_directories(base);
        pixelSpriteDocument.imagePath = base / "sprite.png";
        pixelSpriteDocument.sidecarPath = pixelSpriteDocument.imagePath;
        pixelSpriteDocument.sidecarPath += ".spritesheet";
    };

    auto openPixelSpriteImagePicker = [&]() {
        if (!projectManager.currentProject.isLoaded) {
            addConsoleMessage("Please Load a project before opening images from project assets.", ConsoleMessageType::Warning);
            return;
        }

        fs::path assetRoot = projectManager.currentProject.projectPath / "Assets";
        if (!fs::exists(assetRoot) || !fs::is_directory(assetRoot)) {
            assetRoot = projectManager.currentProject.projectPath;
        }

        pixelSpriteOpenImageBrowser.setProjectRoot(assetRoot);
        pixelSpriteOpenImageBrowser.selectedFile.clear();
        pixelSpriteOpenImageBrowser.searchFilter.clear();
        pixelSpriteOpenImageSearch[0] = '\0';

        fs::path preferredDir = pixelSpriteDocument.imagePath.empty()
            ? assetRoot
            : pixelSpriteDocument.imagePath.parent_path();
        std::error_code preferredError;
        const fs::path canonicalRoot = fs::weakly_canonical(assetRoot, preferredError);
        const fs::path canonicalPreferred = fs::weakly_canonical(preferredDir, preferredError);
        const std::string canonicalRootText = canonicalRoot.generic_string();
        const std::string canonicalPreferredText = canonicalPreferred.generic_string();
        const bool preferredInsideProject =
            !canonicalRootText.empty() &&
            !canonicalPreferredText.empty() &&
            (canonicalPreferredText == canonicalRootText ||
             canonicalPreferredText.rfind(canonicalRootText + "/", 0) == 0);
        if (preferredInsideProject && fs::exists(preferredDir) && fs::is_directory(preferredDir)) {
            pixelSpriteOpenImageBrowser.currentPath = preferredDir;
            pixelSpriteOpenImageBrowser.needsRefresh = true;
        }

        pixelSpriteOpenImagePopupTrigger = true;
        pixelSpriteOpenImagePopupOpen = true;
    };

    auto pushHistory = [&](const char* label = "Edit") {
        PixelSpriteHistoryState state;
        state.label = (label && *label) ? label : "Edit";
        state.width = pixelSpriteDocument.width;
        state.height = pixelSpriteDocument.height;
        state.pixels = pixelSpriteDocument.pixels;
        state.selectionActive = pixelSpriteDocument.selectionActive;
        state.selectionStart = pixelSpriteDocument.selectionStart;
        state.selectionEnd = pixelSpriteDocument.selectionEnd;
        state.expectedMinimumModuEngineVersionOrHigher = pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher;
        state.strictValidation = pixelSpriteDocument.strictValidation;
        state.spriteFrames = pixelSpriteDocument.spriteFrames;
        state.spriteFrameNames = pixelSpriteDocument.spriteFrameNames;
        state.spriteFrameScales = pixelSpriteDocument.spriteFrameScales;
        state.layers = pixelSpriteDocument.layers;
        state.activeLayer = pixelSpriteDocument.activeLayer;
        state.activeFrame = pixelSpriteDocument.activeFrame;
        pixelSpriteUndoStack.push_back(std::move(state));
        if (pixelSpriteUndoStack.size() > 128) {
            pixelSpriteUndoStack.erase(pixelSpriteUndoStack.begin());
        }
        pixelSpriteRedoStack.clear();
    };

    auto commitHistoryTop = [&]() {
        if (!pixelSpriteUndoStack.empty()) {
            pixelSpriteUndoStack.back().width = pixelSpriteDocument.width;
            pixelSpriteUndoStack.back().height = pixelSpriteDocument.height;
            pixelSpriteUndoStack.back().pixels = pixelSpriteDocument.pixels;
            pixelSpriteUndoStack.back().selectionActive = pixelSpriteDocument.selectionActive;
            pixelSpriteUndoStack.back().selectionStart = pixelSpriteDocument.selectionStart;
            pixelSpriteUndoStack.back().selectionEnd = pixelSpriteDocument.selectionEnd;
            pixelSpriteUndoStack.back().expectedMinimumModuEngineVersionOrHigher = pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher;
            pixelSpriteUndoStack.back().strictValidation = pixelSpriteDocument.strictValidation;
            pixelSpriteUndoStack.back().spriteFrames = pixelSpriteDocument.spriteFrames;
            pixelSpriteUndoStack.back().spriteFrameNames = pixelSpriteDocument.spriteFrameNames;
            pixelSpriteUndoStack.back().spriteFrameScales = pixelSpriteDocument.spriteFrameScales;
            pixelSpriteUndoStack.back().layers = pixelSpriteDocument.layers;
            pixelSpriteUndoStack.back().activeLayer = pixelSpriteDocument.activeLayer;
            pixelSpriteUndoStack.back().activeFrame = pixelSpriteDocument.activeFrame;
        }
    };

    auto undoHistory = [&]() {
        if (pixelSpriteUndoStack.size() <= 1) return;
        pixelSpriteRedoStack.push_back(pixelSpriteUndoStack.back());
        pixelSpriteUndoStack.pop_back();
        const PixelSpriteHistoryState& state = pixelSpriteUndoStack.back();
        pixelSpriteDocument.width = state.width;
        pixelSpriteDocument.height = state.height;
        pixelSpriteDocument.pixels = state.pixels;
        pixelSpriteDocument.selectionActive = state.selectionActive;
        pixelSpriteDocument.selectionStart = state.selectionStart;
        pixelSpriteDocument.selectionEnd = state.selectionEnd;
        pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher = state.expectedMinimumModuEngineVersionOrHigher;
        pixelSpriteDocument.strictValidation = state.strictValidation;
        pixelSpriteDocument.spriteFrames = state.spriteFrames;
        pixelSpriteDocument.spriteFrameNames = state.spriteFrameNames;
        pixelSpriteDocument.spriteFrameScales = state.spriteFrameScales;
        pixelSpriteDocument.layers = state.layers;
        EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);
        RebuildCompositePixels(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height, pixelSpriteDocument.pixels);
        pixelSpriteDocument.activeLayer = std::clamp(state.activeLayer, 0, std::max(0, static_cast<int>(pixelSpriteDocument.layers.size()) - 1));
        pixelSpriteDocument.activeFrame = std::clamp(state.activeFrame, 0, std::max(0, static_cast<int>(state.spriteFrames.size()) - 1));
        pixelSpriteFloatingSelectionActive = false;
        pixelSpriteFloatingSelectionPixels.clear();
        pixelSpriteFloatingSelectionSize = glm::ivec2(0);
        pixelSpriteFloatingSelectionPosition = glm::ivec2(0);
        pixelSpriteFloatingSelectionLayer = -1;
        pixelSpriteDocument.dirty = true;
    };

    auto redoHistory = [&]() {
        if (pixelSpriteRedoStack.empty()) return;
        pixelSpriteUndoStack.push_back(pixelSpriteRedoStack.back());
        pixelSpriteRedoStack.pop_back();
        const PixelSpriteHistoryState& state = pixelSpriteUndoStack.back();
        pixelSpriteDocument.width = state.width;
        pixelSpriteDocument.height = state.height;
        pixelSpriteDocument.pixels = state.pixels;
        pixelSpriteDocument.selectionActive = state.selectionActive;
        pixelSpriteDocument.selectionStart = state.selectionStart;
        pixelSpriteDocument.selectionEnd = state.selectionEnd;
        pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher = state.expectedMinimumModuEngineVersionOrHigher;
        pixelSpriteDocument.strictValidation = state.strictValidation;
        pixelSpriteDocument.spriteFrames = state.spriteFrames;
        pixelSpriteDocument.spriteFrameNames = state.spriteFrameNames;
        pixelSpriteDocument.spriteFrameScales = state.spriteFrameScales;
        pixelSpriteDocument.layers = state.layers;
        EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);
        RebuildCompositePixels(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height, pixelSpriteDocument.pixels);
        pixelSpriteDocument.activeLayer = std::clamp(state.activeLayer, 0, std::max(0, static_cast<int>(pixelSpriteDocument.layers.size()) - 1));
        pixelSpriteDocument.activeFrame = std::clamp(state.activeFrame, 0, std::max(0, static_cast<int>(state.spriteFrames.size()) - 1));
        pixelSpriteFloatingSelectionActive = false;
        pixelSpriteFloatingSelectionPixels.clear();
        pixelSpriteFloatingSelectionSize = glm::ivec2(0);
        pixelSpriteFloatingSelectionPosition = glm::ivec2(0);
        pixelSpriteFloatingSelectionLayer = -1;
        pixelSpriteDocument.dirty = true;
    };

    auto applyDocToSelectedSprite = [&]() {
        SceneObject* selected = getSelectedObject();
        if (!selected || !selected->hasUI ||
            (selected->ui.type != UIElementType::Sprite2D && selected->ui.type != UIElementType::Image)) {
            addConsoleMessage("Select a Sprite2D, Sprite 2.5D, or UI Image object to apply sprite clips.", ConsoleMessageType::Warning);
            return;
        }
        ensureProjectAssetPath();
        if (pixelSpriteDocument.imagePath.empty()) {
            return;
        }
        if (pixelSpriteDocument.dirty) {
            savePixelSpriteDocument();
        }
        selected->albedoTexturePath = pixelSpriteDocument.imagePath.string();
        selected->ui.spriteSheetEnabled = true;
        selected->ui.spriteCustomFramesEnabled = !pixelSpriteDocument.spriteFrames.empty();
        selected->ui.spriteSourceWidth = pixelSpriteDocument.width;
        selected->ui.spriteSourceHeight = pixelSpriteDocument.height;
        selected->ui.spriteCustomFrames = pixelSpriteDocument.spriteFrames;
        selected->ui.spriteCustomFrameNames = pixelSpriteDocument.spriteFrameNames;
        selected->ui.spriteCustomFrameScales = pixelSpriteDocument.spriteFrameScales;
        if (!pixelSpriteDocument.spriteFrames.empty()) {
            selected->ui.size.x = static_cast<float>(pixelSpriteDocument.spriteFrames[0].z);
            selected->ui.size.y = static_cast<float>(pixelSpriteDocument.spriteFrames[0].w);
        } else {
            selected->ui.size.x = static_cast<float>(pixelSpriteDocument.width);
            selected->ui.size.y = static_cast<float>(pixelSpriteDocument.height);
        }
        projectManager.currentProject.hasUnsavedChanges = true;
        addConsoleMessage("Applied sprite clips to: " + selected->name, ConsoleMessageType::Success);
    };

    const bool editorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (editorFocused && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        undoHistory();
    }
    if (editorFocused && ((ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ||
        (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))) {
        redoHistory();
    }

    static bool shapeDragActive = false;
    static glm::ivec2 shapeDragStart(0);
    static glm::ivec2 shapeDragCurrent(0);
    static PixelSpriteTool shapeDragTool = PixelSpriteTool::Pencil;
    static bool selectionMoveDrag = false;
    static glm::ivec2 selectionMoveAnchor(0);
    static glm::ivec4 selectionMoveOrigin(0, 0, 1, 1);

    struct PixelSpriteToolDefinition {
        PixelSpriteTool tool;
        const char* label;
        const char* tooltip;
        const char* iconPath;
    };

    constexpr std::array<PixelSpriteToolDefinition, 12> toolDefinitions = {{
        { PixelSpriteTool::Pencil, "Pencil", "Draw pixels", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Pencil.png" },
        { PixelSpriteTool::Eraser, "Eraser", "Erase pixels", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Eraser.png" },
        { PixelSpriteTool::Bucket, "Bucket", "Fill a contiguous area", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Paint Bucket.png" },
        { PixelSpriteTool::ColorPicker, "Color Picker", "Sample a color from the canvas", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Color Picker.png" },
        { PixelSpriteTool::MagicSelect, "Magic Select", "Select the clicked color region", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Auto Select by Area and Color.png" },
        { PixelSpriteTool::Lasso, "Lasso", "Create a freeform selection workflow", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Lasso Selection.png" },
        { PixelSpriteTool::LineCurve, "Line / Curve", "Draw a line between two points", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Line and Curve Draw.png" },
        { PixelSpriteTool::MoveSelectedArea, "Move Selection", "Move the current selection bounds", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Move Selected Area.png" },
        { PixelSpriteTool::Rectangle, "Rectangle", "Draw a rectangle outline", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Rectangle shape.png" },
        { PixelSpriteTool::RoundedRectangle, "Rounded Rectangle", "Draw a rounded rectangle outline", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Rounded Rectangle Shape.png" },
        { PixelSpriteTool::Circle, "Circle", "Draw an ellipse outline", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Curcle Shape.png" },
        { PixelSpriteTool::SelectArea, "Select Area", "Create a rectangular selection", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Select Area.png" }
    }};

    auto pixelSpriteToolLabel = [&](PixelSpriteTool tool) -> const char* {
        for (const PixelSpriteToolDefinition& def : toolDefinitions) {
            if (def.tool == tool) {
                return def.label;
            }
        }
        return "Tool";
    };

    auto colorsNearEqual = [](const glm::vec4& a, const glm::vec4& b) {
        return std::abs(a.r - b.r) <= 0.001f &&
               std::abs(a.g - b.g) <= 0.001f &&
               std::abs(a.b - b.b) <= 0.001f &&
               std::abs(a.a - b.a) <= 0.001f;
    };

    auto appendRecentColor = [&](const glm::vec4& color) {
        auto existing = std::find_if(pixelSpriteRecentColors.begin(),
                                     pixelSpriteRecentColors.end(),
                                     [&](const glm::vec4& value) {
                                         return colorsNearEqual(value, color);
                                     });
        if (existing != pixelSpriteRecentColors.end()) {
            pixelSpriteRecentColors.erase(existing);
        }
        pixelSpriteRecentColors.insert(pixelSpriteRecentColors.begin(), color);
        if (pixelSpriteRecentColors.size() > 10) {
            pixelSpriteRecentColors.resize(10);
        }
    };

    auto setPrimaryColor = [&](const glm::vec4& color, bool trackRecent = true) {
        pixelSpritePrimaryColor = color;
        if (trackRecent) {
            appendRecentColor(color);
        }
    };

    auto setSecondaryColor = [&](const glm::vec4& color, bool trackRecent = true) {
        pixelSpriteSecondaryColor = color;
        if (trackRecent) {
            appendRecentColor(color);
        }
    };

    if (pixelSpriteRecentColors.empty()) {
        appendRecentColor(pixelSpritePrimaryColor);
        appendRecentColor(pixelSpriteSecondaryColor);
    }

    auto pixelSpriteToolUsesBrush = [&](PixelSpriteTool tool) {
        switch (tool) {
            case PixelSpriteTool::Pencil:
            case PixelSpriteTool::Eraser:
            case PixelSpriteTool::LineCurve:
            case PixelSpriteTool::Rectangle:
            case PixelSpriteTool::RoundedRectangle:
            case PixelSpriteTool::Circle:
                return true;
            default:
                return false;
        }
    };

    auto pixelSpriteToolIsSelectionLike = [&](PixelSpriteTool tool) {
        switch (tool) {
            case PixelSpriteTool::MagicSelect:
            case PixelSpriteTool::Lasso:
            case PixelSpriteTool::MoveSelectedArea:
            case PixelSpriteTool::SelectArea:
                return true;
            default:
                return false;
        }
    };

    auto pixelSpriteToolIsShape = [&](PixelSpriteTool tool) {
        switch (tool) {
            case PixelSpriteTool::LineCurve:
            case PixelSpriteTool::Rectangle:
            case PixelSpriteTool::RoundedRectangle:
            case PixelSpriteTool::Circle:
                return true;
            default:
                return false;
        }
    };

    char pathBuf[512];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s", pixelSpriteDocument.imagePath.string().c_str());

    struct PixelSpriteUiIcon {
        ImTextureID id = static_cast<ImTextureID>(0);
        bool flipY = false;
    };

    const bool hasVulkanUiImages = usingVulkan() && vulkanRendererInitialized && (vulkanRenderer != nullptr);
    auto resolveUiIcon = [&](const char* iconPath) -> PixelSpriteUiIcon {
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

    auto drawIconButton = [&](const char* id,
                              const char* iconPath,
                              const char* fallbackText,
                              const char* tooltip,
                              bool active,
                              bool disabled,
                              ImVec2 size,
                              bool animatedSelection = false) -> bool {
        if (disabled) {
            ImGui::BeginDisabled();
        }

        ImGui::PushID(id);
        float* activeAnimRef = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("##PixelSpriteIconAnim"), 0.0f);
        ImGui::PopID();
        const float targetAnim = active ? 1.0f : 0.0f;
        const float animLerp = std::clamp(ImGui::GetIO().DeltaTime * (active ? 11.0f : 18.0f), 0.0f, 1.0f);
        *activeAnimRef = ImLerp(*activeAnimRef, targetAnim, animLerp);
        const float activeAnim = animatedSelection ? *activeAnimRef : (active ? 1.0f : 0.0f);
        const float pulse = (animatedSelection && active && activeAnim > 0.001f)
            ? std::sin(static_cast<float>(ImGui::GetTime()) * 7.5f)
            : 0.0f;
        const float brighten = std::max(0.0f, pulse) * activeAnim;
        const float darken = std::max(0.0f, -pulse) * activeAnim;

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, size);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 max(pos.x + size.x, pos.y + size.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float rounding = size.x >= 40.0f ? 8.0f : 5.0f;
        const float expand = animatedSelection
            ? activeAnim * (0.7f + brighten * 1.2f - darken * 0.45f)
            : 0.0f;
        const float inset = std::max(1.5f, 2.5f - expand * 0.9f);
        const ImVec2 frameMin(pos.x + inset, pos.y + inset);
        const ImVec2 frameMax(max.x - inset, max.y - inset);

        ImVec4 bg = active
            ? ImVec4(0.36f, 0.21f, 0.64f, disabled ? 0.45f : 0.92f)
            : hovered
                ? ImVec4(0.23f, 0.14f, 0.39f, disabled ? 0.35f : 0.82f)
                : ImVec4(0.09f, 0.10f, 0.16f, disabled ? 0.30f : 0.74f);
        ImVec4 border = active
            ? ImVec4(0.83f, 0.71f, 1.0f, disabled ? 0.52f : 1.0f)
            : hovered
                ? ImVec4(0.50f, 0.40f, 0.72f, disabled ? 0.40f : 0.86f)
                : ImVec4(0.26f, 0.28f, 0.40f, disabled ? 0.32f : 0.66f);
        if (activeAnim > 0.001f) {
            bg = ImVec4(0.34f + brighten * 0.08f - darken * 0.03f,
                        0.20f + brighten * 0.05f - darken * 0.02f,
                        0.60f + brighten * 0.10f - darken * 0.04f,
                        disabled ? 0.45f : 0.93f);
            border = ImVec4(0.78f + brighten * 0.10f,
                            0.64f + brighten * 0.06f,
                            1.0f,
                            disabled ? 0.52f : 1.0f);
        }
        if (animatedSelection && activeAnim > 0.05f && active) {
            drawList->AddRectFilled(
                ImVec2(frameMin.x + 1.0f, frameMin.y + 2.0f),
                ImVec2(frameMax.x + 1.0f, frameMax.y + 2.0f),
                ImGui::GetColorU32(ImVec4(0.10f, 0.05f, 0.18f, disabled ? 0.08f : (0.18f + brighten * 0.08f))),
                rounding);
        }
        drawList->AddRectFilled(frameMin, frameMax, ImGui::GetColorU32(bg), rounding);
        drawList->AddRect(frameMin, frameMax, ImGui::GetColorU32(border), rounding, 0, 1.0f + activeAnim);

        const PixelSpriteUiIcon icon = resolveUiIcon(iconPath);
        const float iconInset = size.x >= 40.0f ? 8.0f : 5.0f;
        const float iconGrow = animatedSelection
            ? activeAnim * (1.0f + brighten * 1.1f - darken * 0.35f)
            : 0.0f;
        const ImVec2 iconMin(frameMin.x + iconInset - iconGrow * 0.35f, frameMin.y + iconInset - iconGrow * 0.35f);
        const ImVec2 iconMax(frameMax.x - iconInset + iconGrow * 0.35f, frameMax.y - iconInset + iconGrow * 0.35f);
        const int alpha = disabled ? 104 : active ? 255 : hovered ? 240 : 214;
        if (icon.id != static_cast<ImTextureID>(0)) {
            const ImVec2 uvMin = icon.flipY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
            const ImVec2 uvMax = icon.flipY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
            drawList->AddImage(icon.id, iconMin, iconMax, uvMin, uvMax, IM_COL32(255, 255, 255, alpha));
        } else if (fallbackText && *fallbackText) {
            const ImVec2 textSize = ImGui::CalcTextSize(fallbackText);
            drawList->AddText(
                ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + (size.y - textSize.y) * 0.5f),
                IM_COL32(255, 255, 255, alpha),
                fallbackText);
        }

        if (hovered && tooltip && *tooltip) {
            ImGui::SetTooltip("%s", tooltip);
        }

        if (disabled) {
            ImGui::EndDisabled();
        }
        return !disabled && pressed;
    };

    auto beginRectSelection = [&](int x, int y) {
        bool clickedExistingClip = false;
        if (pixelSpriteEditorMode == PixelSpriteEditorMode::SpriteSheet) {
            for (size_t i = 0; i < pixelSpriteDocument.spriteFrames.size(); ++i) {
                const glm::ivec4& rect = pixelSpriteDocument.spriteFrames[i];
                if (RectContainsPixel(rect, x, y)) {
                    pixelSpriteDocument.activeFrame = static_cast<int>(i);
                    pixelSpriteDocument.selectionActive = true;
                    pixelSpriteDocument.selectionStart = glm::ivec2(rect.x, rect.y);
                    pixelSpriteDocument.selectionEnd = glm::ivec2(rect.x + rect.z - 1, rect.y + rect.w - 1);
                    clickedExistingClip = true;
                    break;
                }
            }
        }
        if (!clickedExistingClip) {
            const char* selectionLabel = pixelSpriteTool == PixelSpriteTool::Lasso ? "Lasso Select" : "Rectangle Select";
            pushHistory(selectionLabel);
            pixelSpriteDocument.selectionActive = true;
            pixelSpriteDocument.selectionStart = glm::ivec2(x, y);
            pixelSpriteDocument.selectionEnd = glm::ivec2(x, y);
            commitHistoryTop();
        }
    };

    auto resetFloatingSelection = [&]() {
        pixelSpriteFloatingSelectionActive = false;
        pixelSpriteFloatingSelectionPixels.clear();
        pixelSpriteFloatingSelectionSize = glm::ivec2(0);
        pixelSpriteFloatingSelectionPosition = glm::ivec2(0);
        pixelSpriteFloatingSelectionLayer = -1;
    };

    auto rebuildComposite = [&]() {
        EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);
        RebuildCompositePixels(pixelSpriteDocument.layers,
                               pixelSpriteDocument.width,
                               pixelSpriteDocument.height,
                               pixelSpriteDocument.pixels);
    };

    auto getActiveLayer = [&]() -> PixelSpriteLayerState* {
        if (pixelSpriteDocument.activeLayer < 0 ||
            pixelSpriteDocument.activeLayer >= static_cast<int>(pixelSpriteDocument.layers.size())) {
            return nullptr;
        }
        return &pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer];
    };

    auto commitFloatingSelection = [&](bool markDirty) -> bool {
        if (!pixelSpriteFloatingSelectionActive ||
            pixelSpriteFloatingSelectionLayer < 0 ||
            pixelSpriteFloatingSelectionLayer >= static_cast<int>(pixelSpriteDocument.layers.size())) {
            return false;
        }
        PixelSpriteLayerState& targetLayer = pixelSpriteDocument.layers[pixelSpriteFloatingSelectionLayer];
        for (int y = 0; y < pixelSpriteFloatingSelectionSize.y; ++y) {
            for (int x = 0; x < pixelSpriteFloatingSelectionSize.x; ++x) {
                const PixelRgba color = GetPixel(pixelSpriteFloatingSelectionPixels,
                                                 pixelSpriteFloatingSelectionSize.x,
                                                 pixelSpriteFloatingSelectionSize.y,
                                                 x,
                                                 y);
                SetPixel(targetLayer.pixels,
                         pixelSpriteDocument.width,
                         pixelSpriteDocument.height,
                         pixelSpriteFloatingSelectionPosition.x + x,
                         pixelSpriteFloatingSelectionPosition.y + y,
                         color);
            }
        }
        pixelSpriteDocument.selectionActive = true;
        pixelSpriteDocument.selectionStart = pixelSpriteFloatingSelectionPosition;
        pixelSpriteDocument.selectionEnd = glm::ivec2(
            pixelSpriteFloatingSelectionPosition.x + pixelSpriteFloatingSelectionSize.x - 1,
            pixelSpriteFloatingSelectionPosition.y + pixelSpriteFloatingSelectionSize.y - 1);
        resetFloatingSelection();
        rebuildComposite();
        if (markDirty) {
            pixelSpriteDocument.dirty = true;
        }
        return true;
    };

    auto beginFloatingSelectionMove = [&]() -> bool {
        PixelSpriteLayerState* activeLayer = getActiveLayer();
        if (!activeLayer || !pixelSpriteDocument.selectionActive) {
            return false;
        }
        const glm::ivec4 rect = NormalizeRect(pixelSpriteDocument.selectionStart, pixelSpriteDocument.selectionEnd);
        if (rect.z <= 0 || rect.w <= 0) {
            return false;
        }

        pixelSpriteFloatingSelectionPixels.assign(PixelBufferSize(rect.z, rect.w), 0);
        pixelSpriteFloatingSelectionSize = glm::ivec2(rect.z, rect.w);
        pixelSpriteFloatingSelectionPosition = glm::ivec2(rect.x, rect.y);
        pixelSpriteFloatingSelectionLayer = pixelSpriteDocument.activeLayer;
        for (int y = 0; y < rect.w; ++y) {
            for (int x = 0; x < rect.z; ++x) {
                const PixelRgba sampled = GetPixel(activeLayer->pixels,
                                                   pixelSpriteDocument.width,
                                                   pixelSpriteDocument.height,
                                                   rect.x + x,
                                                   rect.y + y);
                SetPixel(pixelSpriteFloatingSelectionPixels, rect.z, rect.w, x, y, sampled);
                SetPixel(activeLayer->pixels,
                         pixelSpriteDocument.width,
                         pixelSpriteDocument.height,
                         rect.x + x,
                         rect.y + y,
                         PixelRgba{0, 0, 0, 0});
            }
        }
        pixelSpriteFloatingSelectionActive = true;
        rebuildComposite();
        pixelSpriteDocument.dirty = true;
        return true;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.10f, 0.16f, 0.96f));
    if (ImGui::BeginChild("PixelSpriteTopBar", ImVec2(0.0f, 84.0f), true)) {
        auto playToolbarTick = [&]() {
            playEditorFeedbackPreview("Resources/Sounds/Selection Tick Main Editor.mp3", 0.95f, false, EditorFeedbackSoundCategory::Click);
        };
        const float sectionSpacing = 12.0f;
        const float totalWidth = ImGui::GetContentRegionAvail().x;
        const float actionsWidth = 228.0f;
        const float viewWidth = 360.0f;
        const float docWidth = std::max(240.0f, totalWidth - actionsWidth - viewWidth - sectionSpacing * 2.0f);

        ImGui::BeginChild("PixelSpriteTopBarDocument", ImVec2(docWidth, 0.0f), false);
        ImGui::TextDisabled("Document");
        ImGui::SameLine();
        ImGui::Text("%s%s", pixelSpriteDocument.name.c_str(), pixelSpriteDocument.dirty ? " *" : "");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##PixelSpriteImagePath", pathBuf, sizeof(pathBuf))) {
            pixelSpriteDocument.imagePath = fs::path(pathBuf);
            pixelSpriteDocument.sidecarPath = pixelSpriteDocument.imagePath;
            pixelSpriteDocument.sidecarPath += ".spritesheet";
            pixelSpriteDocument.name = pixelSpriteDocument.imagePath.filename().string();
        }
        ImGui::TextDisabled("%d x %d px", pixelSpriteDocument.width, pixelSpriteDocument.height);
        if (pixelSpriteDocument.selectionActive) {
            const glm::ivec4 selectionRect = NormalizeRect(pixelSpriteDocument.selectionStart, pixelSpriteDocument.selectionEnd);
            ImGui::SameLine();
            ImGui::TextDisabled("| Selection %d x %d", selectionRect.z, selectionRect.w);
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, sectionSpacing);
        ImGui::BeginChild("PixelSpriteTopBarActions", ImVec2(actionsWidth, 0.0f), false);
        ImGui::TextDisabled("Actions");
        if (drawIconButton("##PixelSpriteLoad", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Load Sprite.png", "O", "Open image from project assets", false, !projectManager.currentProject.isLoaded, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            openPixelSpriteImagePicker();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (drawIconButton("##PixelSpriteSave", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Save.png", "S", "Save sprite", false, false, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            ensureProjectAssetPath();
            savePixelSpriteDocument();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (drawIconButton("##PixelSpriteUndo", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Undo.png", "U", "Undo", false, pixelSpriteUndoStack.size() <= 1, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            undoHistory();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (drawIconButton("##PixelSpriteRedo", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Redo.png", "R", "Redo", false, pixelSpriteRedoStack.empty(), ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            redoHistory();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (drawIconButton("##PixelSpriteApply", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Apply to Object.png", "A", "Apply sprite clips to selected object", false, false, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            applyDocToSelectedSprite();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, sectionSpacing);
        ImGui::BeginChild("PixelSpriteTopBarView", ImVec2(0.0f, 0.0f), false);
        ImGui::TextDisabled("View");
        if (drawIconButton("##PixelSpriteModeEdit", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Edit Mode.png", "E", "Edit mode", pixelSpriteEditorMode == PixelSpriteEditorMode::Edit, false, ImVec2(32.0f, 32.0f), true)) {
            if (pixelSpriteEditorMode != PixelSpriteEditorMode::Edit) {
                playToolbarTick();
                pixelSpriteEditorMode = PixelSpriteEditorMode::Edit;
            }
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (drawIconButton("##PixelSpriteModeSheet", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Spritesheet Mode.png", "SS", "Spritesheet mode", pixelSpriteEditorMode == PixelSpriteEditorMode::SpriteSheet, false, ImVec2(32.0f, 32.0f), true)) {
            if (pixelSpriteEditorMode != PixelSpriteEditorMode::SpriteSheet) {
                playToolbarTick();
                pixelSpriteEditorMode = PixelSpriteEditorMode::SpriteSheet;
                if (!pixelSpriteToolIsSelectionLike(pixelSpriteTool)) {
                    pixelSpriteTool = PixelSpriteTool::SelectArea;
                }
            }
        }
        ImGui::SameLine(0.0f, 10.0f);
        if (drawIconButton("##PixelSpriteZoomOut", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Magnifier Zoom Out.png", "-", "Zoom out", false, false, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            pixelSpriteTargetZoom = pixelSpritePixelPerfect
                ? std::max(1.0f, std::round(pixelSpriteTargetZoom) - 1.0f)
                : std::max(1.0f, pixelSpriteTargetZoom / 1.12f);
        }
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::DragFloat("##PixelSpriteTopZoom", &pixelSpriteTargetZoom, pixelSpritePixelPerfect ? 1.0f : 0.25f, 1.0f, 128.0f, pixelSpritePixelPerfect ? "%.0fx" : "%.1fx")) {
            if (pixelSpritePixelPerfect) {
                pixelSpriteTargetZoom = std::round(pixelSpriteTargetZoom);
            }
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (drawIconButton("##PixelSpriteZoomIn", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Magnifier Zoom In.png", "+", "Zoom in", false, false, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            pixelSpriteTargetZoom = pixelSpritePixelPerfect
                ? std::min(128.0f, std::round(pixelSpriteTargetZoom) + 1.0f)
                : std::min(128.0f, pixelSpriteTargetZoom * 1.12f);
        }
        ImGui::SameLine(0.0f, 10.0f);
        if (drawIconButton("##PixelSpriteGridToggle", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Pixel Grid.png", "G", "Toggle pixel grid", pixelSpriteShowGrid, false, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            pixelSpriteShowGrid = !pixelSpriteShowGrid;
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (drawIconButton("##PixelSpritePixelPerfect", "Resources/Engine-Root/Image Editor and Spritesheet Editor/Pixel Perfect.png", "PP", "Toggle pixel perfect zoom", pixelSpritePixelPerfect, false, ImVec2(32.0f, 32.0f), true)) {
            playToolbarTick();
            pixelSpritePixelPerfect = !pixelSpritePixelPerfect;
            pixelSpriteTargetZoom = std::clamp(pixelSpriteTargetZoom, 1.0f, 128.0f);
            if (pixelSpritePixelPerfect) {
                pixelSpriteTargetZoom = std::round(pixelSpriteTargetZoom);
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const float zoomBlend = std::clamp(deltaTime * 14.0f, 0.0f, 1.0f);
    if (pixelSpritePixelPerfect) {
        pixelSpriteTargetZoom = std::round(std::clamp(pixelSpriteTargetZoom, 1.0f, 128.0f));
    } else {
        pixelSpriteTargetZoom = std::clamp(pixelSpriteTargetZoom, 1.0f, 128.0f);
    }
    pixelSpriteZoom += (pixelSpriteTargetZoom - pixelSpriteZoom) * zoomBlend;
    if (pixelSpritePixelPerfect) {
        pixelSpriteZoom = std::round(pixelSpriteZoom);
    }

    const float leftToolbarWidth = 44.0f;
    const float bodySpacing = 10.0f;
    const float rightPanelWidth = pixelSpriteRightPanelCollapsed ? 30.0f : 300.0f;
    const float canvasWidth = std::max(180.0f, ImGui::GetContentRegionAvail().x - leftToolbarWidth - rightPanelWidth - bodySpacing * 2.0f);

    constexpr float toolbarButtonSize = 30.0f;
    constexpr float toolbarButtonGap = 4.0f;
    constexpr float toolbarSeparatorHeight = 8.0f;
    constexpr int toolbarSeparatorCount = 3;
    const float toolbarContentHeight =
        toolDefinitions.size() * toolbarButtonSize +
        (toolDefinitions.size() - 1) * toolbarButtonGap +
        toolbarSeparatorCount * toolbarSeparatorHeight;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("PixelSpriteLeftToolbar", ImVec2(leftToolbarWidth, 0.0f), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY(std::max(8.0f, (ImGui::GetContentRegionAvail().y - toolbarContentHeight) * 0.5f));
    for (size_t i = 0; i < toolDefinitions.size(); ++i) {
        if (i == 4 || i == 8 || i == 11) {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            const ImVec2 sepMin = ImGui::GetCursorScreenPos();
            const float sepWidth = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(sepMin.x + 10.0f, sepMin.y),
                ImVec2(sepMin.x + sepWidth - 10.0f, sepMin.y),
                IM_COL32(122, 110, 158, 96),
                1.0f);
            ImGui::Dummy(ImVec2(0.0f, toolbarSeparatorHeight - 2.0f));
        }
        const PixelSpriteToolDefinition& def = toolDefinitions[i];
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - toolbarButtonSize) * 0.5f));
        if (drawIconButton(def.label, def.iconPath, def.label, def.tooltip, pixelSpriteTool == def.tool, false, ImVec2(toolbarButtonSize, toolbarButtonSize))) {
            if (pixelSpriteFloatingSelectionActive && def.tool != PixelSpriteTool::MoveSelectedArea) {
                commitFloatingSelection(true);
            }
            pixelSpriteTool = def.tool;
            shapeDragActive = false;
            selectionMoveDrag = false;
        }
        if (i + 1 < toolDefinitions.size()) {
            ImGui::Dummy(ImVec2(0.0f, toolbarButtonGap));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, bodySpacing);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.06f, 0.11f, 1.0f));
    ImGui::BeginChild("PixelSpriteCanvasArea",
                      ImVec2(canvasWidth, 0.0f),
                      true,
                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    const ImVec2 imageSize(
        pixelSpriteDocument.width * pixelSpriteZoom,
        pixelSpriteDocument.height * pixelSpriteZoom);
    const ImVec2 childWindowPos = ImGui::GetWindowPos();
    const ImVec2 childContentMin = ImGui::GetWindowContentRegionMin();
    const ImVec2 childContentMax = ImGui::GetWindowContentRegionMax();
    const ImVec2 childPos(childWindowPos.x + childContentMin.x, childWindowPos.y + childContentMin.y);
    const ImVec2 childMax(childWindowPos.x + childContentMax.x, childWindowPos.y + childContentMax.y);
    const ImVec2 avail(childMax.x - childPos.x, childMax.y - childPos.y);
    const ImVec2 paintPanelSize(
        std::clamp(avail.x - 28.0f, 208.0f, 238.0f),
        std::clamp(avail.y - 28.0f, 116.0f, 124.0f));
    const ImVec2 paintPanelMin(
        childPos.x + 14.0f,
        std::max(childPos.y + 14.0f, childMax.y - paintPanelSize.y - 14.0f));
    const ImVec2 paintPanelMax(paintPanelMin.x + paintPanelSize.x, paintPanelMin.y + paintPanelSize.y);
    if (pixelSpriteCanvasCenterPending) {
        pixelSpriteCanvasPan = ImVec2(0.0f, 0.0f);
        pixelSpriteCanvasTargetPan = pixelSpriteCanvasPan;
        pixelSpriteCanvasCenterPending = false;
    }

    ImGui::SetCursorScreenPos(childPos);
    ImGui::InvisibleButton("PixelCanvasButton", avail,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();
    const bool canvasActive = ImGui::IsItemActive();
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    const bool paintPanelHovered = ImGui::IsMouseHoveringRect(paintPanelMin, paintPanelMax, true);
    const bool wheelZoom = canvasHovered && !paintPanelHovered && std::abs(ImGui::GetIO().MouseWheel) > 0.0f;
    const bool middleMousePan = canvasActive &&
        ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f);

    if (wheelZoom) {
        const float oldZoom = std::max(0.001f, pixelSpriteZoom);
        const float nextZoom = pixelSpritePixelPerfect
            ? std::clamp(std::round(oldZoom) + (ImGui::GetIO().MouseWheel > 0.0f ? 1.0f : -1.0f), 1.0f, 128.0f)
            : std::clamp(oldZoom * (ImGui::GetIO().MouseWheel > 0.0f ? 1.12f : (1.0f / 1.12f)), 1.0f, 128.0f);

        const ImVec2 oldImageMin(
            childPos.x + (avail.x - imageSize.x) * 0.5f + pixelSpriteCanvasPan.x,
            childPos.y + (avail.y - imageSize.y) * 0.5f + pixelSpriteCanvasPan.y);
        const float imageSpaceX = (mousePos.x - oldImageMin.x) / oldZoom;
        const float imageSpaceY = (mousePos.y - oldImageMin.y) / oldZoom;
        pixelSpriteTargetZoom = nextZoom;
        pixelSpriteZoom = nextZoom;

        const ImVec2 nextImageSize(
            pixelSpriteDocument.width * nextZoom,
            pixelSpriteDocument.height * nextZoom);
        const ImVec2 nextCenteredMin(
            childPos.x + (avail.x - nextImageSize.x) * 0.5f,
            childPos.y + (avail.y - nextImageSize.y) * 0.5f);
        pixelSpriteCanvasPan.x = mousePos.x - (nextCenteredMin.x + imageSpaceX * nextZoom);
        pixelSpriteCanvasPan.y = mousePos.y - (nextCenteredMin.y + imageSpaceY * nextZoom);
        pixelSpriteCanvasTargetPan = pixelSpriteCanvasPan;
    } else if (middleMousePan) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        pixelSpriteCanvasPan.x += delta.x;
        pixelSpriteCanvasPan.y += delta.y;
        pixelSpriteCanvasTargetPan = pixelSpriteCanvasPan;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(childPos, childMax, true);
    const ImVec2 canvasOrigin(
        childPos.x + (avail.x - imageSize.x) * 0.5f + pixelSpriteCanvasPan.x,
        childPos.y + (avail.y - imageSize.y) * 0.5f + pixelSpriteCanvasPan.y);
    const ImVec2 imageMin = canvasOrigin;
    const ImVec2 imageMax(canvasOrigin.x + imageSize.x, canvasOrigin.y + imageSize.y);
    const int sampleStep = std::max(1, static_cast<int>(std::ceil(3.0f / std::max(0.001f, pixelSpriteZoom))));
    const bool darkChecker = pixelSpriteCheckerTheme == PixelSpriteCheckerTheme::Dark;

    draw->AddRectFilled(childPos, childMax, IM_COL32(12, 14, 24, 255), 12.0f);
    for (int y = 0; y < pixelSpriteDocument.height; y += sampleStep) {
        for (int x = 0; x < pixelSpriteDocument.width; x += sampleStep) {
            const int blockW = std::min(sampleStep, pixelSpriteDocument.width - x);
            const int blockH = std::min(sampleStep, pixelSpriteDocument.height - y);
            const ImVec2 p0(canvasOrigin.x + x * pixelSpriteZoom, canvasOrigin.y + y * pixelSpriteZoom);
            const ImVec2 p1(p0.x + blockW * pixelSpriteZoom, p0.y + blockH * pixelSpriteZoom);
            draw->AddRectFilled(p0, p1, CheckerColor(darkChecker, ((x + y) & 1) != 0));
        }
    }

    for (int y = 0; y < pixelSpriteDocument.height; y += sampleStep) {
        for (int x = 0; x < pixelSpriteDocument.width; x += sampleStep) {
            const size_t idx = static_cast<size_t>((y * pixelSpriteDocument.width + x) * 4);
            const ImU32 color = IM_COL32(
                pixelSpriteDocument.pixels[idx + 0],
                pixelSpriteDocument.pixels[idx + 1],
                pixelSpriteDocument.pixels[idx + 2],
                pixelSpriteDocument.pixels[idx + 3]);
            if ((color >> IM_COL32_A_SHIFT) == 0) {
                continue;
            }
            const int blockW = std::min(sampleStep, pixelSpriteDocument.width - x);
            const int blockH = std::min(sampleStep, pixelSpriteDocument.height - y);
            const ImVec2 p0(canvasOrigin.x + x * pixelSpriteZoom, canvasOrigin.y + y * pixelSpriteZoom);
            const ImVec2 p1(p0.x + blockW * pixelSpriteZoom, p0.y + blockH * pixelSpriteZoom);
            draw->AddRectFilled(p0, p1, color);
            if (sampleStep == 1 && pixelSpriteShowGrid && pixelSpriteZoom >= 8.0f) {
                draw->AddRect(p0, p1, IM_COL32(0, 0, 0, 72));
            }
        }
    }

    EnsureSpriteClipNames(pixelSpriteDocument.spriteFrameNames, pixelSpriteDocument.spriteFrames.size());
    for (size_t i = 0; i < pixelSpriteDocument.spriteFrames.size(); ++i) {
        const glm::ivec4 frame = pixelSpriteDocument.spriteFrames[i];
        const ImU32 frameColor = static_cast<int>(i) == pixelSpriteDocument.activeFrame
            ? IM_COL32(250, 210, 80, 255)
            : IM_COL32(80, 220, 170, 220);
        draw->AddRect(
            ImVec2(canvasOrigin.x + frame.x * pixelSpriteZoom, canvasOrigin.y + frame.y * pixelSpriteZoom),
            ImVec2(canvasOrigin.x + (frame.x + frame.z) * pixelSpriteZoom, canvasOrigin.y + (frame.y + frame.w) * pixelSpriteZoom),
            frameColor,
            0.0f,
            0,
            2.0f);
        draw->AddText(
            ImVec2(canvasOrigin.x + frame.x * pixelSpriteZoom + 4.0f,
                   canvasOrigin.y + frame.y * pixelSpriteZoom + 2.0f),
            frameColor,
            pixelSpriteDocument.spriteFrameNames[i].c_str());
    }

    if (pixelSpriteDocument.selectionActive) {
        const glm::ivec4 rect = NormalizeRect(pixelSpriteDocument.selectionStart, pixelSpriteDocument.selectionEnd);
        draw->AddRect(
            ImVec2(canvasOrigin.x + rect.x * pixelSpriteZoom, canvasOrigin.y + rect.y * pixelSpriteZoom),
            ImVec2(canvasOrigin.x + (rect.x + rect.z) * pixelSpriteZoom, canvasOrigin.y + (rect.y + rect.w) * pixelSpriteZoom),
            IM_COL32(255, 255, 255, 220),
            0.0f,
            0,
            2.0f);
    }

    if (pixelSpriteFloatingSelectionActive) {
        for (int y = 0; y < pixelSpriteFloatingSelectionSize.y; ++y) {
            for (int x = 0; x < pixelSpriteFloatingSelectionSize.x; ++x) {
                const size_t idx = static_cast<size_t>((y * pixelSpriteFloatingSelectionSize.x + x) * 4);
                const ImU32 color = IM_COL32(
                    pixelSpriteFloatingSelectionPixels[idx + 0],
                    pixelSpriteFloatingSelectionPixels[idx + 1],
                    pixelSpriteFloatingSelectionPixels[idx + 2],
                    pixelSpriteFloatingSelectionPixels[idx + 3]);
                if ((color >> IM_COL32_A_SHIFT) == 0) {
                    continue;
                }
                const ImVec2 p0(canvasOrigin.x + (pixelSpriteFloatingSelectionPosition.x + x) * pixelSpriteZoom,
                                canvasOrigin.y + (pixelSpriteFloatingSelectionPosition.y + y) * pixelSpriteZoom);
                const ImVec2 p1(p0.x + pixelSpriteZoom, p0.y + pixelSpriteZoom);
                draw->AddRectFilled(p0, p1, color);
                if (pixelSpriteShowGrid && pixelSpriteZoom >= 8.0f) {
                    draw->AddRect(p0, p1, IM_COL32(0, 0, 0, 72));
                }
            }
        }
    }

    if (shapeDragActive && pixelSpriteToolIsShape(shapeDragTool)) {
        const glm::ivec4 previewRect = NormalizeRect(shapeDragStart, shapeDragCurrent);
        const ImVec2 previewMin(canvasOrigin.x + previewRect.x * pixelSpriteZoom, canvasOrigin.y + previewRect.y * pixelSpriteZoom);
        const ImVec2 previewMax(canvasOrigin.x + (previewRect.x + previewRect.z) * pixelSpriteZoom, canvasOrigin.y + (previewRect.y + previewRect.w) * pixelSpriteZoom);
        const ImU32 previewColor = IM_COL32(206, 166, 255, 255);
        if (shapeDragTool == PixelSpriteTool::LineCurve) {
            draw->AddLine(
                ImVec2(canvasOrigin.x + (shapeDragStart.x + 0.5f) * pixelSpriteZoom, canvasOrigin.y + (shapeDragStart.y + 0.5f) * pixelSpriteZoom),
                ImVec2(canvasOrigin.x + (shapeDragCurrent.x + 0.5f) * pixelSpriteZoom, canvasOrigin.y + (shapeDragCurrent.y + 0.5f) * pixelSpriteZoom),
                previewColor,
                std::max(1.5f, pixelSpriteZoom * 0.08f));
        } else if (shapeDragTool == PixelSpriteTool::Rectangle) {
            draw->AddRect(previewMin, previewMax, previewColor, 0.0f, 0, 2.0f);
        } else if (shapeDragTool == PixelSpriteTool::RoundedRectangle) {
            draw->AddRect(previewMin, previewMax, previewColor, std::max(4.0f, pixelSpriteZoom * 0.6f), 0, 2.0f);
        } else if (shapeDragTool == PixelSpriteTool::Circle) {
            const ImVec2 center((previewMin.x + previewMax.x) * 0.5f, (previewMin.y + previewMax.y) * 0.5f);
            const ImVec2 radius((previewMax.x - previewMin.x) * 0.5f, (previewMax.y - previewMin.y) * 0.5f);
            const int samples = 48;
            for (int i = 0; i < samples; ++i) {
                const float t0 = static_cast<float>(i) / static_cast<float>(samples);
                const float t1 = static_cast<float>(i + 1) / static_cast<float>(samples);
                const float a0 = t0 * 6.28318530718f;
                const float a1 = t1 * 6.28318530718f;
                draw->AddLine(
                    ImVec2(center.x + std::cos(a0) * radius.x, center.y + std::sin(a0) * radius.y),
                    ImVec2(center.x + std::cos(a1) * radius.x, center.y + std::sin(a1) * radius.y),
                    previewColor,
                    2.0f);
            }
        }
    }

    draw->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 42));

    const std::string canvasHint = std::string(pixelSpriteToolLabel(pixelSpriteTool)) + "  |  Wheel zoom  |  MMB pan  |  Ctrl+Z / Ctrl+Y";
    const ImVec2 hintSize = ImGui::CalcTextSize(canvasHint.c_str());
    const ImVec2 hintMin(childMax.x - hintSize.x - 28.0f, childMax.y - hintSize.y - 18.0f);
    const ImVec2 hintMax(hintMin.x + hintSize.x + 16.0f, hintMin.y + hintSize.y + 10.0f);
    draw->AddRectFilled(hintMin, hintMax, IM_COL32(10, 12, 22, 210), 8.0f);
    draw->AddRect(hintMin, hintMax, IM_COL32(90, 76, 130, 180), 8.0f, 0, 1.0f);
    draw->AddText(ImVec2(hintMin.x + 8.0f, hintMin.y + 5.0f), IM_COL32(228, 228, 248, 220), canvasHint.c_str());
    draw->PopClipRect();

    ImGui::SetCursorScreenPos(paintPanelMin);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.08f, 0.12f, 0.84f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.42f, 0.31f, 0.62f, 0.58f));
    if (ImGui::BeginChild("PixelSpritePaintPanel", paintPanelSize, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImGuiColorEditFlags overlayColorFlags =
            ImGuiColorEditFlags_NoInputs |
            ImGuiColorEditFlags_NoLabel |
            ImGuiColorEditFlags_AlphaPreviewHalf;

        ImGui::TextDisabled("Paint");
        const float colorSectionWidth = (ImGui::GetContentRegionAvail().x - 10.0f) * 0.5f;
        ImGui::BeginGroup();
        ImGui::TextDisabled("Color");
        ImGui::SetNextItemWidth(colorSectionWidth);
        if (ImGui::ColorEdit4("##PixelSpriteOverlayPrimary", &pixelSpritePrimaryColor.x, overlayColorFlags)) {
            appendRecentColor(pixelSpritePrimaryColor);
        }
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 10.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("Alt Color");
        ImGui::SetNextItemWidth(colorSectionWidth);
        if (ImGui::ColorEdit4("##PixelSpriteOverlaySecondary", &pixelSpriteSecondaryColor.x, overlayColorFlags)) {
            appendRecentColor(pixelSpriteSecondaryColor);
        }
        ImGui::EndGroup();

        ImGui::TextDisabled("Recent Colors");
        const float swatchSize = 20.0f;
        const float swatchGap = 5.0f;
        const int swatchesPerRow = 5;
        for (int swatchIndex = 0; swatchIndex < 10; ++swatchIndex) {
            if ((swatchIndex % swatchesPerRow) != 0) {
                ImGui::SameLine(0.0f, swatchGap);
            }

            ImGui::PushID(swatchIndex);
            const bool hasSwatch = swatchIndex < static_cast<int>(pixelSpriteRecentColors.size());
            if (hasSwatch) {
                const glm::vec4 swatch = pixelSpriteRecentColors[swatchIndex];
                ImGui::InvisibleButton("##PixelSpriteRecentSwatch",
                                       ImVec2(swatchSize, swatchSize),
                                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    setPrimaryColor(swatch);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    setSecondaryColor(swatch);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Left: primary\nRight: secondary");
                }

                const ImVec2 swatchMin = ImGui::GetItemRectMin();
                const ImVec2 swatchMax = ImGui::GetItemRectMax();
                const ImU32 swatchColor = ImGui::ColorConvertFloat4ToU32(ImVec4(swatch.r, swatch.g, swatch.b, swatch.a));
                const bool primaryMatch = colorsNearEqual(swatch, pixelSpritePrimaryColor);
                const bool secondaryMatch = colorsNearEqual(swatch, pixelSpriteSecondaryColor);
                const ImU32 borderColor = primaryMatch
                    ? IM_COL32(210, 182, 255, 255)
                    : secondaryMatch
                        ? IM_COL32(140, 198, 255, 255)
                        : IM_COL32(92, 96, 122, 210);
                ImGui::GetWindowDrawList()->AddRectFilled(swatchMin, swatchMax, swatchColor, 4.0f);
                ImGui::GetWindowDrawList()->AddRect(swatchMin, swatchMax, borderColor, 4.0f, 0, primaryMatch || secondaryMatch ? 2.0f : 1.0f);
            } else {
                ImGui::Dummy(ImVec2(swatchSize, swatchSize));
                const ImVec2 swatchMin = ImGui::GetItemRectMin();
                const ImVec2 swatchMax = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(swatchMin, swatchMax, IM_COL32(72, 76, 98, 110), 4.0f);
            }
            ImGui::PopID();
        }

        const bool toolUsesBrush = pixelSpriteToolUsesBrush(pixelSpriteTool);
        ImGui::TextDisabled("Tool Drawing Size");
        if (!toolUsesBrush) {
            ImGui::BeginDisabled();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##PixelSpriteOverlayBrushSize", &pixelSpriteBrushSize, 1, 8);
        if (!toolUsesBrush) {
            ImGui::EndDisabled();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);

    const bool hovered = canvasHovered && !paintPanelHovered;
    const bool held = canvasActive && !paintPanelHovered;
    const int px = static_cast<int>((mousePos.x - canvasOrigin.x) / pixelSpriteZoom);
    const int py = static_cast<int>((mousePos.y - canvasOrigin.y) / pixelSpriteZoom);
    const bool validPixel = hovered && px >= 0 && py >= 0 && px < pixelSpriteDocument.width && py < pixelSpriteDocument.height;

    auto sampleCanvasPixel = [&](int x, int y) -> PixelRgba {
        PixelRgba sampled = GetPixel(pixelSpriteDocument.pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, x, y);
        if (pixelSpriteFloatingSelectionActive &&
            x >= pixelSpriteFloatingSelectionPosition.x &&
            y >= pixelSpriteFloatingSelectionPosition.y &&
            x < pixelSpriteFloatingSelectionPosition.x + pixelSpriteFloatingSelectionSize.x &&
            y < pixelSpriteFloatingSelectionPosition.y + pixelSpriteFloatingSelectionSize.y) {
            const PixelRgba floating = GetPixel(pixelSpriteFloatingSelectionPixels,
                                                pixelSpriteFloatingSelectionSize.x,
                                                pixelSpriteFloatingSelectionSize.y,
                                                x - pixelSpriteFloatingSelectionPosition.x,
                                                y - pixelSpriteFloatingSelectionPosition.y);
            sampled = AlphaComposite(sampled, floating);
        }
        return sampled;
    };

    if (validPixel && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pixelSpriteTool == PixelSpriteTool::ColorPicker) {
        const PixelRgba sampled = sampleCanvasPixel(px, py);
        setSecondaryColor(glm::vec4(
            sampled.r / 255.0f,
            sampled.g / 255.0f,
            sampled.b / 255.0f,
            sampled.a / 255.0f));
    }

    if (validPixel && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        PixelSpriteLayerState* activeLayer = getActiveLayer();
        if (pixelSpriteTool == PixelSpriteTool::ColorPicker) {
            const PixelRgba sampled = sampleCanvasPixel(px, py);
            setPrimaryColor(glm::vec4(
                sampled.r / 255.0f,
                sampled.g / 255.0f,
                sampled.b / 255.0f,
                sampled.a / 255.0f));
        } else if (pixelSpriteTool == PixelSpriteTool::Bucket && activeLayer) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
                commitHistoryTop();
                activeLayer = getActiveLayer();
            }
            const size_t idx = static_cast<size_t>((py * pixelSpriteDocument.width + px) * 4);
            const PixelRgba target{
                activeLayer->pixels[idx + 0],
                activeLayer->pixels[idx + 1],
                activeLayer->pixels[idx + 2],
                activeLayer->pixels[idx + 3]
            };
            pushHistory("Paint Bucket");
            appendRecentColor(pixelSpritePrimaryColor);
            FloodFill(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, px, py, target, ToRgba8(pixelSpritePrimaryColor));
            rebuildComposite();
            pixelSpriteDocument.dirty = true;
            commitHistoryTop();
        } else if (pixelSpriteTool == PixelSpriteTool::MagicSelect && activeLayer) {
            pushHistory("Magic Select");
            const glm::ivec4 bounds = FindConnectedRegionBounds(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, px, py);
            pixelSpriteDocument.selectionActive = true;
            pixelSpriteDocument.selectionStart = glm::ivec2(bounds.x, bounds.y);
            pixelSpriteDocument.selectionEnd = glm::ivec2(bounds.x + bounds.z - 1, bounds.y + bounds.w - 1);
            commitHistoryTop();
        } else if (pixelSpriteEditorMode == PixelSpriteEditorMode::SpriteSheet ||
                   pixelSpriteTool == PixelSpriteTool::SelectArea ||
                   pixelSpriteTool == PixelSpriteTool::Lasso) {
            beginRectSelection(px, py);
        } else if (pixelSpriteTool == PixelSpriteTool::MoveSelectedArea) {
            if (pixelSpriteDocument.selectionActive &&
                RectContainsPixel(NormalizeRect(pixelSpriteDocument.selectionStart, pixelSpriteDocument.selectionEnd), px, py)) {
                if (!pixelSpriteFloatingSelectionActive) {
                    pushHistory("Move Selection");
                    if (beginFloatingSelectionMove()) {
                        commitHistoryTop();
                    }
                }
                selectionMoveDrag = true;
                selectionMoveAnchor = glm::ivec2(px, py);
                selectionMoveOrigin = NormalizeRect(pixelSpriteDocument.selectionStart, pixelSpriteDocument.selectionEnd);
            } else {
                beginRectSelection(px, py);
            }
        } else if (pixelSpriteToolIsShape(pixelSpriteTool)) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
                commitHistoryTop();
            }
            const char* shapeHistoryLabel = "Shape";
            switch (pixelSpriteTool) {
                case PixelSpriteTool::LineCurve: shapeHistoryLabel = "Line / Curve"; break;
                case PixelSpriteTool::Rectangle: shapeHistoryLabel = "Rectangle"; break;
                case PixelSpriteTool::RoundedRectangle: shapeHistoryLabel = "Rounded Rectangle"; break;
                case PixelSpriteTool::Circle: shapeHistoryLabel = "Circle"; break;
                default: break;
            }
            pushHistory(shapeHistoryLabel);
            appendRecentColor(pixelSpritePrimaryColor);
            shapeDragActive = true;
            shapeDragStart = glm::ivec2(px, py);
            shapeDragCurrent = shapeDragStart;
            shapeDragTool = pixelSpriteTool;
            commitHistoryTop();
        } else if (activeLayer) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
                commitHistoryTop();
            }
            pushHistory(pixelSpriteTool == PixelSpriteTool::Eraser ? "Eraser" : "Paintbrush");
            appendRecentColor(pixelSpriteTool == PixelSpriteTool::Eraser ? pixelSpriteSecondaryColor : pixelSpritePrimaryColor);
            const PixelRgba color = (pixelSpriteTool == PixelSpriteTool::Eraser)
                ? ToRgba8(pixelSpriteSecondaryColor)
                : ToRgba8(pixelSpritePrimaryColor);
            PlotBrush(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, px, py, pixelSpriteBrushSize, color);
            rebuildComposite();
            pixelSpriteDocument.dirty = true;
            commitHistoryTop();
        }
    }

    if (validPixel && held && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (pixelSpriteEditorMode == PixelSpriteEditorMode::SpriteSheet ||
            pixelSpriteTool == PixelSpriteTool::SelectArea ||
            pixelSpriteTool == PixelSpriteTool::Lasso) {
            pixelSpriteDocument.selectionEnd = glm::ivec2(px, py);
            commitHistoryTop();
        } else if (selectionMoveDrag && pixelSpriteFloatingSelectionActive) {
            glm::ivec4 moved = selectionMoveOrigin;
            moved.x = std::clamp(selectionMoveOrigin.x + (px - selectionMoveAnchor.x), 0, std::max(0, pixelSpriteDocument.width - moved.z));
            moved.y = std::clamp(selectionMoveOrigin.y + (py - selectionMoveAnchor.y), 0, std::max(0, pixelSpriteDocument.height - moved.w));
            pixelSpriteFloatingSelectionPosition = glm::ivec2(moved.x, moved.y);
            pixelSpriteDocument.selectionActive = true;
            pixelSpriteDocument.selectionStart = glm::ivec2(moved.x, moved.y);
            pixelSpriteDocument.selectionEnd = glm::ivec2(moved.x + moved.z - 1, moved.y + moved.w - 1);
        } else if (shapeDragActive) {
            shapeDragCurrent = glm::ivec2(px, py);
        } else if (pixelSpriteTool == PixelSpriteTool::Pencil || pixelSpriteTool == PixelSpriteTool::Eraser) {
            PixelSpriteLayerState* activeLayer = getActiveLayer();
            if (activeLayer) {
                const PixelRgba color = (pixelSpriteTool == PixelSpriteTool::Eraser)
                    ? ToRgba8(pixelSpriteSecondaryColor)
                    : ToRgba8(pixelSpritePrimaryColor);
                PlotBrush(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, px, py, pixelSpriteBrushSize, color);
                rebuildComposite();
                pixelSpriteDocument.dirty = true;
                commitHistoryTop();
            }
        }
    }

    if (shapeDragActive && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        PixelSpriteLayerState* activeLayer = getActiveLayer();
        const PixelRgba color = ToRgba8(pixelSpritePrimaryColor);
        if (activeLayer) {
            switch (shapeDragTool) {
                case PixelSpriteTool::LineCurve:
                    DrawLineStroke(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, shapeDragStart, shapeDragCurrent, pixelSpriteBrushSize, color);
                    pixelSpriteDocument.dirty = true;
                    break;
                case PixelSpriteTool::Rectangle:
                    DrawRectangleStroke(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, shapeDragStart, shapeDragCurrent, pixelSpriteBrushSize, color);
                    pixelSpriteDocument.dirty = true;
                    break;
                case PixelSpriteTool::RoundedRectangle:
                    DrawRoundedRectangleStroke(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, shapeDragStart, shapeDragCurrent, pixelSpriteBrushSize, color);
                    pixelSpriteDocument.dirty = true;
                    break;
                case PixelSpriteTool::Circle:
                    DrawEllipseStroke(activeLayer->pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, shapeDragStart, shapeDragCurrent, pixelSpriteBrushSize, color);
                    pixelSpriteDocument.dirty = true;
                    break;
                default:
                    break;
            }
            rebuildComposite();
        }
        commitHistoryTop();
        shapeDragActive = false;
    }

    if (selectionMoveDrag && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (pixelSpriteFloatingSelectionActive) {
            commitFloatingSelection(true);
            commitHistoryTop();
        }
        selectionMoveDrag = false;
    }

    ImGui::EndChild();

    ImGui::SameLine(0.0f, bodySpacing);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.15f, 0.98f));
    ImGui::BeginChild("PixelSpriteRightPanel", ImVec2(0.0f, 0.0f), true);
    ImGui::PopStyleColor();

    if (pixelSpriteRightPanelCollapsed) {
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - 24.0f) * 0.5f));
        if (ImGui::Button("<", ImVec2(24.0f, 32.0f))) {
            pixelSpriteRightPanelCollapsed = false;
        }
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);

        ImGui::TextDisabled("Inspector");
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 42.0f));
        if (ImGui::Button(">", ImVec2(24.0f, 24.0f))) {
            pixelSpriteRightPanelCollapsed = true;
        }

        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::SeparatorText("Active Tool");
        ImGui::Text("%s", pixelSpriteToolLabel(pixelSpriteTool));
        ImGui::TextDisabled("%s", pixelSpriteEditorMode == PixelSpriteEditorMode::SpriteSheet ? "Spritesheet mode" : "Edit mode");
        if (pixelSpriteToolUsesBrush(pixelSpriteTool)) {
            ImGui::TextDisabled("Brush size: %d", pixelSpriteBrushSize);
        }
        if (pixelSpriteTool == PixelSpriteTool::ColorPicker) {
            ImGui::TextWrapped("Left click samples the primary color. Right click samples the secondary color.");
        } else if (pixelSpriteTool == PixelSpriteTool::MagicSelect) {
            ImGui::TextWrapped("Selects the bounding area of the clicked contiguous color region.");
        } else if (pixelSpriteTool == PixelSpriteTool::MoveSelectedArea) {
            ImGui::TextWrapped("Moves the current floating selection. Use a selection tool first if no region is active.");
        }

        ImGui::SeparatorText("View");
        if (ImGui::SliderFloat("Zoom", &pixelSpriteTargetZoom, 1.0f, 128.0f, pixelSpritePixelPerfect ? "%.0fx" : "%.1fx")) {
            if (pixelSpritePixelPerfect) {
                pixelSpriteTargetZoom = std::round(pixelSpriteTargetZoom);
            }
        }
        const char* checkerLabels[] = { "Light Checker", "Dark Checker" };
        int checkerIndex = static_cast<int>(pixelSpriteCheckerTheme);
        if (ImGui::Combo("Background", &checkerIndex, checkerLabels, IM_ARRAYSIZE(checkerLabels))) {
            pixelSpriteCheckerTheme = static_cast<PixelSpriteCheckerTheme>(checkerIndex);
        }
        ImGui::Checkbox("Show Grid", &pixelSpriteShowGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Pixel Perfect", &pixelSpritePixelPerfect);
        int dims[2] = { pixelSpriteDocument.width, pixelSpriteDocument.height };
        if (ImGui::InputInt2("Canvas Size", dims)) {
            dims[0] = std::clamp(dims[0], 1, 1024);
            dims[1] = std::clamp(dims[1], 1, 1024);
            if (dims[0] != pixelSpriteDocument.width || dims[1] != pixelSpriteDocument.height) {
                if (pixelSpriteFloatingSelectionActive) {
                    commitFloatingSelection(true);
                }
                pushHistory("Resize Canvas");
                const int copyW = std::min(dims[0], pixelSpriteDocument.width);
                const int copyH = std::min(dims[1], pixelSpriteDocument.height);
                for (PixelSpriteLayerState& layer : pixelSpriteDocument.layers) {
                    std::vector<unsigned char> resized(static_cast<size_t>(dims[0] * dims[1] * 4), 0);
                    for (int y = 0; y < copyH; ++y) {
                        for (int x = 0; x < copyW; ++x) {
                            for (int c = 0; c < 4; ++c) {
                                resized[static_cast<size_t>((y * dims[0] + x) * 4 + c)] =
                                    layer.pixels[static_cast<size_t>((y * pixelSpriteDocument.width + x) * 4 + c)];
                            }
                        }
                    }
                    layer.pixels.swap(resized);
                }
                pixelSpriteDocument.width = dims[0];
                pixelSpriteDocument.height = dims[1];
                rebuildComposite();
                pixelSpriteDocument.dirty = true;
                commitHistoryTop();
            }
        }
        if (pixelSpriteDocument.selectionActive) {
            const glm::ivec4 rect = NormalizeRect(pixelSpriteDocument.selectionStart, pixelSpriteDocument.selectionEnd);
            ImGui::TextDisabled("Selection: (%d, %d)  %d x %d", rect.x, rect.y, rect.z, rect.w);
        } else {
            ImGui::TextDisabled("Selection: none");
        }

        ImGui::SeparatorText("Layers");
        EnsureSpriteLayers(pixelSpriteDocument.layers, pixelSpriteDocument.width, pixelSpriteDocument.height);

        if (ImGui::Button("+", ImVec2(28.0f, 24.0f))) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
            }
            pushHistory("Add Layer");
            PixelSpriteLayerState newLayer;
            newLayer.name = "Layer_" + std::to_string(pixelSpriteDocument.layers.size());
            newLayer.visible = true;
            newLayer.pixels.assign(PixelBufferSize(pixelSpriteDocument.width, pixelSpriteDocument.height), 0);
            pixelSpriteDocument.layers.push_back(std::move(newLayer));
            pixelSpriteDocument.activeLayer = static_cast<int>(pixelSpriteDocument.layers.size()) - 1;
            pixelSpriteDocument.dirty = true;
            rebuildComposite();
            commitHistoryTop();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pixelSpriteDocument.layers.size() <= 1);
        if (ImGui::Button("-", ImVec2(28.0f, 24.0f))) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
            }
            pushHistory("Delete Layer");
            pixelSpriteDocument.layers.erase(pixelSpriteDocument.layers.begin() + pixelSpriteDocument.activeLayer);
            pixelSpriteDocument.activeLayer = std::clamp(pixelSpriteDocument.activeLayer, 0, static_cast<int>(pixelSpriteDocument.layers.size()) - 1);
            rebuildComposite();
            pixelSpriteDocument.dirty = true;
            commitHistoryTop();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(pixelSpriteDocument.layers.size() <= 1 || pixelSpriteDocument.activeLayer <= 0);
        if (ImGui::Button("Merge", ImVec2(54.0f, 24.0f))) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
            }
            pushHistory("Merge Layer Down");
            PixelSpriteLayerState& destinationLayer = pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer - 1];
            PixelSpriteLayerState& sourceLayer = pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer];
            for (int y = 0; y < pixelSpriteDocument.height; ++y) {
                for (int x = 0; x < pixelSpriteDocument.width; ++x) {
                    const PixelRgba dst = GetPixel(destinationLayer.pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, x, y);
                    const PixelRgba src = GetPixel(sourceLayer.pixels, pixelSpriteDocument.width, pixelSpriteDocument.height, x, y);
                    SetPixel(destinationLayer.pixels,
                             pixelSpriteDocument.width,
                             pixelSpriteDocument.height,
                             x,
                             y,
                             AlphaComposite(dst, src));
                }
            }
            pixelSpriteDocument.layers.erase(pixelSpriteDocument.layers.begin() + pixelSpriteDocument.activeLayer);
            --pixelSpriteDocument.activeLayer;
            rebuildComposite();
            pixelSpriteDocument.dirty = true;
            commitHistoryTop();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(pixelSpriteDocument.activeLayer >= static_cast<int>(pixelSpriteDocument.layers.size()) - 1);
        if (ImGui::Button("Up", ImVec2(38.0f, 24.0f))) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
            }
            pushHistory("Move Layer Up");
            std::swap(pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer],
                      pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer + 1]);
            if (pixelSpriteFloatingSelectionLayer == pixelSpriteDocument.activeLayer) {
                ++pixelSpriteFloatingSelectionLayer;
            } else if (pixelSpriteFloatingSelectionLayer == pixelSpriteDocument.activeLayer + 1) {
                --pixelSpriteFloatingSelectionLayer;
            }
            ++pixelSpriteDocument.activeLayer;
            rebuildComposite();
            pixelSpriteDocument.dirty = true;
            commitHistoryTop();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(pixelSpriteDocument.activeLayer <= 0);
        if (ImGui::Button("Dn", ImVec2(38.0f, 24.0f))) {
            if (pixelSpriteFloatingSelectionActive) {
                commitFloatingSelection(true);
            }
            pushHistory("Move Layer Down");
            std::swap(pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer],
                      pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer - 1]);
            if (pixelSpriteFloatingSelectionLayer == pixelSpriteDocument.activeLayer) {
                --pixelSpriteFloatingSelectionLayer;
            } else if (pixelSpriteFloatingSelectionLayer == pixelSpriteDocument.activeLayer - 1) {
                ++pixelSpriteFloatingSelectionLayer;
            }
            --pixelSpriteDocument.activeLayer;
            rebuildComposite();
            pixelSpriteDocument.dirty = true;
            commitHistoryTop();
        }
        ImGui::EndDisabled();

        if (ImGui::BeginChild("PixelSpriteLayerList", ImVec2(0.0f, 168.0f), true)) {
            for (int displayIndex = static_cast<int>(pixelSpriteDocument.layers.size()) - 1; displayIndex >= 0; --displayIndex) {
                PixelSpriteLayerState& layer = pixelSpriteDocument.layers[displayIndex];
                ImGui::PushID(displayIndex);
                if (ImGui::Button(layer.visible ? "V" : "H", ImVec2(24.0f, 22.0f))) {
                    if (pixelSpriteFloatingSelectionActive) {
                        commitFloatingSelection(true);
                    }
                    pushHistory("Toggle Layer Visibility");
                    layer.visible = !layer.visible;
                    rebuildComposite();
                    pixelSpriteDocument.dirty = true;
                    commitHistoryTop();
                }
                ImGui::SameLine(0.0f, 6.0f);
                const bool isActiveLayer = displayIndex == pixelSpriteDocument.activeLayer;
                if (ImGui::Selectable(layer.name.c_str(), isActiveLayer, 0, ImVec2(0.0f, 22.0f))) {
                    if (pixelSpriteFloatingSelectionActive) {
                        commitFloatingSelection(true);
                    }
                    pixelSpriteDocument.activeLayer = displayIndex;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        char activeLayerName[128];
        std::snprintf(activeLayerName, sizeof(activeLayerName), "%s", pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer].name.c_str());
        if (ImGui::InputText("Layer Name", activeLayerName, sizeof(activeLayerName))) {
            pixelSpriteDocument.layers[pixelSpriteDocument.activeLayer].name = activeLayerName;
            pixelSpriteDocument.dirty = true;
            commitHistoryTop();
        }
        ImGui::TextDisabled("Active layer %d of %d", pixelSpriteDocument.activeLayer + 1, static_cast<int>(pixelSpriteDocument.layers.size()));

        ImGui::SeparatorText("History");
        const float historyHeight = pixelSpriteEditorMode == PixelSpriteEditorMode::SpriteSheet ? 150.0f : 0.0f;
        if (ImGui::BeginChild("PixelSpriteHistoryList", ImVec2(0.0f, historyHeight), true)) {
            for (int historyIndex = static_cast<int>(pixelSpriteUndoStack.size()) - 1; historyIndex >= 0; --historyIndex) {
                const bool isCurrent = historyIndex == static_cast<int>(pixelSpriteUndoStack.size()) - 1;
                const std::string historyLabel = std::string(isCurrent ? "> " : "  ") + pixelSpriteUndoStack[historyIndex].label;
                ImGui::Selectable(historyLabel.c_str(), isCurrent, ImGuiSelectableFlags_Disabled, ImVec2(0.0f, 22.0f));
            }
        }
        ImGui::EndChild();

        if (pixelSpriteEditorMode == PixelSpriteEditorMode::SpriteSheet) {
            ImGui::SeparatorText("Spritesheet");
            if (ImGui::Button("Add Selection As Clip") && pixelSpriteDocument.selectionActive) {
                pushHistory("Add Clip");
                pixelSpriteDocument.spriteFrames.push_back(NormalizeRect(pixelSpriteDocument.selectionStart, pixelSpriteDocument.selectionEnd));
                pixelSpriteDocument.spriteFrameNames.push_back("Rect_" + std::to_string(pixelSpriteDocument.spriteFrames.size() - 1));
                pixelSpriteDocument.spriteFrameScales.push_back(glm::vec2(1.0f));
                pixelSpriteDocument.activeFrame = static_cast<int>(pixelSpriteDocument.spriteFrames.size()) - 1;
                pixelSpriteDocument.dirty = true;
                commitHistoryTop();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Clips")) {
                pushHistory("Clear Clips");
                pixelSpriteDocument.spriteFrames.clear();
                pixelSpriteDocument.spriteFrameNames.clear();
                pixelSpriteDocument.spriteFrameScales.clear();
                pixelSpriteDocument.activeFrame = 0;
                pixelSpriteDocument.dirty = true;
                commitHistoryTop();
            }

            if (!pixelSpriteDocument.spriteFrames.empty()) {
                EnsureSpriteClipNames(pixelSpriteDocument.spriteFrameNames, pixelSpriteDocument.spriteFrames.size());
                EnsureSpriteClipScales(pixelSpriteDocument.spriteFrameScales, pixelSpriteDocument.spriteFrames.size());
                pixelSpriteDocument.activeFrame = std::clamp(pixelSpriteDocument.activeFrame, 0, static_cast<int>(pixelSpriteDocument.spriteFrames.size()) - 1);
                ImGui::SliderInt("Active Clip", &pixelSpriteDocument.activeFrame, 0, static_cast<int>(pixelSpriteDocument.spriteFrames.size()) - 1);

                char clipNameBuf[128];
                std::snprintf(clipNameBuf, sizeof(clipNameBuf), "%s", pixelSpriteDocument.spriteFrameNames[pixelSpriteDocument.activeFrame].c_str());
                if (ImGui::InputText("Clip Name", clipNameBuf, sizeof(clipNameBuf))) {
                    pixelSpriteDocument.spriteFrameNames[pixelSpriteDocument.activeFrame] = clipNameBuf;
                    pixelSpriteDocument.dirty = true;
                    commitHistoryTop();
                }

                glm::ivec4 clipRect = pixelSpriteDocument.spriteFrames[pixelSpriteDocument.activeFrame];
                int clipPosition[2] = { clipRect.x, clipRect.y };
                int clipSize[2] = { clipRect.z, clipRect.w };
                bool clipRectChanged = false;
                if (ImGui::DragInt2("Clip Position", clipPosition, 1.0f, 0, std::max(pixelSpriteDocument.width, pixelSpriteDocument.height))) {
                    clipRect.x = clipPosition[0];
                    clipRect.y = clipPosition[1];
                    clipRectChanged = true;
                }
                if (ImGui::DragInt2("Clip Size", clipSize, 1.0f, 1, std::max(pixelSpriteDocument.width, pixelSpriteDocument.height))) {
                    clipRect.z = clipSize[0];
                    clipRect.w = clipSize[1];
                    clipRectChanged = true;
                }
                if (clipRectChanged) {
                    pixelSpriteDocument.spriteFrames[pixelSpriteDocument.activeFrame] =
                        ClampSpriteClipRect(clipRect, pixelSpriteDocument.width, pixelSpriteDocument.height);
                    pixelSpriteDocument.dirty = true;
                    commitHistoryTop();
                }
            }

            char versionBuf[128];
            std::snprintf(versionBuf, sizeof(versionBuf), "%s",
                          pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher.empty()
                              ? "V6.7"
                              : pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher.c_str());
            if (ImGui::InputText("Engine Version", versionBuf, sizeof(versionBuf))) {
                pixelSpriteDocument.expectedMinimumModuEngineVersionOrHigher = versionBuf;
                pixelSpriteDocument.dirty = true;
            }
            if (ImGui::Checkbox("Strict Validation", &pixelSpriteDocument.strictValidation)) {
                pixelSpriteDocument.dirty = true;
            }
        }
        ImGui::PopItemWidth();
        ImGui::PopStyleVar(3);
    }

    ImGui::EndChild();

    if (pixelSpriteOpenImagePopupTrigger) {
        ImGui::OpenPopup("Open Sprite Image");
        pixelSpriteOpenImagePopupTrigger = false;
    }
    if (pixelSpriteOpenImagePopupOpen) {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 460.0f), ImGuiCond_Appearing);
    }
    if (ImGui::BeginPopupModal("Open Sprite Image", &pixelSpriteOpenImagePopupOpen, ImGuiWindowFlags_NoCollapse)) {
        pixelSpriteOpenImageBrowser.refresh();

        std::error_code relError;
        fs::path relativeDir = fs::relative(pixelSpriteOpenImageBrowser.currentPath, pixelSpriteOpenImageBrowser.projectRoot, relError);
        const std::string currentDirLabel =
            relError || relativeDir.empty() || relativeDir == "."
                ? pixelSpriteOpenImageBrowser.projectRoot.filename().string()
                : relativeDir.generic_string();

        const bool canGoUp = pixelSpriteOpenImageBrowser.currentPath != pixelSpriteOpenImageBrowser.projectRoot &&
                             pixelSpriteOpenImageBrowser.currentPath.has_parent_path();

        ImGui::Text("Project Assets");
        ImGui::TextDisabled("%s", currentDirLabel.c_str());
        if (ImGui::Button("Up", ImVec2(54.0f, 0.0f)) && canGoUp) {
            pixelSpriteOpenImageBrowser.selectedFile.clear();
            pixelSpriteOpenImageBrowser.navigateUp();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(74.0f, 0.0f))) {
            pixelSpriteOpenImageBrowser.needsRefresh = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##PixelSpriteOpenImageSearch", "Filter image assets...", pixelSpriteOpenImageSearch, sizeof(pixelSpriteOpenImageSearch))) {
            pixelSpriteOpenImageBrowser.searchFilter = pixelSpriteOpenImageSearch;
            pixelSpriteOpenImageBrowser.needsRefresh = true;
        }

        if (ImGui::BeginChild("PixelSpriteOpenImageList", ImVec2(0.0f, -42.0f), true)) {
            if (pixelSpriteOpenImageBrowser.isRefreshing()) {
                ImGui::TextDisabled("Loading...");
            }

            for (const fs::directory_entry& entry : pixelSpriteOpenImageBrowser.entries) {
                const FileCategory category = pixelSpriteOpenImageBrowser.getFileCategory(entry);
                const bool isFolder = category == FileCategory::Folder;
                const bool isTexture = category == FileCategory::Texture;
                if (!isFolder && !isTexture) {
                    continue;
                }

                const bool selected = pixelSpriteOpenImageBrowser.selectedFile == entry.path();
                const std::string label = std::string(isFolder ? "[Folder] " : "[Image] ") + entry.path().filename().string();
                if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0.0f, 22.0f))) {
                    if (isFolder) {
                        pixelSpriteOpenImageBrowser.selectedFile.clear();
                    } else {
                        pixelSpriteOpenImageBrowser.selectedFile = entry.path();
                    }
                }

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (isFolder) {
                        pixelSpriteOpenImageBrowser.selectedFile.clear();
                        pixelSpriteOpenImageBrowser.navigateTo(entry.path());
                    } else if (loadPixelSpriteDocument(entry.path())) {
                        pixelSpriteOpenImagePopupOpen = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }
        ImGui::EndChild();

        bool canOpenSelected = false;
        if (!pixelSpriteOpenImageBrowser.selectedFile.empty() && fs::exists(pixelSpriteOpenImageBrowser.selectedFile)) {
            std::error_code entryError;
            fs::directory_entry selectedEntry(pixelSpriteOpenImageBrowser.selectedFile, entryError);
            canOpenSelected = !entryError && pixelSpriteOpenImageBrowser.isTextureFile(selectedEntry);
        }

        ImGui::BeginDisabled(!canOpenSelected);
        if (ImGui::Button("Open Image", ImVec2(108.0f, 0.0f))) {
            if (loadPixelSpriteDocument(pixelSpriteOpenImageBrowser.selectedFile)) {
                pixelSpriteOpenImagePopupOpen = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(108.0f, 0.0f))) {
            pixelSpriteOpenImagePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    ImGui::End();
}
