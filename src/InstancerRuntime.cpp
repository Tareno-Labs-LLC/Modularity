#include "InstancerRuntime.h"

#include "ThirdParty/glm/gtc/quaternion.hpp"
#include "ThirdParty/glm/gtc/matrix_transform.hpp"

#include <cmath>
#include <cstring>

namespace InstancerRuntime {

namespace {

constexpr size_t kInstanceBytes = 26;
constexpr char kBlobPrefix[] = "v1:";
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void PutFloat(uint8_t* dst, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    dst[0] = static_cast<uint8_t>(bits & 0xFFu);
    dst[1] = static_cast<uint8_t>((bits >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((bits >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((bits >> 24) & 0xFFu);
}

float GetFloat(const uint8_t* src) {
    const uint32_t bits = static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
                          (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

uint64_t Mix(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

uint64_t FloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace

std::string EncodeInstances(const std::vector<InstancerInstance>& instances) {
    std::string out(kBlobPrefix);
    out.reserve(out.size() + (instances.size() * kInstanceBytes + 2) / 3 * 4);
    uint8_t record[kInstanceBytes];
    // Stream the records through a 3-byte base64 group without building the raw buffer.
    uint8_t group[3];
    int groupFill = 0;
    const auto flushGroup = [&](int fill) {
        const uint32_t v = (static_cast<uint32_t>(group[0]) << 16) |
                           (fill > 1 ? static_cast<uint32_t>(group[1]) << 8 : 0u) |
                           (fill > 2 ? static_cast<uint32_t>(group[2]) : 0u);
        out.push_back(kBase64Alphabet[(v >> 18) & 63u]);
        out.push_back(kBase64Alphabet[(v >> 12) & 63u]);
        out.push_back(fill > 1 ? kBase64Alphabet[(v >> 6) & 63u] : '=');
        out.push_back(fill > 2 ? kBase64Alphabet[v & 63u] : '=');
    };
    for (const InstancerInstance& instance : instances) {
        PutFloat(record + 0, instance.position.x);
        PutFloat(record + 4, instance.position.y);
        PutFloat(record + 8, instance.position.z);
        PutFloat(record + 12, instance.yawDegrees);
        PutFloat(record + 16, instance.scale);
        record[20] = static_cast<uint8_t>(instance.prototype & 0xFFu);
        record[21] = static_cast<uint8_t>((instance.prototype >> 8) & 0xFFu);
        record[22] = instance.tint;
        record[23] = static_cast<uint8_t>(instance.tiltX);
        record[24] = static_cast<uint8_t>(instance.tiltZ);
        record[25] = 0;
        for (size_t i = 0; i < kInstanceBytes; ++i) {
            group[groupFill++] = record[i];
            if (groupFill == 3) {
                flushGroup(3);
                groupFill = 0;
            }
        }
    }
    if (groupFill > 0) flushGroup(groupFill);
    return out;
}

bool DecodeInstances(const std::string& text, std::vector<InstancerInstance>& out) {
    out.clear();
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    if (text.compare(begin, sizeof(kBlobPrefix) - 1, kBlobPrefix) != 0) {
        return text.size() == begin;   // An empty value is an empty list, anything else is foreign.
    }
    begin += sizeof(kBlobPrefix) - 1;

    std::vector<uint8_t> bytes;
    bytes.reserve((text.size() - begin) / 4 * 3);
    uint32_t accumulator = 0;
    int bits = 0;
    bool intact = true;
    for (size_t i = begin; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        const int value = Base64Value(c);
        if (value < 0) { intact = false; break; }   // keep every record decoded before the damage
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFu));
        }
    }
    if (bytes.size() % kInstanceBytes != 0) intact = false;   // a cut-off tail record is dropped
    const size_t count = bytes.size() / kInstanceBytes;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* record = bytes.data() + i * kInstanceBytes;
        InstancerInstance instance;
        instance.position = glm::vec3(GetFloat(record), GetFloat(record + 4), GetFloat(record + 8));
        instance.yawDegrees = GetFloat(record + 12);
        instance.scale = GetFloat(record + 16);
        instance.prototype = static_cast<uint16_t>(record[20] | (record[21] << 8));
        instance.tint = record[22];
        instance.tiltX = static_cast<int8_t>(record[23]);
        instance.tiltZ = static_cast<int8_t>(record[24]);
        if (!std::isfinite(instance.position.x) || !std::isfinite(instance.position.y) ||
            !std::isfinite(instance.position.z) || !std::isfinite(instance.yawDegrees) ||
            !std::isfinite(instance.scale) || instance.scale <= 0.0f) {
            intact = false;   // a damaged record would poison the bounds and matrices; skip just that one
            continue;
        }
        out.push_back(instance);
    }
    return intact;
}

void SetInstanceTilt(InstancerInstance& instance, const glm::vec3& groundNormal) {
    glm::vec3 n = groundNormal;
    const float length = glm::length(n);
    if (!(length > 1e-4f) || !std::isfinite(length)) {
        instance.tiltX = 0;
        instance.tiltZ = 0;
        return;
    }
    n /= length;
    if (n.y < 0.2f) {
        // Never lay an instance on its side; clamp to a steep but upright tilt.
        n = glm::normalize(glm::vec3(n.x, 0.2f, n.z));
    }
    instance.tiltX = static_cast<int8_t>(std::lround(glm::clamp(n.x, -1.0f, 1.0f) * 127.0f));
    instance.tiltZ = static_cast<int8_t>(std::lround(glm::clamp(n.z, -1.0f, 1.0f) * 127.0f));
}

glm::vec3 InstanceUp(const InstancerInstance& instance) {
    if (instance.tiltX == 0 && instance.tiltZ == 0) return glm::vec3(0.0f, 1.0f, 0.0f);
    const float x = static_cast<float>(instance.tiltX) / 127.0f;
    const float z = static_cast<float>(instance.tiltZ) / 127.0f;
    const float horizontal = x * x + z * z;
    const float y = std::sqrt(std::max(1.0f - horizontal, 0.04f));
    return glm::normalize(glm::vec3(x, y, z));
}

glm::mat4 ObjectWorldMatrix(const SceneObject& object) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, object.position);
    model = glm::rotate(model, glm::radians(object.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(object.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(object.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(model, object.scale);
}

glm::mat4 InstanceLocalMatrix(const InstancerInstance& instance) {
    const float yaw = glm::radians(instance.yawDegrees);
    const float s = instance.scale;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    // Ry(yaw) * S(scale): columns are the rotated, scaled basis vectors.
    glm::vec3 bx(cy * s, 0.0f, -sy * s);
    glm::vec3 by(0.0f, s, 0.0f);
    glm::vec3 bz(sy * s, 0.0f, cy * s);
    if (instance.tiltX != 0 || instance.tiltZ != 0) {
        const glm::vec3 up = InstanceUp(instance);
        const float cosAngle = glm::clamp(up.y, -1.0f, 1.0f);
        glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
        const float axisLength = glm::length(axis);
        if (axisLength > 1e-5f) {
            const glm::quat tilt = glm::angleAxis(std::acos(cosAngle), axis / axisLength);
            bx = tilt * bx;
            by = tilt * by;
            bz = tilt * bz;
        }
    }
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(bx, 0.0f);
    m[1] = glm::vec4(by, 0.0f);
    m[2] = glm::vec4(bz, 0.0f);
    m[3] = glm::vec4(instance.position, 1.0f);
    return m;
}

InstanceCapsule BuildInstanceCapsule(const InstancerPrototype& prototype,
                                     const InstancerInstance& instance,
                                     const glm::mat4& instancerWorld) {
    InstanceCapsule capsule;
    const glm::mat4 world = instancerWorld * InstanceLocalMatrix(instance);
    const float worldScale = std::max(glm::length(glm::vec3(world[0])), 1e-4f);
    capsule.radius = std::max(prototype.colliderRadius * worldScale, 0.02f);
    capsule.height = std::max(prototype.colliderHeight * worldScale, capsule.radius * 2.0f + 0.02f);
    capsule.base = glm::vec3(world * glm::vec4(0.0f, prototype.colliderOffsetY, 0.0f, 1.0f));
    capsule.rotation = glm::quat_cast(glm::mat3(world[0] / worldScale, world[1] / worldScale,
                                                world[2] / worldScale));
    return capsule;
}

uint64_t HashPrototypes(const std::vector<InstancerPrototype>& prototypes) {
    uint64_t hash = 1469598103934665603ull;
    hash = Mix(hash, prototypes.size());
    for (const InstancerPrototype& p : prototypes) {
        hash = Mix(hash, static_cast<uint64_t>(static_cast<int64_t>(p.sourceObjectId)));
        hash = Mix(hash, static_cast<uint64_t>(static_cast<int64_t>(p.lodSourceObjectId)));
        hash = Mix(hash, FloatBits(p.lodDistance));
        hash = Mix(hash, FloatBits(p.maxDrawDistance));
        hash = Mix(hash, p.castShadows ? 3u : 2u);
        hash = Mix(hash, FloatBits(p.shadowDistance));
        hash = Mix(hash, static_cast<uint64_t>(p.colliderMode));
        hash = Mix(hash, FloatBits(p.colliderRadius));
        hash = Mix(hash, FloatBits(p.colliderHeight));
        hash = Mix(hash, FloatBits(p.colliderOffsetY));
    }
    return hash;
}

} // namespace InstancerRuntime
