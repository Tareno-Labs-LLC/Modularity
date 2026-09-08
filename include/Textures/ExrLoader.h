#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Modularity {

// True when the buffer starts with the OpenEXR magic (0x76 0x2f 0x31 0x01).
bool IsExrData(const uint8_t* bytes, size_t size);

// Decodes an EXR into tightly packed 8-bit RGBA, bottom-up to match the row order
// stb_image produces under stbi_set_flip_vertically_on_load(1) - the rest of the
// texture pipeline already assumes that orientation.
//
// Values are clamped to [0,1] before scaling. See src/ThirdParty/tinyexr/README.md for
// why that is the right call for the maps this exists to load, and what it costs.
bool LoadExrToRgba8(const uint8_t* bytes,
                    size_t size,
                    int& outWidth,
                    int& outHeight,
                    std::vector<uint8_t>& outPixels,
                    std::string& outError);

} // namespace Modularity
