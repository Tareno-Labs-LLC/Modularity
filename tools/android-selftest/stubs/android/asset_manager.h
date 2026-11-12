#pragma once
#include <cstddef>
#include <cstdint>
struct AAssetManager {};
struct AAsset {};
using off64_t = int64_t;
enum { AASSET_MODE_UNKNOWN, AASSET_MODE_BUFFER, AASSET_MODE_STREAMING };
inline AAsset* AAssetManager_open(AAssetManager*, const char*, int) { return nullptr; }
inline void AAsset_close(AAsset*) {}
inline int AAsset_read(AAsset*, void*, size_t) { return 0; }
inline off64_t AAsset_seek64(AAsset*, off64_t, int) { return -1; }
inline off64_t AAsset_getLength64(AAsset*) { return 0; }
inline off64_t AAsset_getRemainingLength64(AAsset*) { return 0; }
