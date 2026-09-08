#include "NetworkSerializer.h"

namespace Net {

void Writer::writeBool(bool value) { writeRaw<uint8_t>(value ? 1u : 0u); }
void Writer::writeInt8(int8_t value) { writeRaw(value); }
void Writer::writeUInt8(uint8_t value) { writeRaw(value); }
void Writer::writeInt16(int16_t value) { writeRaw(value); }
void Writer::writeUInt16(uint16_t value) { writeRaw(value); }
void Writer::writeInt32(int32_t value) { writeRaw(value); }
void Writer::writeUInt32(uint32_t value) { writeRaw(value); }
void Writer::writeInt64(int64_t value) { writeRaw(value); }
void Writer::writeUInt64(uint64_t value) { writeRaw(value); }
void Writer::writeFloat(float value) { writeRaw(value); }
void Writer::writeDouble(double value) { writeRaw(value); }

void Writer::writeString(const std::string& value) {
    const uint32_t length = static_cast<uint32_t>(
        value.size() > kMaxSerializedElements ? kMaxSerializedElements : value.size());
    writeUInt32(length);
    if (length > 0) {
        const size_t offset = buffer.size();
        buffer.resize(offset + length);
        std::memcpy(buffer.data() + offset, value.data(), length);
    }
}

void Writer::writeVec2(const float* xy) { writeFloat(xy[0]); writeFloat(xy[1]); }
void Writer::writeVec3(const float* xyz) { writeFloat(xyz[0]); writeFloat(xyz[1]); writeFloat(xyz[2]); }
void Writer::writeVec4(const float* v) { writeFloat(v[0]); writeFloat(v[1]); writeFloat(v[2]); writeFloat(v[3]); }
void Writer::writeQuat(const float* v) { writeVec4(v); }

void Writer::writeFloatArray(const std::vector<float>& values) {
    const uint32_t count = static_cast<uint32_t>(
        values.size() > kMaxSerializedElements ? kMaxSerializedElements : values.size());
    writeUInt32(count);
    for (uint32_t i = 0; i < count; ++i) writeFloat(values[i]);
}

void Writer::writeInt32Array(const std::vector<int32_t>& values) {
    const uint32_t count = static_cast<uint32_t>(
        values.size() > kMaxSerializedElements ? kMaxSerializedElements : values.size());
    writeUInt32(count);
    for (uint32_t i = 0; i < count; ++i) writeInt32(values[i]);
}

void Writer::writeStringArray(const std::vector<std::string>& values) {
    const uint32_t count = static_cast<uint32_t>(
        values.size() > kMaxSerializedElements ? kMaxSerializedElements : values.size());
    writeUInt32(count);
    for (uint32_t i = 0; i < count; ++i) writeString(values[i]);
}

void Writer::writeBytes(const void* data, size_t size) {
    if (!data || size == 0) return;
    const size_t offset = buffer.size();
    buffer.resize(offset + size);
    std::memcpy(buffer.data() + offset, data, size);
}

void Writer::writeSpawnRequest(const SpawnRequest& request) {
    writeNetworkId(request.networkId);
    writeAssetId(request.assetId);
    writeInstanceId(request.instanceId);
    writePlayerId(request.owner);
    writeVec3(request.position);
    writeVec3(request.rotation);
    writeVec3(request.scale);
    writeNetworkId(request.parentNetworkId);
    writeBool(request.spawnDisabled);
}

bool Reader::consume(void* out, size_t size) {
    if (failure) return false;
    if (size > remaining) {
        failure = true;
        return false;
    }
    std::memcpy(out, ptr, size);
    ptr += size;
    remaining -= size;
    return true;
}

bool Reader::readBool() { return readRaw<uint8_t>() != 0; }
int8_t Reader::readInt8() { return readRaw<int8_t>(); }
uint8_t Reader::readUInt8() { return readRaw<uint8_t>(); }
int16_t Reader::readInt16() { return readRaw<int16_t>(); }
uint16_t Reader::readUInt16() { return readRaw<uint16_t>(); }
int32_t Reader::readInt32() { return readRaw<int32_t>(); }
uint32_t Reader::readUInt32() { return readRaw<uint32_t>(); }
int64_t Reader::readInt64() { return readRaw<int64_t>(); }
uint64_t Reader::readUInt64() { return readRaw<uint64_t>(); }
float Reader::readFloat() { return readRaw<float>(); }
double Reader::readDouble() { return readRaw<double>(); }

std::string Reader::readString() {
    const uint32_t length = readUInt32();
    if (failure) return {};
    if (length > kMaxSerializedElements || length > remaining) {
        failure = true;
        return {};
    }
    std::string value;
    value.resize(length);
    if (length > 0 && !consume(&value[0], length)) return {};
    return value;
}

void Reader::readVec2(float* out) { out[0] = readFloat(); out[1] = readFloat(); }
void Reader::readVec3(float* out) { out[0] = readFloat(); out[1] = readFloat(); out[2] = readFloat(); }
void Reader::readVec4(float* out) {
    out[0] = readFloat(); out[1] = readFloat(); out[2] = readFloat(); out[3] = readFloat();
}
void Reader::readQuat(float* out) { readVec4(out); }

std::vector<float> Reader::readFloatArray() {
    const uint32_t count = readUInt32();
    if (failure) return {};
    if (count > kMaxSerializedElements || static_cast<size_t>(count) * sizeof(float) > remaining) {
        failure = true;
        return {};
    }
    std::vector<float> values(count);
    for (uint32_t i = 0; i < count; ++i) values[i] = readFloat();
    return values;
}

std::vector<int32_t> Reader::readInt32Array() {
    const uint32_t count = readUInt32();
    if (failure) return {};
    if (count > kMaxSerializedElements || static_cast<size_t>(count) * sizeof(int32_t) > remaining) {
        failure = true;
        return {};
    }
    std::vector<int32_t> values(count);
    for (uint32_t i = 0; i < count; ++i) values[i] = readInt32();
    return values;
}

std::vector<std::string> Reader::readStringArray() {
    const uint32_t count = readUInt32();
    if (failure) return {};
    if (count > kMaxSerializedElements || static_cast<size_t>(count) * sizeof(uint32_t) > remaining) {
        failure = true;
        return {};
    }
    std::vector<std::string> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        values.push_back(readString());
        if (failure) return {};
    }
    return values;
}

bool Reader::readBytes(void* out, size_t size) { return consume(out, size); }

SpawnRequest Reader::readSpawnRequest() {
    SpawnRequest request;
    request.networkId = readNetworkId();
    request.assetId = readAssetId();
    request.instanceId = readInstanceId();
    request.owner = readPlayerId();
    readVec3(request.position);
    readVec3(request.rotation);
    readVec3(request.scale);
    request.parentNetworkId = readNetworkId();
    request.spawnDisabled = readBool();
    return request;
}

}  // namespace Net
