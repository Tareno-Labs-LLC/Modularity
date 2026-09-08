#ifdef __ANDROID__

#include "../../include/Platform/AssetSource.h"

#include <android/asset_manager.h>
#include <android/log.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#define ASSET_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "Modularity.AssetSource", __VA_ARGS__)
#define ASSET_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Modularity.AssetSource", __VA_ARGS__)

namespace Modularity::Platform {

namespace {

std::string NormalizeAssetPath(const std::string& path) {
    std::string out = path;
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

class AndroidAssetStream final : public AssetStream {
public:
    explicit AndroidAssetStream(AAsset* asset) : mAsset(asset) {}
    ~AndroidAssetStream() override {
        if (mAsset) AAsset_close(mAsset);
    }

    size_t Read(void* dest, size_t bytes) override {
        if (!mAsset) return 0;
        const int n = AAsset_read(mAsset, dest, bytes);
        return (n > 0) ? static_cast<size_t>(n) : 0;
    }
    bool Seek(int64_t offset, int whence) override {
        if (!mAsset) return false;
        const off64_t r = AAsset_seek64(mAsset, offset, whence);
        return r != static_cast<off64_t>(-1);
    }
    int64_t Tell() const override {
        if (!mAsset) return -1;
        return static_cast<int64_t>(AAsset_getLength64(mAsset)
                                    - AAsset_getRemainingLength64(mAsset));
    }
    int64_t Size() const override {
        if (!mAsset) return -1;
        return static_cast<int64_t>(AAsset_getLength64(mAsset));
    }
private:
    AAsset* mAsset = nullptr;
};

class FilesystemAssetStream final : public AssetStream {
public:
    FilesystemAssetStream(std::unique_ptr<std::ifstream> stream, int64_t size)
        : mStream(std::move(stream)), mSize(size) {}

    size_t Read(void* dest, size_t bytes) override {
        mStream->read(reinterpret_cast<char*>(dest), static_cast<std::streamsize>(bytes));
        return static_cast<size_t>(mStream->gcount());
    }
    bool Seek(int64_t offset, int whence) override {
        std::ios_base::seekdir dir = std::ios_base::beg;
        if (whence == SEEK_CUR) dir = std::ios_base::cur;
        else if (whence == SEEK_END) dir = std::ios_base::end;
        mStream->clear();
        mStream->seekg(static_cast<std::streamoff>(offset), dir);
        return mStream->good();
    }
    int64_t Tell() const override { return mStream->tellg(); }
    int64_t Size() const override { return mSize; }
private:
    std::unique_ptr<std::ifstream> mStream;
    int64_t mSize;
};

class AndroidAssetSource final : public AssetSource {
public:
    explicit AndroidAssetSource(AAssetManager* mgr) : mMgr(mgr) {}

    bool Exists(const std::string& path) const override {
        std::error_code ec;
        if (fs::exists(fs::path(path), ec) && !ec) return true;
        if (!mMgr) return false;
        const std::string key = NormalizeAssetPath(path);
        AAsset* a = AAssetManager_open(mMgr, key.c_str(), AASSET_MODE_UNKNOWN);
        if (!a) return false;
        AAsset_close(a);
        return true;
    }

    std::vector<uint8_t> ReadAll(const std::string& path) const override {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f) {
            const std::streamsize n = f.tellg();
            if (n > 0) {
                f.seekg(0, std::ios::beg);
                std::vector<uint8_t> buf(static_cast<size_t>(n));
                if (f.read(reinterpret_cast<char*>(buf.data()), n)) {
                    return buf;
                }
            }
        }
        if (!mMgr) {
            ASSET_LOGW("ReadAll miss (no AAssetManager): %s", path.c_str());
            return {};
        }
        const std::string key = NormalizeAssetPath(path);
        AAsset* a = AAssetManager_open(mMgr, key.c_str(), AASSET_MODE_BUFFER);
        if (!a) {
            ASSET_LOGW("ReadAll miss: %s", key.c_str());
            return {};
        }
        const off64_t size = AAsset_getLength64(a);
        std::vector<uint8_t> buf(static_cast<size_t>(size));
        const int n = AAsset_read(a, buf.data(), static_cast<size_t>(size));
        AAsset_close(a);
        if (n != static_cast<int>(size)) {
            ASSET_LOGE("ReadAll short read: %s (%d of %lld)",
                       key.c_str(), n, (long long)size);
            buf.clear();
        }
        return buf;
    }

    std::unique_ptr<AssetStream> Open(const std::string& path) const override {
        {
            auto f = std::make_unique<std::ifstream>(path, std::ios::binary | std::ios::ate);
            if (*f) {
                const int64_t size = f->tellg();
                f->seekg(0, std::ios::beg);
                return std::make_unique<FilesystemAssetStream>(std::move(f), size);
            }
        }
        if (!mMgr) {
            ASSET_LOGW("Open miss (no AAssetManager): %s", path.c_str());
            return nullptr;
        }
        const std::string key = NormalizeAssetPath(path);
        AAsset* a = AAssetManager_open(mMgr, key.c_str(), AASSET_MODE_STREAMING);
        if (!a) {
            ASSET_LOGW("Open miss: %s", key.c_str());
            return nullptr;
        }
        return std::make_unique<AndroidAssetStream>(a);
    }

private:
    AAssetManager* mMgr = nullptr;
};

} // namespace

std::unique_ptr<AssetSource> MakeAndroidAssetSource(AAssetManager* mgr) {
    return std::make_unique<AndroidAssetSource>(mgr);
}

}

#endif // __ANDROID__
