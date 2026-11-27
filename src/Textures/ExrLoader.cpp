#include "../../include/Textures/ExrLoader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

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

// ---------------------------------------------------------------------------
// DWAA / DWAB decoder
//
// tinyexr does not implement DreamWorks' lossy DCT compression, and it is the default
// for a lot of downloaded material (every Poly Haven EXR, for one). This is a port of
// the decode half of OpenEXR's ImfDwaCompressor.cpp (BSD-3-Clause, Copyright (c) 2009-2014
// DreamWorks Animation LLC), restricted to scanline images - which is all those libraries
// ship. It reuses tinyexr's Huffman / RLE / FP16 helpers, which are compiled into this TU.
//
// A DWA chunk holds, per channel, one of three encodings:
//   LOSSY_DCT - 8x8 DCT blocks, AC terms RLE'd then Huffman- or zlib-packed, DC terms
//               zlib-packed with the ZIP predictor. R/G/B triples sharing a layer prefix
//               are stored as Y'CbCr and in a perceptual (gamma 2.2 / log) space.
//   RLE       - EXR RLE over byte planes, then zlib (used for alpha).
//   UNKNOWN   - plain zlib over the planar samples (everything else).
// ---------------------------------------------------------------------------

enum ExrPixelType { kExrUint = 0, kExrHalf = 1, kExrFloat = 2 };
enum DwaScheme { kDwaUnknown = 0, kDwaLossyDct = 1, kDwaRle = 2 };
enum DwaAcCompression { kDwaStaticHuffman = 0, kDwaDeflate = 1 };

struct ExrChannelInfo {
    std::string name;
    int type = kExrHalf;
    bool pLinear = false;
    int xSampling = 1;
    int ySampling = 1;
};

struct ExrScanlineHeader {
    std::vector<ExrChannelInfo> channels;
    int compression = -1;
    int minX = 0, minY = 0, maxX = -1, maxY = -1;
    bool tiled = false;
    size_t headerEnd = 0; // offset of the first byte after the header terminator
};

struct DwaRule {
    std::string suffix;
    int scheme = kDwaUnknown;
    int type = kExrHalf;
    int cscIdx = -1;
    bool caseInsensitive = false;
};

int ExrPixelSize(int type) { return type == kExrHalf ? 2 : 4; }

int32_t ReadLe32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

uint64_t ReadLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void WriteLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>(v >> 8);
}

float HalfToFloat(uint16_t bits) {
    tinyexr::FP16 h;
    h.u = bits;
    return tinyexr::half_to_float(h).f;
}

uint16_t FloatToHalf(float value) {
    tinyexr::FP32 f;
    f.f = value;
    return static_cast<uint16_t>(tinyexr::float_to_half_full(f).u);
}

// Floor division / positive modulus, matching Imath::divp / modp.
int DivFloor(int x, int y) {
    return (x >= 0) ? ((y >= 0) ? x / y : -(x / -y))
                    : ((y >= 0) ? -((y - 1 - x) / y) : ((-y - 1 - x) / -y));
}

int ModPositive(int x, int y) { return x - y * DivFloor(x, y); }

// Imf::numSamples - how many samples a channel with sampling `s` has in [a, b].
int NumSamples(int s, int a, int b) {
    const int a1 = DivFloor(a, s);
    const int b1 = DivFloor(b, s);
    return b1 - a1 + ((a1 * s < a) ? 0 : 1);
}

