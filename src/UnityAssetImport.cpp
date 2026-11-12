#include "UnityAssetImport.h"
#include "EngineMaterialIO.h"

#include "../include/Platform/AssetSource.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace UnityAssetImport {

namespace {

namespace stdfs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// File access
// ---------------------------------------------------------------------------------------------

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string LowerExtension(const std::string& path) {
    return LowerAscii(stdfs::path(path).extension().string());
}

// First `maxBytes` of a file. Goes through the AssetSource so a mounted bundle works, and falls
// back to the filesystem for loose files the source does not know (it logs failed reads, which
// would flood the log when the file browser sniffs files that are not there).
bool ReadHead(const std::string& path, size_t maxBytes, std::string& out) {
    out.clear();
    Modularity::Platform::AssetSource& source = Modularity::Platform::GetAssetSource();
    if (source.Exists(path)) {
        std::unique_ptr<Modularity::Platform::AssetStream> stream = source.Open(path);
        if (stream) {
            out.resize(maxBytes);
            const size_t got = stream->Read(&out[0], maxBytes);
            out.resize(got);
            return got > 0;
        }
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    out.resize(maxBytes);
    file.read(&out[0], static_cast<std::streamsize>(maxBytes));
    out.resize(static_cast<size_t>(std::max<std::streamsize>(0, file.gcount())));
    return !out.empty();
}

bool ReadWhole(const std::string& path, std::string& out) {
    out.clear();
    Modularity::Platform::AssetSource& source = Modularity::Platform::GetAssetSource();
    if (source.Exists(path)) {
        std::vector<uint8_t> bytes = source.ReadAll(path);
        if (!bytes.empty()) {
            out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return true;
        }
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    file.read(&out[0], static_cast<std::streamsize>(size));
    return file.gcount() == size;
}

// Loose-file read for the project scan (prefabs, materials, .meta). These always sit in a real
// Unity folder, never in a bundle, so skip the AssetSource and its failure logging.
bool ReadLoose(const stdfs::path& path, std::string& out, size_t maxBytes = 0) {
    out.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) return false;
    file.seekg(0, std::ios::beg);
    size_t want = static_cast<size_t>(size);
    if (maxBytes > 0 && want > maxBytes) want = maxBytes;
    out.resize(want);
    if (want > 0) file.read(&out[0], static_cast<std::streamsize>(want));
    out.resize(static_cast<size_t>(std::max<std::streamsize>(0, file.gcount())));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Line reader and scalar helpers for Unity's YAML
// ---------------------------------------------------------------------------------------------

struct Line {
    const char* b = nullptr;  // start of the content (after the indentation)
    const char* e = nullptr;  // end, without the line break
    int indent = 0;
    bool empty() const { return b == e; }
    size_t size() const { return static_cast<size_t>(e - b); }
};

class Lines {
public:
    Lines(const char* begin, const char* end) : p_(begin), end_(end) {}
    bool next(Line& out) {
        if (p_ >= end_) return false;
        const char* s = p_;
        const char* nl = static_cast<const char*>(std::memchr(s, '\n', static_cast<size_t>(end_ - s)));
        const char* e = nl ? nl : end_;
        p_ = nl ? nl + 1 : end_;
        const char* le = e;
        if (le > s && le[-1] == '\r') --le;
        int indent = 0;
        while (s < le && *s == ' ') { ++s; ++indent; }
        out.b = s;
        out.e = le;
        out.indent = indent;
        return true;
    }
    // Look at the following line without consuming it.
    bool peek(Line& out) const {
        Lines copy = *this;
        return copy.next(out);
    }
private:
    const char* p_;
    const char* end_;
};

bool StartsWith(const Line& l, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return l.size() >= n && std::memcmp(l.b, prefix, n) == 0;
}

// "key: value" -> key and value (value trimmed). False when there is no colon.
bool SplitKeyValue(const Line& l, std::string& key, const char*& vb, const char*& ve) {
    const char* colon = nullptr;
    for (const char* c = l.b; c < l.e; ++c) {
        if (*c == ':' && (c + 1 == l.e || c[1] == ' ')) { colon = c; break; }
    }
    if (!colon) return false;
    key.assign(l.b, colon);
    vb = colon + 1;
    ve = l.e;
    while (vb < ve && (*vb == ' ' || *vb == '\t')) ++vb;
    while (ve > vb && (ve[-1] == ' ' || ve[-1] == '\t')) --ve;
    return true;
}

double ParseDouble(const char* b, const char* e, double fallback = 0.0) {
    if (b >= e) return fallback;
    std::string tmp(b, e);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    return (end == tmp.c_str()) ? fallback : v;
}

long long ParseInt(const char* b, const char* e, long long fallback = 0) {
    if (b >= e) return fallback;
    std::string tmp(b, e);
    char* end = nullptr;
    const long long v = std::strtoll(tmp.c_str(), &end, 10);
    return (end == tmp.c_str()) ? fallback : v;
}

// Number following `tag` (for example "r:") inside `{r: 1, g: 0.5, ...}`.
bool NumberAfter(const char* b, const char* e, const char* tag, double& out) {
    const size_t n = std::strlen(tag);
    for (const char* c = b; c + n <= e; ++c) {
        if (std::memcmp(c, tag, n) == 0 && (c == b || c[-1] == '{' || c[-1] == ' ' || c[-1] == ',')) {
            const char* v = c + n;
            while (v < e && *v == ' ') ++v;
            const char* ve = v;
            while (ve < e && *ve != ',' && *ve != '}' && *ve != ' ') ++ve;
            out = ParseDouble(v, ve);
            return true;
        }
    }
    return false;
}

// 32-character hex guid after "guid: " inside a reference like {fileID: 2800000, guid: ..., type: 3}.
std::string GuidIn(const char* b, const char* e) {
    static const char kTag[] = "guid: ";
    const size_t n = sizeof(kTag) - 1;
    for (const char* c = b; c + n + 32 <= e; ++c) {
        if (std::memcmp(c, kTag, n) == 0) {
            return std::string(c + n, c + n + 32);
        }
    }
    return std::string();
}

long long FileIdIn(const char* b, const char* e) {
    static const char kTag[] = "fileID: ";
    const size_t n = sizeof(kTag) - 1;
    for (const char* c = b; c + n < e; ++c) {
        if (std::memcmp(c, kTag, n) == 0) {
            const char* v = c + n;
            const char* ve = v;
            while (ve < e && (*ve == '-' || std::isdigit(static_cast<unsigned char>(*ve)))) ++ve;
            return ParseInt(v, ve);
        }
    }
    return 0;
}

bool IsHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Decodes hex text, appending to `out`. False on a stray character or an odd digit count.
bool AppendHex(const char* b, const char* e, std::vector<uint8_t>& out) {
    static int8_t table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        for (int i = 0; i < 10; ++i) table['0' + i] = static_cast<int8_t>(i);
        for (int i = 0; i < 6; ++i) {
            table['a' + i] = static_cast<int8_t>(10 + i);
            table['A' + i] = static_cast<int8_t>(10 + i);
        }
        tableReady = true;
    }
    const size_t count = static_cast<size_t>(e - b);
    if (count % 2 != 0) return false;
    const size_t base = out.size();
    out.resize(base + count / 2);
    uint8_t* dst = out.data() + base;
    for (size_t i = 0; i < count; i += 2) {
        const int hi = table[static_cast<unsigned char>(b[i])];
        const int lo = table[static_cast<unsigned char>(b[i + 1])];
        if (hi < 0 || lo < 0) return false;
        *dst++ = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool LineIsAllHex(const Line& l) {
    if (l.empty()) return false;
    for (const char* c = l.b; c < l.e; ++c) {
        if (!IsHexChar(*c)) return false;
    }
    return true;
}

// Decodes the hex value of a field and any wrapped continuation lines.
bool ReadHexField(Lines& lines, const char* vb, const char* ve, int fieldIndent, std::vector<uint8_t>& out) {
    out.clear();
    if (!AppendHex(vb, ve, out)) return false;
    Line peeked;
    while (lines.peek(peeked) && peeked.indent > fieldIndent && LineIsAllHex(peeked)) {
        Line consumed;
        lines.next(consumed);
        if (!AppendHex(consumed.b, consumed.e, out)) return false;
    }
    return true;
}

// The first "--- !u!<id>" header in the text: its class id, or -1.
int FirstClassId(const char* b, const char* e) {
    static const char kTag[] = "--- !u!";
    const size_t n = sizeof(kTag) - 1;
    for (const char* c = b; c + n < e; ++c) {
        if ((c == b || c[-1] == '\n') && std::memcmp(c, kTag, n) == 0) {
            const char* v = c + n;
            const char* ve = v;
            while (ve < e && std::isdigit(static_cast<unsigned char>(*ve))) ++ve;
            if (ve > v) return static_cast<int>(ParseInt(v, ve));
            return -1;
        }
    }
    return -1;
}

bool HasYamlHeader(const std::string& head) {
    size_t i = 0;
    if (head.size() >= 3 && static_cast<unsigned char>(head[0]) == 0xEF &&
        static_cast<unsigned char>(head[1]) == 0xBB && static_cast<unsigned char>(head[2]) == 0xBF) {
        i = 3;
    }
    return head.compare(i, 5, "%YAML") == 0 || head.compare(i, 7, "--- !u!") == 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Sniffing
// ---------------------------------------------------------------------------------------------

AssetKind SniffAsset(const std::string& path) {
    std::string head;
    if (!ReadHead(path, 512, head)) return AssetKind::Unknown;
    if (!HasYamlHeader(head)) return AssetKind::Other;
    if (LowerExtension(path) == ".prefab") return AssetKind::Prefab;
    switch (FirstClassId(head.data(), head.data() + head.size())) {
        case 43: return AssetKind::Mesh;
        case 187: return AssetKind::Texture2DArray;
        case 21: return AssetKind::Material;
        default: return AssetKind::Other;
    }
}

bool IsUnityYamlFile(const std::string& path) {
    std::string head;
    if (!ReadHead(path, 16, head)) return false;
    size_t i = 0;
    if (head.size() >= 3 && static_cast<unsigned char>(head[0]) == 0xEF &&
        static_cast<unsigned char>(head[1]) == 0xBB && static_cast<unsigned char>(head[2]) == 0xBF) {
        i = 3;
    }
    return head.compare(i, 5, "%YAML") == 0;
}

// ---------------------------------------------------------------------------------------------
// Mesh
// ---------------------------------------------------------------------------------------------

namespace {

struct UChannel {
    int stream = 0;
    int offset = 0;
    int format = 0;
    int dimension = 0;
};

struct USubMesh {
    uint32_t firstByte = 0;
    uint32_t indexCount = 0;
    uint32_t topology = 0;
    uint32_t baseVertex = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
};

struct UnityMeshRaw {
    std::string name;
    int serializedVersion = 0;
    uint32_t vertexCount = 0;
    uint32_t dataSize = 0;
    int indexFormat = 0;  // 0 uint16, 1 uint32
    int compression = 0;
    std::vector<UChannel> channels;
    std::vector<USubMesh> subs;
    std::vector<uint8_t> indexBytes;
    std::vector<uint8_t> vertexBytes;
};

// Channel indices in the 14-channel layout (Unity 2018+).
constexpr int kChanPos = 0;
constexpr int kChanNormal = 1;
constexpr int kChanColor = 3;
constexpr int kChanUV0 = 4;
constexpr int kChanUV1 = 5;
constexpr int kChanUV2 = 6;

// Unity's VertexAttributeFormat as of 2019: 0 F32, 1 F16, 2 UNorm8, 3 SNorm8, 4 UNorm16,
// 5 SNorm16, 6 UInt8, 7 SInt8, 8 UInt16, 9 SInt16, 10 UInt32, 11 SInt32.
// Mesh serializedVersion 9 and older used the 2017 enum, where 2 is "Color" (RGBA8, the same
// bytes as UNorm8) and everything after it sits one slot higher.
int NormalizeFormat(int raw, int serializedVersion) {
    if (serializedVersion >= 10) return raw;
    if (raw <= 2) return raw;
    return raw - 1;
}

int FormatSize(int format) {
    switch (format) {
        case 0: return 4;
        case 1: return 2;
        case 2: case 3: case 6: case 7: return 1;
        case 4: case 5: case 8: case 9: return 2;
        case 10: case 11: return 4;
        default: return 0;
    }
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    float value;
    if (exp == 0) {
        value = std::ldexp(static_cast<float>(mant), -24);
    } else if (exp == 31) {
        value = mant ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    } else {
        value = std::ldexp(static_cast<float>(mant | 0x400u), static_cast<int>(exp) - 25);
    }
    return sign ? -value : value;
}

float DecodeComponent(const uint8_t* p, int format) {
    switch (format) {
        case 0: { float f; std::memcpy(&f, p, 4); return f; }
        case 1: { uint16_t h; std::memcpy(&h, p, 2); return HalfToFloat(h); }
        case 2: return static_cast<float>(p[0]) / 255.0f;
        case 3: return std::max(static_cast<float>(static_cast<int8_t>(p[0])) / 127.0f, -1.0f);
        case 4: { uint16_t v; std::memcpy(&v, p, 2); return static_cast<float>(v) / 65535.0f; }
        case 5: { int16_t v; std::memcpy(&v, p, 2); return std::max(static_cast<float>(v) / 32767.0f, -1.0f); }
        case 6: return static_cast<float>(p[0]);
        case 7: return static_cast<float>(static_cast<int8_t>(p[0]));
        case 8: { uint16_t v; std::memcpy(&v, p, 2); return static_cast<float>(v); }
        case 9: { int16_t v; std::memcpy(&v, p, 2); return static_cast<float>(v); }
        case 10: { uint32_t v; std::memcpy(&v, p, 4); return static_cast<float>(v); }
        case 11: { int32_t v; std::memcpy(&v, p, 4); return static_cast<float>(v); }
        default: return 0.0f;
    }
}

bool ParseMeshYaml(const std::string& text, UnityMeshRaw& m, std::string& error) {
    Lines lines(text.data(), text.data() + text.size());
    Line l;
    bool sawHeader = false;
    bool sawMesh = false;
    bool inFirstDoc = false;
    bool haveUse16 = false;
    bool use16 = true;
    std::string section;
    std::string key;
    const char* vb = nullptr;
    const char* ve = nullptr;

    while (lines.next(l)) {
        if (StartsWith(l, "--- ")) {
            if (inFirstDoc) break;  // only the first document is the mesh
            if (l.indent == 0 && StartsWith(l, "--- !u!43 ")) {
                sawHeader = true;
                inFirstDoc = true;
            } else {
                // A different class first: not a mesh.
                error = "not a Unity Mesh asset (class 43)";
                return false;
            }
            continue;
        }
        if (!sawHeader) continue;
        if (l.empty()) continue;
        if (!sawMesh) {
            if (l.indent == 0 && StartsWith(l, "Mesh:")) sawMesh = true;
            continue;
        }

        if (l.indent == 2 && !StartsWith(l, "- ") && SplitKeyValue(l, key, vb, ve)) {
            section = key;
            if (key == "serializedVersion") m.serializedVersion = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_Name") m.name.assign(vb, ve);
            else if (key == "m_IndexFormat") m.indexFormat = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_Use16BitIndices") { haveUse16 = true; use16 = ParseInt(vb, ve) != 0; }
            else if (key == "m_MeshCompression") m.compression = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_IndexBuffer") {
                if (!ReadHexField(lines, vb, ve, 2, m.indexBytes)) {
                    error = "m_IndexBuffer is not valid hex";
                    return false;
                }
            }
            continue;
        }

        if (section == "m_SubMeshes") {
            if (StartsWith(l, "- serializedVersion")) {
                m.subs.emplace_back();
            } else if (!m.subs.empty() && l.indent == 4 && SplitKeyValue(l, key, vb, ve)) {
                USubMesh& s = m.subs.back();
                const uint32_t v = static_cast<uint32_t>(ParseInt(vb, ve));
                if (key == "firstByte") s.firstByte = v;
                else if (key == "indexCount") s.indexCount = v;
                else if (key == "topology") s.topology = v;
                else if (key == "baseVertex") s.baseVertex = v;
                else if (key == "firstVertex") s.firstVertex = v;
                else if (key == "vertexCount") s.vertexCount = v;
            }
        } else if (section == "m_VertexData") {
            if (StartsWith(l, "- stream:")) {
                m.channels.emplace_back();
                SplitKeyValue(l, key, vb, ve);  // "- stream" key; value follows the colon
                m.channels.back().stream = static_cast<int>(ParseInt(vb, ve));
            } else if (l.indent == 6 && !m.channels.empty() && SplitKeyValue(l, key, vb, ve)) {
                UChannel& c = m.channels.back();
                if (key == "offset") c.offset = static_cast<int>(ParseInt(vb, ve));
                else if (key == "format") c.format = static_cast<int>(ParseInt(vb, ve));
                else if (key == "dimension") c.dimension = static_cast<int>(ParseInt(vb, ve));
            } else if (l.indent == 4 && SplitKeyValue(l, key, vb, ve)) {
                if (key == "m_VertexCount") m.vertexCount = static_cast<uint32_t>(ParseInt(vb, ve));
                else if (key == "m_DataSize") m.dataSize = static_cast<uint32_t>(ParseInt(vb, ve));
                else if (key == "_typelessdata") {
                    if (!ReadHexField(lines, vb, ve, 4, m.vertexBytes)) {
                        error = "_typelessdata is not valid hex";
                        return false;
                    }
                }
            }
        }
    }

    if (!sawHeader || !sawMesh) {
        error = "not a Unity Mesh asset (class 43)";
        return false;
    }
    if (haveUse16) m.indexFormat = use16 ? 0 : 1;
    if (m.compression != 0) {
        error = "compressed Unity meshes (m_MeshCompression) are not supported; turn mesh compression off in Unity and re-save the asset";
        return false;
    }
    if (m.vertexBytes.empty() || m.indexBytes.empty() || m.subs.empty() || m.vertexCount == 0) {
        error = "Unity mesh has no readable vertex/index data (compressed or binary-serialized)";
        return false;
    }
    return true;
}

struct DecodedVertex {
    glm::vec3 pos;
    glm::vec3 nrm;
    glm::vec2 uv0;
    glm::vec4 col;
    glm::vec2 uv1;
    glm::vec3 uv2;
};

bool BuildMeshData(const UnityMeshRaw& m, MeshData& out, std::string& error) {
    if (m.channels.size() == 8) {
        error = "legacy 8-channel Unity vertex layout (Unity 5/2017 or older) is not supported; re-save the mesh in Unity 2018 or newer";
        return false;
    }
    if (m.channels.size() != 14) {
        error = "unexpected Unity vertex channel count " + std::to_string(m.channels.size()) + " (expected 14)";
        return false;
    }

    // Channels in use, with the format normalised to the modern enum.
    struct Chan { bool present = false; int stream = 0; int offset = 0; int format = 0; int dim = 0; };
    std::vector<Chan> chans(m.channels.size());
    int streamStride[4] = {0, 0, 0, 0};
    int lastStream = 0;
    for (size_t i = 0; i < m.channels.size(); ++i) {
        const UChannel& c = m.channels[i];
        const int dim = c.dimension & 0xF;
        if (dim <= 0) continue;
        const int fmt = NormalizeFormat(c.format, m.serializedVersion);
        const int fsize = FormatSize(fmt);
        if (fsize == 0) {
            error = "unsupported Unity vertex format " + std::to_string(c.format) + " on channel " + std::to_string(i);
            return false;
        }
        if (c.stream < 0 || c.stream > 3) {
            error = "unsupported Unity vertex stream " + std::to_string(c.stream);
            return false;
        }
        Chan& ch = chans[i];
        ch.present = true;
        ch.stream = c.stream;
        ch.offset = c.offset;
        ch.format = fmt;
        ch.dim = dim;
        streamStride[c.stream] = std::max(streamStride[c.stream], c.offset + fsize * dim);
        lastStream = std::max(lastStream, c.stream);
    }
    if (!chans[kChanPos].present || chans[kChanPos].dim < 3) {
        error = "Unity mesh has no position channel";
        return false;
    }

    // Streams are laid out one after another, each starting on a 16 byte boundary; the last one
    // is not padded.
    uint64_t streamStart[4] = {0, 0, 0, 0};
    for (int s = 1; s <= 3; ++s) {
        const uint64_t prevEnd = streamStart[s - 1] + static_cast<uint64_t>(m.vertexCount) * streamStride[s - 1];
        streamStart[s] = (prevEnd + 15u) & ~static_cast<uint64_t>(15u);
    }
    const uint64_t expected = streamStart[lastStream] + static_cast<uint64_t>(m.vertexCount) * streamStride[lastStream];
    if (expected != m.vertexBytes.size()) {
        error = "Unity vertex data is " + std::to_string(m.vertexBytes.size()) +
                " bytes but the channel layout needs " + std::to_string(expected);
        return false;
    }
    if (m.dataSize != 0 && m.dataSize != m.vertexBytes.size()) {
        error = "Unity m_DataSize (" + std::to_string(m.dataSize) + ") does not match the vertex data (" +
                std::to_string(m.vertexBytes.size()) + ")";
        return false;
    }

    // Reads channel `ci` of vertex `v` into out[0..3]; components the channel lacks keep `def`.
    auto readChannel = [&](int ci, uint32_t v, float* outv, const float* def) {
        for (int k = 0; k < 4; ++k) outv[k] = def[k];
        const Chan& ch = chans[static_cast<size_t>(ci)];
        if (!ch.present) return;
        const uint8_t* p = m.vertexBytes.data() + streamStart[ch.stream] +
                           static_cast<uint64_t>(v) * streamStride[ch.stream] + ch.offset;
        const int fsize = FormatSize(ch.format);
        for (int k = 0; k < ch.dim && k < 4; ++k) {
            outv[k] = DecodeComponent(p + k * fsize, ch.format);
        }
    };

    const bool hasNormal = chans[kChanNormal].present;
    const bool hasColor = chans[kChanColor].present;
    const bool hasUV0 = chans[kChanUV0].present;
    const bool hasUV1 = chans[kChanUV1].present;
    const bool hasUV2 = chans[kChanUV2].present;
    const int uv2Components = hasUV2 ? (chans[kChanUV2].dim >= 3 ? 3 : 2) : 0;

    const size_t indexSize = (m.indexFormat == 0) ? 2u : 4u;
    out.name = m.name;
    out.submeshes.clear();
    out.submeshes.reserve(m.subs.size());

    for (size_t si = 0; si < m.subs.size(); ++si) {
        const USubMesh& sub = m.subs[si];
        const std::string where = "submesh " + std::to_string(si);
        if (sub.topology != 0 && sub.topology != 2) {
            error = where + " uses mesh topology " + std::to_string(sub.topology) +
                    " (only triangles and quads are supported)";
            return false;
        }
        if (sub.indexCount == 0) {
            out.submeshes.emplace_back();
            continue;
        }
        if (sub.firstByte % indexSize != 0 ||
            static_cast<uint64_t>(sub.firstByte) + static_cast<uint64_t>(sub.indexCount) * indexSize > m.indexBytes.size()) {
            error = where + " index range lies outside the index buffer";
            return false;
        }
        if ((sub.topology == 0 && sub.indexCount % 3 != 0) || (sub.topology == 2 && sub.indexCount % 4 != 0)) {
            error = where + " index count does not match its topology";
            return false;
        }

        // Absolute vertex index of every corner of every triangle (quads split a-b-c, a-c-d).
        std::vector<uint32_t> tri;
        tri.reserve(sub.topology == 2 ? sub.indexCount / 4u * 6u : sub.indexCount);
        std::vector<uint32_t> raw(sub.indexCount);
        const uint8_t* ip = m.indexBytes.data() + sub.firstByte;
        uint32_t lo = 0xFFFFFFFFu;
        uint32_t hi = 0;
        for (uint32_t j = 0; j < sub.indexCount; ++j) {
            uint32_t value;
            if (indexSize == 2) {
                uint16_t v16;
                std::memcpy(&v16, ip + static_cast<size_t>(j) * 2, 2);
                value = v16;
            } else {
                std::memcpy(&value, ip + static_cast<size_t>(j) * 4, 4);
            }
            value += sub.baseVertex;
            if (value >= m.vertexCount) {
                error = where + " has an index past the vertex count";
                return false;
            }
            raw[j] = value;
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
        if (sub.topology == 0) {
            tri = raw;
        } else {
            for (uint32_t q = 0; q + 3 < sub.indexCount; q += 4) {
                tri.push_back(raw[q]); tri.push_back(raw[q + 1]); tri.push_back(raw[q + 2]);
                tri.push_back(raw[q]); tri.push_back(raw[q + 2]); tri.push_back(raw[q + 3]);
            }
        }

        // Decode the vertex range this submesh touches once.
        const uint32_t rangeCount = hi - lo + 1;
        std::vector<DecodedVertex> verts(rangeCount);
        static const float kZero[4] = {0, 0, 0, 0};
        static const float kOne[4] = {1, 1, 1, 1};
        for (uint32_t v = 0; v < rangeCount; ++v) {
            float tmp[4];
            DecodedVertex& d = verts[v];
            readChannel(kChanPos, lo + v, tmp, kZero);
            d.pos = glm::vec3(-tmp[0], tmp[1], tmp[2]);  // Unity -> Modularity: mirror x
            readChannel(kChanNormal, lo + v, tmp, kZero);
            d.nrm = glm::vec3(-tmp[0], tmp[1], tmp[2]);
            readChannel(kChanUV0, lo + v, tmp, kZero);
            d.uv0 = glm::vec2(tmp[0], tmp[1]);
            readChannel(kChanColor, lo + v, tmp, kOne);
            d.col = glm::vec4(tmp[0], tmp[1], tmp[2], tmp[3]);
            readChannel(kChanUV1, lo + v, tmp, kZero);
            d.uv1 = glm::vec2(tmp[0], tmp[1]);
            readChannel(kChanUV2, lo + v, tmp, kZero);
            d.uv2 = glm::vec3(tmp[0], tmp[1], tmp[2]);
        }

        MeshData::Submesh sm;
        const size_t corners = tri.size();
        sm.vertices.reserve(corners * 8);
        if (hasColor) sm.colors.reserve(corners * 4);
        if (hasUV1) sm.uv1.reserve(corners * 2);
        if (hasUV2) sm.uv2.reserve(corners * static_cast<size_t>(uv2Components));
        sm.uv2Components = uv2Components;

        sm.positions.resize(rangeCount);
        for (uint32_t v = 0; v < rangeCount; ++v) sm.positions[v] = verts[v].pos;
        sm.triangleIndices.reserve(corners);

        sm.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        sm.boundsMax = glm::vec3(-std::numeric_limits<float>::max());

        for (size_t t = 0; t + 2 < corners; t += 3) {
            // Mirroring x reverses the winding, so swap the last two corners to restore it.
            const uint32_t order[3] = {tri[t], tri[t + 2], tri[t + 1]};
            glm::vec3 faceNormal(0.0f, 1.0f, 0.0f);
            if (!hasNormal) {
                const glm::vec3 a = verts[order[0] - lo].pos;
                const glm::vec3 b = verts[order[1] - lo].pos;
                const glm::vec3 c = verts[order[2] - lo].pos;
                const glm::vec3 n = glm::cross(b - a, c - a);
                const float len = glm::length(n);
                if (len > 1e-12f) faceNormal = n / len;
            }
            for (int k = 0; k < 3; ++k) {
                const DecodedVertex& d = verts[order[k] - lo];
                const glm::vec3 n = hasNormal ? d.nrm : faceNormal;
                sm.vertices.push_back(d.pos.x); sm.vertices.push_back(d.pos.y); sm.vertices.push_back(d.pos.z);
                sm.vertices.push_back(n.x); sm.vertices.push_back(n.y); sm.vertices.push_back(n.z);
                sm.vertices.push_back(hasUV0 ? d.uv0.x : 0.0f);
                sm.vertices.push_back(hasUV0 ? d.uv0.y : 0.0f);
                if (hasColor) {
                    sm.colors.push_back(d.col.r); sm.colors.push_back(d.col.g);
                    sm.colors.push_back(d.col.b); sm.colors.push_back(d.col.a);
                }
                if (hasUV1) { sm.uv1.push_back(d.uv1.x); sm.uv1.push_back(d.uv1.y); }
                if (hasUV2) {
                    sm.uv2.push_back(d.uv2.x); sm.uv2.push_back(d.uv2.y);
                    if (uv2Components == 3) sm.uv2.push_back(d.uv2.z);
                }
                sm.triangleIndices.push_back(order[k] - lo);
                sm.boundsMin = glm::min(sm.boundsMin, d.pos);
                sm.boundsMax = glm::max(sm.boundsMax, d.pos);
            }
        }

        out.submeshes.push_back(std::move(sm));
    }
    return true;
}

} // namespace

bool LoadMesh(const std::string& path, MeshData& out, std::string& error) {
    out = MeshData();
    error.clear();
    std::string text;
    if (!ReadWhole(path, text)) {
        error = "could not read " + path;
        return false;
    }
    UnityMeshRaw raw;
    if (!ParseMeshYaml(text, raw, error)) return false;
    text.clear();
    text.shrink_to_fit();
    return BuildMeshData(raw, out, error);
}

// ---------------------------------------------------------------------------------------------
// Texture2DArray
// ---------------------------------------------------------------------------------------------

namespace {

size_t FormatBytesForMip(int format, int w, int h) {
    switch (format) {
        case 3: return static_cast<size_t>(w) * h * 3;
        case 4: case 5: return static_cast<size_t>(w) * h * 4;
        case 10: return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 8;
        case 12: return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 16;
        default: return 0;
    }
}

const char* TextureFormatName(int format) {
    switch (format) {
        case 1: return "Alpha8";
        case 2: return "ARGB4444";
        case 3: return "RGB24";
        case 4: return "RGBA32";
        case 5: return "ARGB32";
        case 7: return "RGB565";
        case 9: return "R16";
        case 10: return "DXT1";
        case 12: return "DXT5";
        case 13: return "RGBA4444";
        case 14: return "BGRA32";
        case 15: return "RHalf";
        case 16: return "RGHalf";
        case 17: return "RGBAHalf";
        case 18: return "RFloat";
        case 19: return "RGFloat";
        case 20: return "RGBAFloat";
        case 24: return "BC6H";
        case 25: return "BC7";
        case 28: return "DXT1Crunched";
        case 29: return "DXT5Crunched";
        default: return "an unknown format";
    }
}

struct Rgba { uint8_t r, g, b, a; };

Rgba Expand565(uint16_t c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return { static_cast<uint8_t>((r << 3) | (r >> 2)),
             static_cast<uint8_t>((g << 2) | (g >> 4)),
             static_cast<uint8_t>((b << 3) | (b >> 2)), 255 };
}

// Writes the 16 colours of a BC1 colour block. `forceFourColor` is the BC3 rule: its colour block
// has no punch-through alpha mode.
void DecodeBc1Colors(const uint8_t* block, bool forceFourColor, Rgba out[16]) {
    uint16_t c0, c1;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    Rgba p[4];
    p[0] = Expand565(c0);
    p[1] = Expand565(c1);
    if (c0 > c1 || forceFourColor) {
        for (int i = 0; i < 3; ++i) {
            const int a = (i == 0) ? p[0].r : (i == 1 ? p[0].g : p[0].b);
            const int b = (i == 0) ? p[1].r : (i == 1 ? p[1].g : p[1].b);
            uint8_t& d2 = (i == 0) ? p[2].r : (i == 1 ? p[2].g : p[2].b);
            uint8_t& d3 = (i == 0) ? p[3].r : (i == 1 ? p[3].g : p[3].b);
            d2 = static_cast<uint8_t>((2 * a + b) / 3);
            d3 = static_cast<uint8_t>((a + 2 * b) / 3);
        }
        p[2].a = 255;
        p[3].a = 255;
    } else {
        p[2].r = static_cast<uint8_t>((p[0].r + p[1].r) / 2);
        p[2].g = static_cast<uint8_t>((p[0].g + p[1].g) / 2);
        p[2].b = static_cast<uint8_t>((p[0].b + p[1].b) / 2);
        p[2].a = 255;
        p[3] = { 0, 0, 0, 0 };
    }
    uint32_t bits;
    std::memcpy(&bits, block + 4, 4);
    for (int i = 0; i < 16; ++i) out[i] = p[(bits >> (2 * i)) & 3];
}

void DecodeBc3Alpha(const uint8_t* block, uint8_t out[16]) {
    const int a0 = block[0], a1 = block[1];
    uint8_t pal[8];
    pal[0] = static_cast<uint8_t>(a0);
    pal[1] = static_cast<uint8_t>(a1);
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) pal[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i <= 4; ++i) pal[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

// Decodes mip 0 of one slice into dst (w*h RGBA8) in the order the data is stored.
void DecodeSliceMip0(int format, const uint8_t* src, int w, int h, uint8_t* dst) {
    switch (format) {
        case 3:
            for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
                dst[i * 4 + 0] = src[i * 3 + 0];
                dst[i * 4 + 1] = src[i * 3 + 1];
                dst[i * 4 + 2] = src[i * 3 + 2];
                dst[i * 4 + 3] = 255;
            }
            return;
        case 4:
            std::memcpy(dst, src, static_cast<size_t>(w) * h * 4);
            return;
        case 5:  // bytes are A R G B
            for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
                dst[i * 4 + 0] = src[i * 4 + 1];
                dst[i * 4 + 1] = src[i * 4 + 2];
                dst[i * 4 + 2] = src[i * 4 + 3];
                dst[i * 4 + 3] = src[i * 4 + 0];
            }
            return;
        default:
            break;
    }
    const bool dxt5 = (format == 12);
    const size_t blockBytes = dxt5 ? 16 : 8;
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* block = src + (static_cast<size_t>(by) * bw + bx) * blockBytes;
            Rgba colors[16];
            uint8_t alpha[16];
            if (dxt5) {
                DecodeBc3Alpha(block, alpha);
                DecodeBc1Colors(block + 8, true, colors);
            } else {
                DecodeBc1Colors(block, false, colors);
                for (int i = 0; i < 16; ++i) alpha[i] = colors[i].a;
            }
            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                if (y >= h) break;
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    if (x >= w) break;
                    const Rgba& c = colors[py * 4 + px];
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4;
                    d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = alpha[py * 4 + px];
                }
            }
        }
    }
}

// The Assets folder (or its Unity project parent's) above a path; empty when there is none.
stdfs::path FindAssetsRoot(const stdfs::path& file) {
    stdfs::path dir = file.parent_path();
    while (!dir.empty()) {
        if (LowerAscii(dir.filename().string()) == "assets") return dir;
        stdfs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return stdfs::path();
}

} // namespace

bool LoadTexture2DArray(const std::string& path, TextureArrayData& out, std::string& error) {
    out = TextureArrayData();
    error.clear();
    std::string text;
    if (!ReadWhole(path, text)) {
        error = "could not read " + path;
        return false;
    }

    Lines lines(text.data(), text.data() + text.size());
    Line l;
    bool header = false;
    bool body = false;
    int width = 0, height = 0, depth = 0, format = 0, mipCount = 1, colorSpace = 0;
    long long imageDataSize = 0;
    long long streamOffset = 0, streamSize = 0;
    std::string streamPath;
    std::vector<uint8_t> data;
    bool inStream = false;
    std::string key;
    const char* vb = nullptr;
    const char* ve = nullptr;

    while (lines.next(l)) {
        if (StartsWith(l, "--- ")) {
            if (header) break;
            if (!StartsWith(l, "--- !u!187 ")) {
                error = "not a Unity Texture2DArray asset (class 187)";
                return false;
            }
            header = true;
            continue;
        }
        if (!header || l.empty()) continue;
        if (!body) {
            if (l.indent == 0 && StartsWith(l, "Texture2DArray:")) body = true;
            continue;
        }
        if (l.indent == 2 && SplitKeyValue(l, key, vb, ve)) {
            inStream = (key == "m_StreamData");
            if (key == "m_Width") width = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_Height") height = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_Depth") depth = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_Format" || key == "m_TextureFormat") format = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_MipCount") mipCount = static_cast<int>(ParseInt(vb, ve));
            else if (key == "m_ColorSpace") colorSpace = static_cast<int>(ParseInt(vb, ve));
            else if (key == "image data") imageDataSize = ParseInt(vb, ve);
            else if (key == "_typelessdata") {
                if (!ReadHexField(lines, vb, ve, 2, data)) {
                    error = "_typelessdata is not valid hex";
                    return false;
                }
            }
        } else if (inStream && l.indent == 4 && SplitKeyValue(l, key, vb, ve)) {
            if (key == "offset") streamOffset = ParseInt(vb, ve);
            else if (key == "size") streamSize = ParseInt(vb, ve);
            else if (key == "path") streamPath.assign(vb, ve);
        }
    }
    text.clear();
    text.shrink_to_fit();

    if (!body) {
        error = "not a Unity Texture2DArray asset (class 187)";
        return false;
    }
    if (width <= 0 || height <= 0 || depth <= 0 || width > 16384 || height > 16384 || depth > 2048) {
        error = "Texture2DArray has an invalid size " + std::to_string(width) + "x" + std::to_string(height) +
                "x" + std::to_string(depth);
        return false;
    }
    if (format != 3 && format != 4 && format != 5 && format != 10 && format != 12) {
        error = std::string("Texture2DArray format ") + std::to_string(format) + " (" + TextureFormatName(format) +
                ") is not supported; use RGB24, RGBA32, ARGB32, DXT1 or DXT5";
        return false;
    }
    if (mipCount < 1) mipCount = 1;

    // External data: image data is 0 and the bytes live in a .resS next to the asset.
    if ((data.empty() || imageDataSize == 0) && streamSize > 0 && !streamPath.empty()) {
        const stdfs::path asset(path);
        std::vector<stdfs::path> candidates;
        candidates.push_back(asset.parent_path() / stdfs::path(streamPath).filename());
        candidates.push_back(stdfs::path(streamPath));
        const stdfs::path assetsRoot = FindAssetsRoot(asset);
        if (!assetsRoot.empty()) candidates.push_back(assetsRoot.parent_path() / streamPath);
        for (const stdfs::path& cand : candidates) {
            std::ifstream f(cand, std::ios::binary);
            if (!f.is_open()) continue;
            f.seekg(static_cast<std::streamoff>(streamOffset), std::ios::beg);
            data.resize(static_cast<size_t>(streamSize));
            f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(streamSize));
            if (f.gcount() == static_cast<std::streamsize>(streamSize)) break;
            data.clear();
        }
        if (data.empty()) {
            error = "Texture2DArray image data is stored in " + streamPath + ", which could not be read";
            return false;
        }
    }
    if (data.empty()) {
        error = "Texture2DArray has no image data";
        return false;
    }

    // One slice holds its full mip chain; every mip, however small, takes at least one block.
    size_t sliceStride = 0;
    for (int mip = 0; mip < mipCount; ++mip) {
        const int mw = std::max(1, width >> mip);
        const int mh = std::max(1, height >> mip);
        sliceStride += FormatBytesForMip(format, mw, mh);
    }
    const size_t needed = sliceStride * static_cast<size_t>(depth);
    if (data.size() < needed) {
        error = "Texture2DArray data is " + std::to_string(data.size()) + " bytes but " +
                std::to_string(depth) + " slices of " + std::to_string(mipCount) + " mips need " + std::to_string(needed);
        return false;
    }

    const size_t outSlice = static_cast<size_t>(width) * height * 4;
    if (outSlice * static_cast<size_t>(depth) > (512ull << 20)) {
        error = "Texture2DArray is too large to import";
        return false;
    }
    out.width = width;
    out.height = height;
    out.depth = depth;
    out.srgb = (colorSpace == 1);
    out.rgba.resize(outSlice * static_cast<size_t>(depth));
    for (int s = 0; s < depth; ++s) {
        DecodeSliceMip0(format, data.data() + sliceStride * static_cast<size_t>(s), width, height,
                        out.rgba.data() + outSlice * static_cast<size_t>(s));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Prefab / material resolution
// ---------------------------------------------------------------------------------------------

namespace {

// guid -> file (absolute, without ".meta"), and mesh guid -> prefabs that render it. Built once
// per Unity Assets root and kept for the session so importing a dozen LODs walks the project once.
struct ProjectIndex {
    stdfs::path root;
    std::unordered_map<std::string, std::string> guidToPath;
    std::unordered_map<std::string, std::vector<std::string>> meshToPrefabs;
    std::chrono::steady_clock::time_point builtAt;
    bool truncated = false;
    size_t fileCount = 0;
};

constexpr size_t kMaxIndexedEntries = 400000;
constexpr size_t kMaxPrefabBytes = 64u << 20;

std::mutex g_indexMutex;
std::map<std::string, std::shared_ptr<ProjectIndex>> g_indexCache;

bool SkippedDirectory(const std::string& name) {
    const std::string n = LowerAscii(name);
    return n == "library" || n == "temp" || n == ".git" || n == ".svn" || n == "obj" || n == "logs" ||
           n == "node_modules" || n == "usersettings";
}

std::string GuidFromMeta(const stdfs::path& metaPath) {
    std::string head;
    if (!ReadLoose(metaPath, head, 512)) return std::string();
    static const char kTag[] = "guid: ";
    const size_t at = head.find(kTag);
    if (at == std::string::npos || at + sizeof(kTag) - 1 + 32 > head.size()) return std::string();
    return head.substr(at + sizeof(kTag) - 1, 32);
}

std::shared_ptr<ProjectIndex> BuildIndex(const stdfs::path& root) {
    auto index = std::make_shared<ProjectIndex>();
    index->root = root;
    std::error_code ec;
    stdfs::recursive_directory_iterator it(root, stdfs::directory_options::skip_permission_denied, ec);
    const stdfs::recursive_directory_iterator end;
    std::vector<std::string> prefabs;
    size_t seen = 0;
    while (!ec && it != end) {
        const stdfs::directory_entry& entry = *it;
        std::error_code entryEc;
        if (++seen > kMaxIndexedEntries) {
            index->truncated = true;
            break;
        }
        if (entry.is_directory(entryEc)) {
            if (SkippedDirectory(entry.path().filename().string())) it.disable_recursion_pending();
        } else {
            const std::string ext = LowerExtension(entry.path().string());
            if (ext == ".meta") {
                const std::string guid = GuidFromMeta(entry.path());
                if (!guid.empty()) {
                    stdfs::path target = entry.path();
                    target.replace_extension();  // drop ".meta"
                    index->guidToPath[guid] = target.string();
                }
            } else if (ext == ".prefab") {
                prefabs.push_back(entry.path().string());
            }
            ++index->fileCount;
        }
        it.increment(ec);
    }

    // One pass over the prefabs: which of them render which Unity .asset mesh.
    static const char kMeshTag[] = "m_Mesh: {fileID: 4300000, guid: ";
    const size_t tagLen = sizeof(kMeshTag) - 1;
    std::sort(prefabs.begin(), prefabs.end());
    std::string content;
    for (const std::string& prefab : prefabs) {
        std::error_code sizeEc;
        const auto size = stdfs::file_size(prefab, sizeEc);
        if (sizeEc || size == 0 || size > kMaxPrefabBytes) continue;
        if (!ReadLoose(prefab, content)) continue;
        size_t at = 0;
        std::unordered_set<std::string> seenInFile;
        while ((at = content.find(kMeshTag, at)) != std::string::npos) {
            at += tagLen;
            if (at + 32 > content.size()) break;
            const std::string guid = content.substr(at, 32);
            if (seenInFile.insert(guid).second) index->meshToPrefabs[guid].push_back(prefab);
        }
    }
    index->builtAt = std::chrono::steady_clock::now();
    return index;
}

std::shared_ptr<ProjectIndex> GetIndex(const stdfs::path& root, bool forceRebuild) {
    const std::string key = root.string();
    {
        std::lock_guard<std::mutex> lock(g_indexMutex);
        auto found = g_indexCache.find(key);
        if (found != g_indexCache.end() && !forceRebuild) return found->second;
    }
    std::shared_ptr<ProjectIndex> built = BuildIndex(root);
    std::lock_guard<std::mutex> lock(g_indexMutex);
    g_indexCache[key] = built;
    return built;
}

// ---- Unity material ---------------------------------------------------------------------

struct UnityTexEnv {
    std::string guid;
    glm::vec2 scale = glm::vec2(1.0f);
    glm::vec2 offset = glm::vec2(0.0f);
};

struct UnityMaterial {
    std::string name;
    std::string shaderGuid;
    std::set<std::string> keywords;
    std::unordered_map<std::string, UnityTexEnv> textures;
    std::unordered_map<std::string, double> floats;
    std::unordered_map<std::string, glm::vec4> colors;

    bool hasFloat(const char* n) const { return floats.find(n) != floats.end(); }
    double getFloat(const char* n, double def) const {
        auto it = floats.find(n);
        return it == floats.end() ? def : it->second;
    }
    bool hasColor(const char* n) const { return colors.find(n) != colors.end(); }
    glm::vec4 getColor(const char* n, const glm::vec4& def) const {
        auto it = colors.find(n);
        return it == colors.end() ? def : it->second;
    }
    // A keyword toggle is stored as a keyword and, for shader graphs, a 0/1 float as well.
    bool toggle(const char* n) const {
        if (keywords.count(n)) return true;
        auto it = floats.find(n);
        return it != floats.end() && it->second > 0.5;
    }
    const UnityTexEnv* tex(const char* n) const {
        auto it = textures.find(n);
        return (it == textures.end() || it->second.guid.empty()) ? nullptr : &it->second;
    }
};

void AddKeywords(UnityMaterial& mat, const char* b, const char* e) {
    while (b < e) {
        while (b < e && (*b == ' ' || *b == '\t')) ++b;
        const char* s = b;
        while (b < e && *b != ' ' && *b != '\t') ++b;
        if (b > s) mat.keywords.insert(std::string(s, b));
    }
}

bool ParseUnityMaterial(const std::string& text, UnityMaterial& mat) {
    Lines lines(text.data(), text.data() + text.size());
    Line l;
    bool inMaterial = false;
    std::string section;      // m_TexEnvs / m_Floats / m_Colors / other
    std::string texName;
    bool keywordRun = false;  // inside a wrapped m_ShaderKeywords scalar
    bool validKeywordList = false;
    std::string key;
    const char* vb = nullptr;
    const char* ve = nullptr;
    while (lines.next(l)) {
        if (StartsWith(l, "--- ")) {
            if (inMaterial) break;
            if (!StartsWith(l, "--- !u!21 ")) return false;
            inMaterial = true;
            continue;
        }
        if (!inMaterial || l.empty()) continue;

        if (keywordRun) {
            if (l.indent > 2 && !StartsWith(l, "- ") && l.indent < 4 + 100) {
                // Wrapped continuation of the keyword scalar (more indented than its key).
                AddKeywords(mat, l.b, l.e);
                continue;
            }
            keywordRun = false;
        }
        if (validKeywordList) {
            if (StartsWith(l, "- ")) {
                AddKeywords(mat, l.b + 2, l.e);
                continue;
            }
            validKeywordList = false;
        }

        if (l.indent == 2 && SplitKeyValue(l, key, vb, ve)) {
            section.clear();
            if (key == "m_Name") mat.name.assign(vb, ve);
            else if (key == "m_Shader") mat.shaderGuid = GuidIn(vb, ve);
            else if (key == "m_ShaderKeywords") { AddKeywords(mat, vb, ve); keywordRun = true; }
            else if (key == "m_ValidKeywords" && vb == ve) validKeywordList = true;
            continue;
        }
        // Unity writes the property lists as "- name:" at the same indent as their key.
        if (l.indent <= 4 && SplitKeyValue(l, key, vb, ve) && !StartsWith(l, "- ")) {
            if (key == "m_TexEnvs" || key == "m_Floats" || key == "m_Colors" || key == "m_Ints") section = key;
            else if (l.indent == 4) section = (key == "serializedVersion") ? section : std::string();
            continue;
        }
        if (section == "m_TexEnvs") {
            if (StartsWith(l, "- ") && l.e > l.b && l.e[-1] == ':') {
                texName.assign(l.b + 2, l.e - 1);
                mat.textures[texName];
            } else if (!texName.empty() && SplitKeyValue(l, key, vb, ve)) {
                UnityTexEnv& te = mat.textures[texName];
                if (key == "m_Texture") te.guid = GuidIn(vb, ve);
                else if (key == "m_Scale" || key == "m_Offset") {
                    double x = 0, y = 0;
                    NumberAfter(vb, ve, "x:", x);
                    NumberAfter(vb, ve, "y:", y);
                    if (key == "m_Scale") te.scale = glm::vec2(static_cast<float>(x), static_cast<float>(y));
                    else te.offset = glm::vec2(static_cast<float>(x), static_cast<float>(y));
                }
            }
        } else if (section == "m_Floats") {
            if (StartsWith(l, "- ")) {
                Line item = l;
                item.b += 2;
                if (SplitKeyValue(item, key, vb, ve)) mat.floats[key] = ParseDouble(vb, ve);
            }
        } else if (section == "m_Colors") {
            if (StartsWith(l, "- ")) {
                Line item = l;
                item.b += 2;
                if (SplitKeyValue(item, key, vb, ve)) {
                    double r = 0, g = 0, b = 0, a = 0;
                    NumberAfter(vb, ve, "r:", r);
                    NumberAfter(vb, ve, "g:", g);
                    NumberAfter(vb, ve, "b:", b);
                    NumberAfter(vb, ve, "a:", a);
                    mat.colors[key] = glm::vec4(static_cast<float>(r), static_cast<float>(g),
                                                static_cast<float>(b), static_cast<float>(a));
                }
            }
        }
    }
    return inMaterial;
}

// ---- Prefab -----------------------------------------------------------------------------

struct PrefabHit {
    bool found = false;
    std::vector<std::string> materialGuids;  // one per submesh; empty string = none
};

PrefabHit FindMaterialsInPrefab(const std::string& prefabText, const std::string& meshGuid) {
    struct Doc {
        int classId = 0;
        long long gameObject = 0;
        std::string meshGuid;
        long long meshFileId = 0;
        std::vector<std::string> materials;
        bool stripped = false;
    };
    std::vector<Doc> docs;
    Lines lines(prefabText.data(), prefabText.data() + prefabText.size());
    Line l;
    bool inMaterials = false;
    std::string key;
    const char* vb = nullptr;
    const char* ve = nullptr;
    while (lines.next(l)) {
        if (l.indent == 0 && StartsWith(l, "--- !u!")) {
            Doc d;
            const char* c = l.b + 7;
            const char* ce = c;
            while (ce < l.e && std::isdigit(static_cast<unsigned char>(*ce))) ++ce;
            d.classId = static_cast<int>(ParseInt(c, ce));
            d.stripped = std::string(l.b, l.e).find("stripped") != std::string::npos;
            docs.push_back(d);
            inMaterials = false;
            continue;
        }
        if (docs.empty()) continue;
        Doc& d = docs.back();
        if (d.classId != 33 && d.classId != 23 && d.classId != 137) continue;
        if (l.indent == 2 && !StartsWith(l, "- ") && SplitKeyValue(l, key, vb, ve)) {
            inMaterials = false;
            if (key == "m_GameObject") d.gameObject = FileIdIn(vb, ve);
            else if (key == "m_Mesh") { d.meshGuid = GuidIn(vb, ve); d.meshFileId = FileIdIn(vb, ve); }
            else if (key == "m_Materials") inMaterials = true;
        } else if (inMaterials && StartsWith(l, "- ")) {
            d.materials.push_back(GuidIn(l.b, l.e));
        }
    }

    PrefabHit hit;
    for (const Doc& f : docs) {
        if (f.classId != 33 || f.stripped || f.meshGuid != meshGuid || f.meshFileId != 4300000) continue;
        for (const Doc& r : docs) {
            if ((r.classId == 23 || r.classId == 137) && r.gameObject == f.gameObject && r.gameObject != 0) {
                hit.found = true;
                hit.materialGuids = r.materials;
                return hit;
            }
        }
    }
    return hit;
}

// ---- Conversion -------------------------------------------------------------------------

std::string ToStoredTexturePath(const std::string& absolutePath) {
    std::error_code ec;
    stdfs::path p = stdfs::weakly_canonical(stdfs::path(absolutePath), ec);
    if (ec || p.empty()) p = stdfs::path(absolutePath);
    return Modularity::Platform::ToPortableAssetPath(p.string());
}

// Resolves a texture slot to a stored path. Empty when the slot is unset or the file is missing.
std::string ResolveTexture(const ProjectIndex& index, const UnityMaterial& mat, const char* slot,
                           const UnityTexEnv** envOut = nullptr) {
    const UnityTexEnv* env = mat.tex(slot);
    if (!env) return std::string();
    auto found = index.guidToPath.find(env->guid);
    if (found == index.guidToPath.end()) return std::string();
    std::error_code ec;
    if (!stdfs::is_regular_file(found->second, ec)) return std::string();
    if (envOut) *envOut = env;
    return ToStoredTexturePath(found->second);
}

std::string ResolveFirstTexture(const ProjectIndex& index, const UnityMaterial& mat,
                                std::initializer_list<const char*> slots,
                                const UnityTexEnv** envOut = nullptr) {
    for (const char* slot : slots) {
        std::string p = ResolveTexture(index, mat, slot, envOut);
        if (!p.empty()) return p;
    }
    return std::string();
}

ImportedMaterial ConvertMaterial(const ProjectIndex& index, const UnityMaterial& mat, std::string& log) {
    ImportedMaterial result;
    result.valid = true;
    result.name = mat.name;

    MaterialProperties& props = result.props;
    {
        // Best effort: a Standard-shaded material with whatever maps the shader names.
        const UnityTexEnv* albedoEnv = nullptr;
        result.albedoPath = ResolveFirstTexture(index, mat, {"_MainTex", "_BaseMap", "_BaseColorMap"}, &albedoEnv);
        result.normalPath = ResolveFirstTexture(index, mat, {"_BumpMap", "_NormalMap"});
        glm::vec4 tint = mat.hasColor("_Color") ? mat.getColor("_Color", glm::vec4(1.0f))
                                                : mat.getColor("_BaseColor", glm::vec4(1.0f));
        props.color = glm::vec3(tint.r, tint.g, tint.b);
        if (!result.albedoPath.empty()) props.textureMix = 1.0f;
        if (albedoEnv) {
            props.uvTiling = albedoEnv->scale;
            props.uvOffset = albedoEnv->offset;
        }
        if (!result.normalPath.empty() && mat.hasFloat("_BumpScale")) {
            props.normalMapIntensity = static_cast<float>(mat.getFloat("_BumpScale", 1.0));
        }
        const bool clip = mat.keywords.count("_ALPHATEST_ON") > 0 || mat.getFloat("_AlphaClip", 0.0) > 0.5 ||
                          (mat.hasFloat("_Mode") && static_cast<int>(mat.getFloat("_Mode", 0.0) + 0.5) == 1);
        if (clip) props.alphaCutoff = static_cast<float>(mat.getFloat("_Cutoff", 0.5));
        log += "  " + mat.name + ": Standard\n";
        return result;
    }


}

} // namespace

bool ResolveMeshMaterials(const std::string& meshPath,
                          std::vector<ImportedMaterial>& perSubmesh,
                          std::string& log) {
    perSubmesh.clear();
    log.clear();

    const stdfs::path mesh(meshPath);
    const stdfs::path root = FindAssetsRoot(stdfs::absolute(mesh));
    if (root.empty()) {
        log = "Unity import: no Assets folder above " + mesh.filename().string() + "; its materials were not resolved.";
        return false;
    }

    std::string metaText;
    if (!ReadLoose(stdfs::path(meshPath + ".meta"), metaText, 512)) {
        log = "Unity import: " + mesh.filename().string() + ".meta is missing, so the mesh's prefab cannot be found.";
        return false;
    }
    std::string meshGuid;
    {
        static const char kTag[] = "guid: ";
        const size_t at = metaText.find(kTag);
        if (at != std::string::npos && at + sizeof(kTag) - 1 + 32 <= metaText.size()) {
            meshGuid = metaText.substr(at + sizeof(kTag) - 1, 32);
        }
    }
    if (meshGuid.empty()) {
        log = "Unity import: no guid in " + mesh.filename().string() + ".meta.";
        return false;
    }

    std::shared_ptr<ProjectIndex> index = GetIndex(root, false);
    auto prefabIt = index->meshToPrefabs.find(meshGuid);
    if (prefabIt == index->meshToPrefabs.end() || prefabIt->second.empty()) {
        // Files may have been added since the index was built; look again, but not more than
        // every few seconds.
        const auto age = std::chrono::steady_clock::now() - index->builtAt;
        if (age > std::chrono::seconds(5)) {
            index = GetIndex(root, true);
            prefabIt = index->meshToPrefabs.find(meshGuid);
        }
    }
    if (prefabIt == index->meshToPrefabs.end() || prefabIt->second.empty()) {
        log = "Unity import: no prefab under " + root.string() + " renders " + mesh.filename().string() +
              (index->truncated ? " (the project scan was truncated)." : ".");
        return false;
    }

    // Take the first prefab that yields a MeshRenderer for this mesh.
    PrefabHit hit;
    std::string usedPrefab;
    std::string prefabText;
    for (const std::string& prefab : prefabIt->second) {
        if (!ReadLoose(stdfs::path(prefab), prefabText)) continue;
        hit = FindMaterialsInPrefab(prefabText, meshGuid);
        if (hit.found) { usedPrefab = prefab; break; }
    }
    if (!hit.found) {
        log = "Unity import: found " + std::to_string(prefabIt->second.size()) + " prefab(s) for " +
              mesh.filename().string() + " but none has a MeshRenderer on the mesh's object.";
        return false;
    }

    log = "Unity import: " + mesh.filename().string() + " -> prefab " + stdfs::path(usedPrefab).filename().string() +
          ", " + std::to_string(hit.materialGuids.size()) + " material slot(s)\n";
    perSubmesh.assign(hit.materialGuids.size(), ImportedMaterial());
    bool any = false;
    for (size_t i = 0; i < hit.materialGuids.size(); ++i) {
        const std::string& guid = hit.materialGuids[i];
        if (guid.empty()) continue;
        auto matPath = index->guidToPath.find(guid);
        if (matPath == index->guidToPath.end()) {
            log += "  slot " + std::to_string(i) + ": material guid " + guid + " has no .meta in the project\n";
            continue;
        }
        MaterialFileData sidecar;
        if (readMaterialFile(matPath->second + ".modularity.mat", sidecar)) {
            auto& imported=perSubmesh[i]; imported.valid=true;
            imported.name=stdfs::path(matPath->second).stem().string();
            imported.props=sidecar.props; imported.albedoPath=sidecar.albedo;
            imported.normalPath=sidecar.normal; imported.occlusionPath=sidecar.occlusion;
            any=true; continue;
        }
        std::string matText;
        UnityMaterial um;
        if (!ReadLoose(stdfs::path(matPath->second), matText) || !ParseUnityMaterial(matText, um)) {
            log += "  slot " + std::to_string(i) + ": " + stdfs::path(matPath->second).filename().string() +
                   " is not a readable Unity material\n";
            continue;
        }
        if (um.name.empty()) um.name = stdfs::path(matPath->second).stem().string();
        perSubmesh[i] = ConvertMaterial(*index, um, log);
        any = any || perSubmesh[i].valid;
    }
    while (!log.empty() && log.back() == '\n') log.pop_back();
    return any;
}

} // namespace UnityAssetImport
