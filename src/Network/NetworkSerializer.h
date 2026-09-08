#pragma once

#include "NetworkTypes.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Net {

class Writer {
public:
    void writeBool(bool value);
    void writeInt8(int8_t value);
    void writeUInt8(uint8_t value);
    void writeInt16(int16_t value);
    void writeUInt16(uint16_t value);
    void writeInt32(int32_t value);
    void writeUInt32(uint32_t value);
    void writeInt64(int64_t value);
    void writeUInt64(uint64_t value);
    void writeFloat(float value);
    void writeDouble(double value);

    void writeString(const std::string& value);

    void writeVec2(const float* xy);
    void writeVec3(const float* xyz);
    void writeVec4(const float* xyzw);
    void writeQuat(const float* xyzw);

    void writeNetworkId(NetworkId id) { writeUInt32(id); }
    void writePlayerId(PlayerId id) { writeInt32(id); }
    void writeAssetId(const std::string& id) { writeString(id); }
    void writeInstanceId(const std::string& id) { writeString(id); }
    void writeSceneObjectId(int32_t id) { writeInt32(id); }

    void writeFloatArray(const std::vector<float>& values);
    void writeInt32Array(const std::vector<int32_t>& values);
    void writeStringArray(const std::vector<std::string>& values);
    void writeBytes(const void* data, size_t size);

    void writeSpawnRequest(const SpawnRequest& request);

    const std::vector<uint8_t>& data() const { return buffer; }
    std::vector<uint8_t> take() { return std::move(buffer); }
    size_t size() const { return buffer.size(); }
    void clear() { buffer.clear(); }
    void reserve(size_t bytes) { buffer.reserve(bytes); }

private:
    template <typename T>
    void writeRaw(T value) {
        static_assert(std::is_trivially_copyable<T>::value, "raw write needs a POD");
        const size_t offset = buffer.size();
        buffer.resize(offset + sizeof(T));
        std::memcpy(buffer.data() + offset, &value, sizeof(T));
    }

    std::vector<uint8_t> buffer;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : ptr(data), remaining(size) {}
    explicit Reader(const std::vector<uint8_t>& bytes)
        : ptr(bytes.data()), remaining(bytes.size()) {}

    bool readBool();
    int8_t readInt8();
    uint8_t readUInt8();
    int16_t readInt16();
    uint16_t readUInt16();
    int32_t readInt32();
    uint32_t readUInt32();
    int64_t readInt64();
    uint64_t readUInt64();
    float readFloat();
    double readDouble();

    std::string readString();

    void readVec2(float* outXy);
    void readVec3(float* outXyz);
    void readVec4(float* outXyzw);
    void readQuat(float* outXyzw);

    NetworkId readNetworkId() { return readUInt32(); }
    PlayerId readPlayerId() { return readInt32(); }
    std::string readAssetId() { return readString(); }
    std::string readInstanceId() { return readString(); }
    int32_t readSceneObjectId() { return readInt32(); }

    std::vector<float> readFloatArray();
    std::vector<int32_t> readInt32Array();
    std::vector<std::string> readStringArray();
    bool readBytes(void* out, size_t size);

    SpawnRequest readSpawnRequest();

    bool failed() const { return failure; }
    size_t bytesRemaining() const { return remaining; }

private:
    template <typename T>
    T readRaw() {
        T value{};
        if (!consume(&value, sizeof(T))) return T{};
        return value;
    }

    bool consume(void* out, size_t size);

    const uint8_t* ptr = nullptr;
    size_t remaining = 0;
    bool failure = false;
};

constexpr uint32_t kMaxSerializedElements = 1u << 20;

}  // namespace Net