bool ParseScanlineHeader(const uint8_t* bytes, size_t size, ExrScanlineHeader& out, std::string& err) {
    if (size < 8) { err = "truncated EXR header"; return false; }
    const int32_t version = ReadLe32(bytes + 4);
    if ((version & 0xff) != 2) { err = "unsupported EXR version"; return false; }
    if (version & 0x800) { err = "deep EXR images are not supported"; return false; }
    if (version & 0x1000) { err = "multi-part EXR images are not supported"; return false; }
    out.tiled = (version & 0x200) != 0;

    bool haveChannels = false, haveWindow = false;
    size_t offset = 8;
    for (;;) {
        if (offset >= size) { err = "truncated EXR header"; return false; }
        const uint8_t* nameEnd =
            static_cast<const uint8_t*>(std::memchr(bytes + offset, 0, size - offset));
        if (nameEnd == nullptr) { err = "truncated EXR header"; return false; }
        const std::string name(reinterpret_cast<const char*>(bytes + offset),
                               static_cast<size_t>(nameEnd - (bytes + offset)));
        offset = static_cast<size_t>(nameEnd - bytes) + 1;
        if (name.empty()) break; // header terminator

        const uint8_t* typeEnd =
            static_cast<const uint8_t*>(std::memchr(bytes + offset, 0, size - offset));
        if (typeEnd == nullptr) { err = "truncated EXR header"; return false; }
        offset = static_cast<size_t>(typeEnd - bytes) + 1;
        if (offset + 4 > size) { err = "truncated EXR header"; return false; }
        const int32_t attrSize = ReadLe32(bytes + offset);
        offset += 4;
        if (attrSize < 0 || offset + static_cast<size_t>(attrSize) > size) {
            err = "corrupt EXR header attribute";
            return false;
        }
        const uint8_t* value = bytes + offset;

        if (name == "channels") {
            size_t p = 0;
            const size_t n = static_cast<size_t>(attrSize);
            while (p < n && value[p] != 0) {
                const uint8_t* chEnd = static_cast<const uint8_t*>(std::memchr(value + p, 0, n - p));
                if (chEnd == nullptr) { err = "corrupt EXR channel list"; return false; }
                ExrChannelInfo ch;
                ch.name.assign(reinterpret_cast<const char*>(value + p), static_cast<size_t>(chEnd - (value + p)));
                p = static_cast<size_t>(chEnd - value) + 1;
                if (p + 16 > n) { err = "corrupt EXR channel list"; return false; }
                ch.type = ReadLe32(value + p);
                ch.pLinear = value[p + 4] != 0;
                ch.xSampling = ReadLe32(value + p + 8);
                ch.ySampling = ReadLe32(value + p + 12);
                p += 16;
                if (ch.type < kExrUint || ch.type > kExrFloat || ch.xSampling < 1 || ch.ySampling < 1) {
                    err = "corrupt EXR channel '" + ch.name + "'";
                    return false;
                }
                out.channels.push_back(ch);
            }
            haveChannels = true;
        } else if (name == "dataWindow" && attrSize >= 16) {
            out.minX = ReadLe32(value);
            out.minY = ReadLe32(value + 4);
            out.maxX = ReadLe32(value + 8);
            out.maxY = ReadLe32(value + 12);
            haveWindow = true;
        } else if (name == "compression" && attrSize >= 1) {
            out.compression = value[0];
        }
        offset += static_cast<size_t>(attrSize);
    }

    if (!haveChannels || out.channels.empty()) { err = "EXR has no channels"; return false; }
    if (!haveWindow || out.maxX < out.minX || out.maxY < out.minY) {
        err = "EXR has an invalid data window";
        return false;
    }
    const int64_t w = static_cast<int64_t>(out.maxX) - out.minX + 1;
    const int64_t h = static_cast<int64_t>(out.maxY) - out.minY + 1;
    if (w > (1 << 16) || h > (1 << 16)) { err = "EXR image is too large"; return false; }
    out.headerEnd = offset;
    return true;
}

// zlib inflate of exactly `expected` bytes. stb's inflater is already linked in (see README).
bool InflateExact(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t expected) {
    if (expected == 0) return true;
    if (srcSize > static_cast<size_t>(INT32_MAX) || expected > static_cast<size_t>(INT32_MAX)) return false;
    const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(dst), static_cast<int>(expected),
                                            reinterpret_cast<const char*>(src), static_cast<int>(srcSize));
    return got >= 0 && static_cast<size_t>(got) == expected;
}

// Imf::Zip::uncompress - inflate, undo the byte delta predictor, then re-interleave the
// two half-buffers. Used for the DWA DC plane.
bool ZipUncompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t expected) {
    if (expected == 0) return true;
    std::vector<uint8_t> tmp(expected);
    if (!InflateExact(src, srcSize, tmp.data(), expected)) return false;
    for (size_t i = 1; i < expected; ++i) {
        tmp[i] = static_cast<uint8_t>(static_cast<int>(tmp[i - 1]) + static_cast<int>(tmp[i]) - 128);
    }
    const uint8_t* t1 = tmp.data();
    const uint8_t* t2 = tmp.data() + (expected + 1) / 2;
    for (size_t i = 0; i < expected;) {
        dst[i++] = *t1++;
        if (i < expected) dst[i++] = *t2++;
    }
    return true;
}

// dwaLookups' toLinear[]: undo the perceptual curve the encoder applied before the DCT.
const std::vector<uint16_t>& DwaToLinearTable() {
    static const std::vector<uint16_t> table = [] {
        std::vector<uint16_t> lut(65536, 0);
        const float logBase = static_cast<float>(std::pow(2.7182818, 2.2));
        for (int i = 1; i < 65536; ++i) {
            if ((i & 0x7c00) == 0x7c00) continue; // NaN / Inf -> 0
            const float h = HalfToFloat(static_cast<uint16_t>(i));
            const float sign = h < 0.0f ? -1.0f : 1.0f;
            const float a = std::fabs(h);
            const float lin = (a <= 1.0f) ? sign * std::pow(a, 2.2f)
                                          : sign * std::pow(logBase, a - 1.0f);
            lut[static_cast<size_t>(i)] = FloatToHalf(lin);
        }
        return lut;
    }();
    return table;
}

// zigzag index for each position of the 8x8 block, row-major.
const int kDwaZigZag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};

