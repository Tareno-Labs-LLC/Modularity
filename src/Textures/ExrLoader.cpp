#include "../../include/Textures/ExrLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#include "../ThirdParty/tinyexr/tinyexr.h"

extern "C" unsigned char *stbi_zlib_compress(unsigned char *, int, int *out_len, int) {
    if (out_len != nullptr) {
        *out_len = 0;
    }
    return nullptr;
}

namespace Modularity {

namespace {

int ReadExrCompression(const uint8_t* bytes, size_t size) {
    size_t offset = 8;
    while (offset < size) {
        const uint8_t* nameEnd =
            static_cast<const uint8_t*>(std::memchr(bytes + offset, 0, size - offset));
        if (nameEnd == nullptr) break;
        const std::string name(reinterpret_cast<const char*>(bytes + offset),
                               static_cast<size_t>(nameEnd - (bytes + offset)));
        offset = static_cast<size_t>(nameEnd - bytes) + 1;
        if (name.empty()) break;

        const uint8_t* typeEnd =
            static_cast<const uint8_t*>(std::memchr(bytes + offset, 0, size - offset));
        if (typeEnd == nullptr) break;
        offset = static_cast<size_t>(typeEnd - bytes) + 1;

        if (offset + 4 > size) break;
        int32_t attrSize = 0;
        std::memcpy(&attrSize, bytes + offset, 4);
        offset += 4;
        if (attrSize < 0 || offset + static_cast<size_t>(attrSize) > size) break;

        if (name == "compression" && attrSize >= 1) {
            return bytes[offset];
        }
        offset += static_cast<size_t>(attrSize);
    }
    return -1;
}

const char* ExrCompressionName(int compression) {
    switch (compression) {
        case 0: return "NONE";
        case 1: return "RLE";
        case 2: return "ZIPS";
        case 3: return "ZIP";
        case 4: return "PIZ";
        case 5: return "PXR24";
        case 6: return "B44";
        case 7: return "B44A";
        case 8: return "DWAA";
        case 9: return "DWAB";
        default: return "unrecognised";
    }
}

} // namespace

bool IsExrData(const uint8_t* bytes, size_t size) {
    return bytes != nullptr && size >= 4 &&
           bytes[0] == 0x76 && bytes[1] == 0x2f && bytes[2] == 0x31 && bytes[3] == 0x01;
}

bool LoadExrToRgba8(const uint8_t* bytes,
                    size_t size,
                    int& outWidth,
                    int& outHeight,
                    std::vector<uint8_t>& outPixels,
                    std::string& outError) {
    outWidth = 0;
    outHeight = 0;
    outPixels.clear();
    outError.clear();

    if (!IsExrData(bytes, size)) {
        outError = "not an EXR image";
        return false;
    }

    float* rgba = nullptr;
    int width = 0;
    int height = 0;
    const char* err = nullptr;
    const int result = LoadEXRFromMemory(&rgba, &width, &height, bytes, size, &err);
    if (result != TINYEXR_SUCCESS || rgba == nullptr) {
        outError = err ? err : "unknown EXR decode failure";
        if (err) {
            FreeEXRErrorMessage(err);
        }
        const int compression = ReadExrCompression(bytes, size);
        if (compression >= 0) {
            outError += std::string(" [compression: ") + ExrCompressionName(compression) + "]";
            if (compression == 8 || compression == 9) {
                outError +=
                    " - DWAA/DWAB is lossy DreamWorks compression, which the bundled EXR "
                    "reader does not implement. Re-save the image with ZIP or PIZ "
                    "compression, or convert it to PNG.";
            }
        }
        std::free(rgba);
        return false;
    }
    if (err) {
        FreeEXRErrorMessage(err);
    }

    if (width <= 0 || height <= 0) {
        outError = "EXR reported a zero-sized image";
        std::free(rgba);
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    outPixels.resize(rowBytes * static_cast<size_t>(height));

    for (int y = 0; y < height; ++y) {
        const float* src = rgba + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(width) * 4u;
        uint8_t* dst = outPixels.data() + static_cast<size_t>(y) * rowBytes;
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float v = src[static_cast<size_t>(x) * 4u + static_cast<size_t>(c)];
                const float safe = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
                dst[static_cast<size_t>(x) * 4u + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(safe * 255.0f + 0.5f);
            }
        }
    }

    std::free(rgba);

    bool anyAlpha = false;
    for (size_t i = 3; i < outPixels.size(); i += 4) {
        if (outPixels[i] != 0) { anyAlpha = true; break; }
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < outPixels.size(); i += 4) {
            outPixels[i] = 255;
        }
    }

    outWidth = width;
    outHeight = height;
    return true;
}

} // namespace Modularity