// dctInverse8x8_scalar<0> from ImfDwaCompressorSimd.h (same constants, same op order).
void DwaInverseDct8x8(float* data) {
    const float a = .5f * std::cos(3.14159f / 4.0f);
    const float b = .5f * std::cos(3.14159f / 16.0f);
    const float c = .5f * std::cos(3.14159f / 8.0f);
    const float d = .5f * std::cos(3.f * 3.14159f / 16.0f);
    const float e = .5f * std::cos(5.f * 3.14159f / 16.0f);
    const float f = .5f * std::cos(3.f * 3.14159f / 8.0f);
    const float g = .5f * std::cos(7.f * 3.14159f / 16.0f);

    float alpha[4], beta[4], theta[4], gamma[4];
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = 0; k < 8; ++k) {
            // pass 0: rows (stride 1), pass 1: columns (stride 8)
            float* v = pass == 0 ? data + k * 8 : data + k;
            const int s = pass == 0 ? 1 : 8;
            alpha[0] = c * v[2 * s];
            alpha[1] = f * v[2 * s];
            alpha[2] = c * v[6 * s];
            alpha[3] = f * v[6 * s];
            beta[0] = b * v[1 * s] + d * v[3 * s] + e * v[5 * s] + g * v[7 * s];
            beta[1] = d * v[1 * s] - g * v[3 * s] - b * v[5 * s] - e * v[7 * s];
            beta[2] = e * v[1 * s] - b * v[3 * s] + g * v[5 * s] + d * v[7 * s];
            beta[3] = g * v[1 * s] - e * v[3 * s] + d * v[5 * s] - b * v[7 * s];
            theta[0] = a * (v[0] + v[4 * s]);
            theta[3] = a * (v[0] - v[4 * s]);
            theta[1] = alpha[0] + alpha[3];
            theta[2] = alpha[1] - alpha[2];
            gamma[0] = theta[0] + theta[1];
            gamma[1] = theta[3] + theta[2];
            gamma[2] = theta[3] - theta[2];
            gamma[3] = theta[0] - theta[1];
            v[0 * s] = gamma[0] + beta[0];
            v[1 * s] = gamma[1] + beta[1];
            v[2 * s] = gamma[2] + beta[2];
            v[3 * s] = gamma[3] + beta[3];
            v[4 * s] = gamma[3] - beta[3];
            v[5 * s] = gamma[2] - beta[2];
            v[6 * s] = gamma[1] - beta[1];
            v[7 * s] = gamma[0] - beta[0];
        }
    }
}

struct DwaChannelState {
    int scheme = kDwaUnknown;
    int width = 0;
    int height = 0;
    const uint8_t* planar = nullptr;       // UNKNOWN: contiguous samples
    const uint8_t* rlePlanes[4] = {};       // RLE: one plane per byte of the sample
    std::vector<uint8_t*> rows;             // this channel's rows in the output buffer
};

// LossyDctDecoderBase::execute for one channel (numComp == 1) or one CSC triple.
bool DecodeLossyDct(DwaChannelState* const* comps,
                    const int* types,
                    int numComp,
                    const uint16_t* toLinear, // nullptr = identity
                    const uint16_t*& ac,
                    const uint16_t* acEnd,
                    const uint16_t*& dc,
                    const uint16_t* dcEnd) {
    const int width = comps[0]->width;
    const int height = comps[0]->height;
    if (width <= 0 || height <= 0) return true;
    for (int c = 0; c < numComp; ++c) {
        if (static_cast<int>(comps[c]->rows.size()) != height || comps[c]->width != width) return false;
    }

    const int numBlocksX = (width + 7) / 8;
    const int numBlocksY = (height + 7) / 8;
    const size_t dcPerComp = static_cast<size_t>(numBlocksX) * static_cast<size_t>(numBlocksY);
    if (static_cast<size_t>(dcEnd - dc) < dcPerComp * static_cast<size_t>(numComp)) return false;

    const uint16_t* dcComp[3] = {dc, dc + dcPerComp, dc + 2 * dcPerComp};
    std::vector<uint16_t> rowBlock(static_cast<size_t>(numComp) * numBlocksX * 64);
    float dct[3][64];
    uint16_t zig[64];

    for (int by = 0; by < numBlocksY; ++by) {
        const int maxY = (by == numBlocksY - 1) ? height - by * 8 : 8;
        for (int bx = 0; bx < numBlocksX; ++bx) {
            for (int c = 0; c < numComp; ++c) {
                std::memset(zig, 0, sizeof(zig));
                zig[0] = *dcComp[c]++;

                // un-RLE the AC terms: 0xff00 ends the block, 0xffNN skips NN zeros.
                int lastNonZero = 0;
                int k = 1;
                while (k < 64) {
                    if (ac >= acEnd) return false;
                    const uint16_t v = *ac++;
                    if (v == 0xff00) {
                        k = 64;
                    } else if ((v >> 8) == 0xff) {
                        k += v & 0xff;
                    } else {
                        lastNonZero = k;
                        zig[k++] = v;
                    }
                }

                float* blk = dct[c];
                if (lastNonZero == 0) {
                    const float val = HalfToFloat(zig[0]) * 3.535536e-01f * 3.535536e-01f;
                    for (int i = 0; i < 64; ++i) blk[i] = val;
                } else {
                    for (int i = 0; i < 64; ++i) blk[i] = HalfToFloat(zig[kDwaZigZag[i]]);
                    DwaInverseDct8x8(blk);
                }
            }

            if (numComp == 3) {
                // csc709Inverse: Y'CbCr -> R'G'B'
                for (int i = 0; i < 64; ++i) {
                    const float y = dct[0][i], cb = dct[1][i], cr = dct[2][i];
                    dct[0][i] = y + 1.5747f * cr;
                    dct[1][i] = y - 0.1873f * cb - 0.4682f * cr;
                    dct[2][i] = y + 1.8556f * cb;
                }
            }

            for (int c = 0; c < numComp; ++c) {
                uint16_t* dst = &rowBlock[(static_cast<size_t>(c) * numBlocksX + bx) * 64];
                for (int i = 0; i < 64; ++i) dst[i] = FloatToHalf(dct[c][i]);
            }
        }

        // Unblock into the scanlines, back to linear. Rows are written as HALF; FLOAT
        // channels are widened below.
        for (int c = 0; c < numComp; ++c) {
            for (int y = by * 8; y < by * 8 + maxY; ++y) {
                uint8_t* dst = comps[c]->rows[static_cast<size_t>(y)];
                for (int x = 0; x < width; ++x) {
                    const uint16_t nl =
                        rowBlock[(static_cast<size_t>(c) * numBlocksX + x / 8) * 64 + (y & 7) * 8 + (x & 7)];
                    WriteLe16(dst + static_cast<size_t>(x) * 2, toLinear ? toLinear[nl] : nl);
                }
            }
        }
    }

    for (int c = 0; c < numComp; ++c) {
        if (types[c] != kExrFloat) continue;
        std::vector<uint16_t> halves(static_cast<size_t>(width));
        for (int y = 0; y < height; ++y) {
            uint8_t* row = comps[c]->rows[static_cast<size_t>(y)];
            for (int x = 0; x < width; ++x) halves[static_cast<size_t>(x)] = ReadLe16(row + x * 2);
            for (int x = 0; x < width; ++x) {
                const float fv = HalfToFloat(halves[static_cast<size_t>(x)]);
                std::memcpy(row + static_cast<size_t>(x) * 4, &fv, 4);
            }
        }
    }

    dc += dcPerComp * static_cast<size_t>(numComp);
    return true;
}

// DwaCompressor::uncompress for one scanline chunk covering rows [minY, maxY]. `out`
// receives the standard EXR interleaved layout (per row, per channel, all samples).
bool DwaUncompressChunk(const ExrScanlineHeader& hdr,
                        const uint8_t* in,
                        size_t inSize,
                        int minY,
                        int maxY,
                        std::vector<uint8_t>& out,
                        std::string& err) {
    enum {
        VERSION = 0, UNKNOWN_UNCOMPRESSED_SIZE, UNKNOWN_COMPRESSED_SIZE, AC_COMPRESSED_SIZE,
        DC_COMPRESSED_SIZE, RLE_COMPRESSED_SIZE, RLE_UNCOMPRESSED_SIZE, RLE_RAW_SIZE,
        AC_UNCOMPRESSED_COUNT, DC_UNCOMPRESSED_COUNT, AC_COMPRESSION, NUM_SIZES_SINGLE
    };
    const size_t fixedHeader = NUM_SIZES_SINGLE * 8;
    if (inSize < fixedHeader) { err = "DWA chunk header is truncated"; return false; }
    uint64_t sz[NUM_SIZES_SINGLE];
    for (int i = 0; i < NUM_SIZES_SINGLE; ++i) sz[i] = ReadLe64(in + i * 8);
    for (int i = 0; i < NUM_SIZES_SINGLE; ++i) {
        if (sz[i] > static_cast<uint64_t>(INT32_MAX)) { err = "DWA chunk header is corrupt"; return false; }
    }
    const uint64_t version = sz[VERSION];
    if (version > 2) { err = "unsupported DWA data version"; return false; }

    const uint8_t* dataPtr = in + fixedHeader;
    size_t remaining = inSize - fixedHeader;

    // Channel classification rules.
    std::vector<DwaRule> rules;
    if (version < 2) {
        static const char* const kLegacy[][2] = {
            {"r", "0"}, {"red", "0"}, {"g", "1"}, {"grn", "1"}, {"green", "1"},
            {"b", "2"}, {"blu", "2"}, {"blue", "2"}, {"y", "-"}, {"by", "-"}, {"ry", "-"},
        };
        for (const auto& l : kLegacy) {
            for (int t = kExrHalf; t <= kExrFloat; ++t) {
                DwaRule r;
                r.suffix = l[0];
                r.scheme = kDwaLossyDct;
                r.type = t;
                r.cscIdx = l[1][0] == '-' ? -1 : l[1][0] - '0';
                r.caseInsensitive = true;
                rules.push_back(r);
            }
        }
        for (int t = kExrUint; t <= kExrFloat; ++t) {
            DwaRule r;
            r.suffix = "a";
            r.scheme = kDwaRle;
            r.type = t;
            r.caseInsensitive = true;
            rules.push_back(r);
        }
    } else {
        if (remaining < 2) { err = "DWA rule table is truncated"; return false; }
        const uint16_t ruleSize = ReadLe16(dataPtr);
        if (ruleSize < 2 || ruleSize > remaining) { err = "DWA rule table is corrupt"; return false; }
        const uint8_t* p = dataPtr + 2;
        const uint8_t* rulesEnd = dataPtr + ruleSize;
        while (p < rulesEnd) {
            const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(p, 0, static_cast<size_t>(rulesEnd - p)));
            if (nul == nullptr || rulesEnd - nul < 3) { err = "DWA rule is truncated"; return false; }
            DwaRule r;
            r.suffix.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(nul - p));
            const uint8_t packed = nul[1];
            r.cscIdx = static_cast<int>(packed >> 4) - 1;
            r.scheme = (packed >> 2) & 3;
            r.caseInsensitive = (packed & 1) != 0;
            r.type = nul[2];
            if (r.cscIdx < -1 || r.cscIdx >= 3 || r.scheme > kDwaRle || r.type > kExrFloat) {
                err = "DWA rule is corrupt";
                return false;
            }
            if (r.caseInsensitive) {
                std::transform(r.suffix.begin(), r.suffix.end(), r.suffix.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            }
            rules.push_back(r);
            p = nul + 3;
        }
        dataPtr += ruleSize;
        remaining -= ruleSize;
    }

    const uint64_t compressedTotal =
        sz[UNKNOWN_COMPRESSED_SIZE] + sz[AC_COMPRESSED_SIZE] + sz[DC_COMPRESSED_SIZE] + sz[RLE_COMPRESSED_SIZE];
    if (compressedTotal > remaining) { err = "DWA chunk is truncated"; return false; }

    // classifyChannels: last matching rule wins; CSC sets are grouped by layer prefix.
    const size_t numChan = hdr.channels.size();
    std::vector<DwaChannelState> chans(numChan);
    std::map<std::string, std::array<int, 3>> prefixMap;
    for (size_t ci = 0; ci < numChan; ++ci) {
        const ExrChannelInfo& info = hdr.channels[ci];
        std::string prefix, suffix = info.name;
        const size_t dot = suffix.find_last_of('.');
        if (dot != std::string::npos) {
            prefix = suffix.substr(0, dot);
            suffix = suffix.substr(dot + 1);
        }
        std::string lower = suffix;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        auto it = prefixMap.find(prefix);
        if (it == prefixMap.end()) it = prefixMap.emplace(prefix, std::array<int, 3>{{-1, -1, -1}}).first;
        for (const DwaRule& r : rules) {
            if (r.type != info.type) continue;
            if ((r.caseInsensitive ? lower : suffix) != r.suffix) continue;
            chans[ci].scheme = r.scheme;
            if (r.cscIdx >= 0) it->second[static_cast<size_t>(r.cscIdx)] = static_cast<int>(ci);
        }
    }
    std::vector<std::array<int, 3>> cscSets;
    for (const auto& kv : prefixMap) {
        const int r = kv.second[0], g = kv.second[1], b = kv.second[2];
        if (r < 0 || g < 0 || b < 0) continue;
        const ExrChannelInfo& cr = hdr.channels[static_cast<size_t>(r)];
        const ExrChannelInfo& cg = hdr.channels[static_cast<size_t>(g)];
        const ExrChannelInfo& cb = hdr.channels[static_cast<size_t>(b)];
        if (cr.xSampling != cg.xSampling || cr.xSampling != cb.xSampling ||
            cr.ySampling != cg.ySampling || cr.ySampling != cb.ySampling) {
            continue;
        }
        cscSets.push_back(kv.second);
    }

    // setupChannelData: planar buffer sizes per scheme.
    size_t planarSize[3] = {0, 0, 0};
    size_t numDctBlocks = 0;
    for (size_t ci = 0; ci < numChan; ++ci) {
        const ExrChannelInfo& info = hdr.channels[ci];
        DwaChannelState& cd = chans[ci];
        cd.width = NumSamples(info.xSampling, hdr.minX, hdr.maxX);
        cd.height = NumSamples(info.ySampling, minY, maxY);
        const size_t samples = static_cast<size_t>(std::max(cd.width, 0)) * static_cast<size_t>(std::max(cd.height, 0));
        if (cd.scheme == kDwaLossyDct) {
            numDctBlocks += static_cast<size_t>((cd.width + 7) / 8) * static_cast<size_t>((cd.height + 7) / 8);
        } else {
            planarSize[cd.scheme] += samples * static_cast<size_t>(ExrPixelSize(info.type));
        }
    }

    std::vector<uint8_t> unknownBuf(planarSize[kDwaUnknown]);
    std::vector<uint8_t> rleBuf(planarSize[kDwaRle]);
    {
        size_t unkOff = 0, rleOff = 0;
        for (size_t ci = 0; ci < numChan; ++ci) {
            DwaChannelState& cd = chans[ci];
            const int ps = ExrPixelSize(hdr.channels[ci].type);
            const size_t samples = static_cast<size_t>(std::max(cd.width, 0)) * static_cast<size_t>(std::max(cd.height, 0));
            if (cd.scheme == kDwaUnknown) {
                cd.planar = unknownBuf.data() + unkOff;
                unkOff += samples * static_cast<size_t>(ps);
            } else if (cd.scheme == kDwaRle) {
                for (int b = 0; b < ps; ++b) cd.rlePlanes[b] = rleBuf.data() + rleOff + static_cast<size_t>(b) * samples;
                rleOff += samples * static_cast<size_t>(ps);
            }
        }
    }

    const uint8_t* compressedUnknown = dataPtr;
    const uint8_t* compressedAc = compressedUnknown + sz[UNKNOWN_COMPRESSED_SIZE];
    const uint8_t* compressedDc = compressedAc + sz[AC_COMPRESSED_SIZE];
    const uint8_t* compressedRle = compressedDc + sz[DC_COMPRESSED_SIZE];

    if (sz[UNKNOWN_COMPRESSED_SIZE] > 0) {
        if (sz[UNKNOWN_UNCOMPRESSED_SIZE] > unknownBuf.size() ||
            !InflateExact(compressedUnknown, sz[UNKNOWN_COMPRESSED_SIZE], unknownBuf.data(),
                          sz[UNKNOWN_UNCOMPRESSED_SIZE])) {
            err = "DWA: failed to inflate uncompressed-channel data";
            return false;
        }
    }

    std::vector<uint16_t> acBuf;
    if (sz[AC_COMPRESSED_SIZE] > 0) {
        if (sz[AC_UNCOMPRESSED_COUNT] > numDctBlocks * 64) { err = "DWA: AC count is corrupt"; return false; }
        acBuf.resize(sz[AC_UNCOMPRESSED_COUNT]);
        bool ok = false;
        if (sz[AC_COMPRESSION] == kDwaStaticHuffman) {
            ok = tinyexr::hufUncompress(reinterpret_cast<const char*>(compressedAc),
                                        static_cast<int>(sz[AC_COMPRESSED_SIZE]), &acBuf);
        } else if (sz[AC_COMPRESSION] == kDwaDeflate) {
            std::vector<uint8_t> raw(acBuf.size() * 2);
            ok = InflateExact(compressedAc, sz[AC_COMPRESSED_SIZE], raw.data(), raw.size());
            for (size_t i = 0; ok && i < acBuf.size(); ++i) acBuf[i] = ReadLe16(raw.data() + i * 2);
        }
        if (!ok) { err = "DWA: failed to decode AC coefficients"; return false; }
    }

    std::vector<uint16_t> dcBuf;
    if (sz[DC_COMPRESSED_SIZE] > 0) {
        if (sz[DC_UNCOMPRESSED_COUNT] > numDctBlocks) { err = "DWA: DC count is corrupt"; return false; }
        std::vector<uint8_t> raw(sz[DC_UNCOMPRESSED_COUNT] * 2);
        if (!ZipUncompress(compressedDc, sz[DC_COMPRESSED_SIZE], raw.data(), raw.size())) {
            err = "DWA: failed to decode DC coefficients";
            return false;
        }
        dcBuf.resize(sz[DC_UNCOMPRESSED_COUNT]);
        for (size_t i = 0; i < dcBuf.size(); ++i) dcBuf[i] = ReadLe16(raw.data() + i * 2);
    } else if (sz[DC_UNCOMPRESSED_COUNT] != 0) {
        err = "DWA: DC data is corrupt";
        return false;
    }

    if (sz[RLE_RAW_SIZE] > 0) {
        if (sz[RLE_RAW_SIZE] > rleBuf.size() || sz[RLE_UNCOMPRESSED_SIZE] > rleBuf.size() * 2 + 64) {
            err = "DWA: RLE sizes are corrupt";
            return false;
        }
        std::vector<uint8_t> packed(sz[RLE_UNCOMPRESSED_SIZE]);
        if (!InflateExact(compressedRle, sz[RLE_COMPRESSED_SIZE], packed.data(), packed.size()) ||
            tinyexr::rleUncompress(static_cast<int>(packed.size()), static_cast<int>(sz[RLE_RAW_SIZE]),
                                   reinterpret_cast<const signed char*>(packed.data()),
                                   reinterpret_cast<char*>(rleBuf.data())) !=
                static_cast<int>(sz[RLE_RAW_SIZE])) {
            err = "DWA: failed to decode RLE channel data";
            return false;
        }
    }

    // Lay out the output rows.
    size_t outSize = 0;
    for (int y = minY; y <= maxY; ++y) {
        for (size_t ci = 0; ci < numChan; ++ci) {
            if (ModPositive(y, hdr.channels[ci].ySampling) != 0) continue;
            outSize += static_cast<size_t>(chans[ci].width) * static_cast<size_t>(ExrPixelSize(hdr.channels[ci].type));
        }
    }
    out.assign(outSize, 0);
    {
        size_t off = 0;
        for (int y = minY; y <= maxY; ++y) {
            for (size_t ci = 0; ci < numChan; ++ci) {
                if (ModPositive(y, hdr.channels[ci].ySampling) != 0) continue;
                chans[ci].rows.push_back(out.data() + off);
                off += static_cast<size_t>(chans[ci].width) * static_cast<size_t>(ExrPixelSize(hdr.channels[ci].type));
            }
        }
    }

    const uint16_t* ac = acBuf.data();
    const uint16_t* acEnd = acBuf.data() + acBuf.size();
    const uint16_t* dc = dcBuf.data();
    const uint16_t* dcEnd = dcBuf.data() + dcBuf.size();
    const uint16_t* toLinear = DwaToLinearTable().data();
    std::vector<bool> decoded(numChan, false);

    for (const auto& set : cscSets) {
        DwaChannelState* comps[3];
        int types[3];
        for (int c = 0; c < 3; ++c) {
            const size_t idx = static_cast<size_t>(set[static_cast<size_t>(c)]);
            if (chans[idx].scheme != kDwaLossyDct) { err = "DWA: bad compression type for colour set"; return false; }
            comps[c] = &chans[idx];
            types[c] = hdr.channels[idx].type;
            decoded[idx] = true;
        }
        if (!DecodeLossyDct(comps, types, 3, toLinear, ac, acEnd, dc, dcEnd)) {
            err = "DWA: lossy DCT data is corrupt";
            return false;
        }
    }

    for (size_t ci = 0; ci < numChan; ++ci) {
        if (decoded[ci]) continue;
        DwaChannelState& cd = chans[ci];
        const ExrChannelInfo& info = hdr.channels[ci];
        const int ps = ExrPixelSize(info.type);
        const size_t rowBytes = static_cast<size_t>(cd.width) * static_cast<size_t>(ps);
        if (cd.scheme == kDwaLossyDct) {
            DwaChannelState* comps[1] = {&cd};
            const int types[1] = {info.type};
            if (!DecodeLossyDct(comps, types, 1, info.pLinear ? nullptr : toLinear, ac, acEnd, dc, dcEnd)) {
                err = "DWA: lossy DCT data is corrupt";
                return false;
            }
        } else if (cd.scheme == kDwaRle) {
            for (size_t row = 0; row < cd.rows.size(); ++row) {
                uint8_t* dst = cd.rows[row];
                for (int x = 0; x < cd.width; ++x) {
                    for (int b = 0; b < ps; ++b) *dst++ = *cd.rlePlanes[b]++;
                }
            }
        } else {
            for (size_t row = 0; row < cd.rows.size(); ++row) {
                std::memcpy(cd.rows[row], cd.planar, rowBytes);
                cd.planar += rowBytes;
            }
        }
    }
    return true;
}

// Decodes a DWAA/DWAB scanline EXR into top-down float RGBA (the layout tinyexr's
// LoadEXRFromMemory returns), so the 8-bit conversion below is shared.
bool LoadDwaExrToRgbaFloat(const uint8_t* bytes,
                           size_t size,
                           int& outWidth,
                           int& outHeight,
                           std::vector<float>& outRgba,
                           std::string& err) {
    ExrScanlineHeader hdr;
    if (!ParseScanlineHeader(bytes, size, hdr, err)) return false;
    if (hdr.tiled) { err = "tiled DWA EXR images are not supported"; return false; }
    if (hdr.compression != 8 && hdr.compression != 9) { err = "not a DWA image"; return false; }

    const int width = hdr.maxX - hdr.minX + 1;
    const int height = hdr.maxY - hdr.minY + 1;
    const int linesPerChunk = hdr.compression == 8 ? 32 : 256;
    const int numChunks = (height + linesPerChunk - 1) / linesPerChunk;
    if (hdr.headerEnd + static_cast<size_t>(numChunks) * 8 > size) {
        err = "EXR offset table is truncated";
        return false;
    }

    // Which channel feeds which of R, G, B, A. Bare names win over layered ones
    // ("R" over "diffuse.R"); a lone luminance/data channel becomes grey.
    int src[4] = {-1, -1, -1, -1};
    {
        const char* names[4] = {"R", "G", "B", "A"};
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t ci = 0; ci < hdr.channels.size(); ++ci) {
                std::string n = hdr.channels[ci].name;
                if (pass == 1) {
                    const size_t dot = n.find_last_of('.');
                    if (dot == std::string::npos) continue;
                    n = n.substr(dot + 1);
                }
                for (int k = 0; k < 4; ++k) {
                    if (src[k] < 0 && n == names[k]) src[k] = static_cast<int>(ci);
                }
            }
        }
        if (src[0] < 0 && src[1] < 0 && src[2] < 0) {
            int grey = -1;
            for (size_t ci = 0; ci < hdr.channels.size() && grey < 0; ++ci) {
                if (hdr.channels[ci].name == "Y") grey = static_cast<int>(ci);
            }
            if (grey < 0) grey = 0;
            if (grey == src[3]) src[3] = -1;
            src[0] = src[1] = src[2] = grey;
        }
    }

    outRgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0.0f);
    for (size_t i = 3; i < outRgba.size(); i += 4) outRgba[i] = 1.0f;

    auto decodeChunk = [&](int chunk, std::vector<uint8_t>& scratch, std::string& chunkErr) -> bool {
        const uint64_t chunkOffset = ReadLe64(bytes + hdr.headerEnd + static_cast<size_t>(chunk) * 8);
        if (chunkOffset > size || size - chunkOffset < 8) { chunkErr = "EXR chunk offset is out of range"; return false; }
        const uint8_t* p = bytes + chunkOffset;
        const int chunkY = ReadLe32(p);
        const int32_t dataSize = ReadLe32(p + 4);
        if (dataSize < 0 || static_cast<uint64_t>(dataSize) > size - chunkOffset - 8) {
            chunkErr = "EXR chunk is truncated";
            return false;
        }
        if (chunkY < hdr.minY || chunkY > hdr.maxY) { chunkErr = "EXR chunk has an invalid y"; return false; }
        const int y0 = chunkY;
        const int y1 = std::min(hdr.maxY, y0 + linesPerChunk - 1);

        size_t expected = 0;
        for (int y = y0; y <= y1; ++y) {
            for (const ExrChannelInfo& ch : hdr.channels) {
                if (ModPositive(y, ch.ySampling) != 0) continue;
                expected += static_cast<size_t>(NumSamples(ch.xSampling, hdr.minX, hdr.maxX)) *
                            static_cast<size_t>(ExrPixelSize(ch.type));
            }
        }

        const uint8_t* rows = nullptr;
        if (static_cast<size_t>(dataSize) >= expected) {
            rows = p + 8; // the writer stored this chunk raw because it did not shrink
        } else {
            if (!DwaUncompressChunk(hdr, p + 8, static_cast<size_t>(dataSize), y0, y1, scratch, chunkErr)) return false;
            if (scratch.size() != expected) { chunkErr = "DWA chunk decoded to the wrong size"; return false; }
            rows = scratch.data();
        }

        // Scatter into the RGBA image, nearest-filling any subsampled channel.
        size_t off = 0;
        for (int y = y0; y <= y1; ++y) {
            for (size_t ci = 0; ci < hdr.channels.size(); ++ci) {
                const ExrChannelInfo& ch = hdr.channels[ci];
                if (ModPositive(y, ch.ySampling) != 0) continue;
                const int n = NumSamples(ch.xSampling, hdr.minX, hdr.maxX);
                const int ps = ExrPixelSize(ch.type);
                const uint8_t* s = rows + off;
                off += static_cast<size_t>(n) * static_cast<size_t>(ps);

                bool used = false;
                for (int k = 0; k < 4; ++k) used = used || src[k] == static_cast<int>(ci);
                if (!used) continue;

                const int firstSample = DivFloor(hdr.minX, ch.xSampling) + ((DivFloor(hdr.minX, ch.xSampling) * ch.xSampling < hdr.minX) ? 1 : 0);
                for (int i = 0; i < n; ++i) {
                    float v = 0.0f;
                    const uint8_t* sp = s + static_cast<size_t>(i) * static_cast<size_t>(ps);
                    if (ch.type == kExrHalf) {
                        v = HalfToFloat(ReadLe16(sp));
                    } else if (ch.type == kExrFloat) {
                        std::memcpy(&v, sp, 4);
                    } else {
                        v = static_cast<float>(static_cast<uint32_t>(ReadLe32(sp)));
                    }
                    const int px0 = (firstSample + i) * ch.xSampling - hdr.minX;
                    for (int dy = 0; dy < ch.ySampling; ++dy) {
                        const int py = y + dy - hdr.minY;
                        if (py < 0 || py >= height) continue;
                        for (int dx = 0; dx < ch.xSampling; ++dx) {
                            const int px = px0 + dx;
                            if (px < 0 || px >= width) continue;
                            float* dst = &outRgba[(static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)) * 4u];
                            for (int k = 0; k < 4; ++k) {
                                if (src[k] == static_cast<int>(ci)) dst[k] = v;
                            }
                        }
                    }
                }
            }
        }
        return true;
    };

    // Chunks are independent and write disjoint rows, so decode them in parallel -
    // a 4k map is 128 DWAA chunks and takes a noticeable moment single-threaded.
    std::atomic<int> nextChunk(0);
    std::atomic<bool> failed(false);
    std::mutex errMutex;
    auto worker = [&]() {
        std::vector<uint8_t> scratch;
        std::string chunkErr;
        for (;;) {
            const int chunk = nextChunk.fetch_add(1);
            if (chunk >= numChunks || failed.load()) return;
            if (!decodeChunk(chunk, scratch, chunkErr)) {
                std::lock_guard<std::mutex> lock(errMutex);
                if (!failed.exchange(true)) err = chunkErr;
                return;
            }
        }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int threadCount = static_cast<int>(std::min<unsigned>(hw, static_cast<unsigned>(std::max(numChunks, 1)))) - 1;
    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; ++i) {
        try {
            threads.emplace_back(worker);
        } catch (...) {
            break; // fall back to fewer threads; the calling thread still works the queue
        }
    }
    worker();
    for (std::thread& t : threads) t.join();
    if (failed.load()) return false;

    outWidth = width;
    outHeight = height;
    return true;
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

    float* tinyRgba = nullptr; // malloc'd by tinyexr
    std::vector<float> dwaRgba;
    const float* rgba = nullptr;
    int width = 0;
    int height = 0;

    const int compression = ReadExrCompression(bytes, size);
    if (compression == 8 || compression == 9) {
        // DWAA / DWAB: tinyexr cannot decode these, use the in-tree decoder.
        std::string dwaErr;
        if (!LoadDwaExrToRgbaFloat(bytes, size, width, height, dwaRgba, dwaErr)) {
            outError = dwaErr + " [compression: " + ExrCompressionName(compression) + "]";
            return false;
        }
        rgba = dwaRgba.data();
    } else {
        const char* err = nullptr;
        const int result = LoadEXRFromMemory(&tinyRgba, &width, &height, bytes, size, &err);
        if (result != TINYEXR_SUCCESS || tinyRgba == nullptr) {
            outError = err ? err : "unknown EXR decode failure";
            if (err) {
                FreeEXRErrorMessage(err);
            }
            if (compression >= 0) {
                outError += std::string(" [compression: ") + ExrCompressionName(compression) + "]";
            }
            std::free(tinyRgba);
            return false;
        }
        if (err) {
            FreeEXRErrorMessage(err);
        }
        rgba = tinyRgba;
    }

    if (width <= 0 || height <= 0) {
        outError = "EXR reported a zero-sized image";
        std::free(tinyRgba);
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

    std::free(tinyRgba);

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
