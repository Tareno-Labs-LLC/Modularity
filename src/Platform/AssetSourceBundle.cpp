#include "../../include/Platform/AssetSource.h"
#include "../RuntimeContent.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace Modularity::Platform {

namespace {

std::string NormalizeKey(const std::string& raw) {
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    return s;
}

// A window onto a slice of the mounted bundle: reads clamp to [base, base+len) so an entry can
// never over-read into the next one, and Seek is relative to the slice.
class BundleSubStream final : public AssetStream {
public:
    BundleSubStream(std::unique_ptr<AssetStream> inner, int64_t base, int64_t len)
        : mInner(std::move(inner)), mBase(base), mLen(len) {
        mInner->Seek(mBase, SEEK_SET);
    }

    size_t Read(void* dest, size_t bytes) override {
        if (mPos >= mLen) return 0;
        const size_t remaining = static_cast<size_t>(mLen - mPos);
        const size_t want = std::min(bytes, remaining);
        const size_t got = mInner->Read(dest, want);
        mPos += static_cast<int64_t>(got);
        return got;
    }
    bool Seek(int64_t offset, int whence) override {
        int64_t target = mPos;
        if (whence == SEEK_SET) target = offset;
        else if (whence == SEEK_CUR) target = mPos + offset;
        else if (whence == SEEK_END) target = mLen + offset;
        if (target < 0) target = 0;
        if (target > mLen) target = mLen;
        if (!mInner->Seek(mBase + target, SEEK_SET)) return false;
        mPos = target;
        return true;
    }
    int64_t Tell() const override { return mPos; }
    int64_t Size() const override { return mLen; }

private:
    std::unique_ptr<AssetStream> mInner;
    int64_t mBase;
    int64_t mLen;
    int64_t mPos = 0;
};

class BundleAssetSource final : public AssetSource {
public:
    BundleAssetSource(std::unique_ptr<AssetSource> fallback,
                      std::string bundlePath,
                      RuntimeBundleIndex index)
        : mFallback(std::move(fallback)), mBundlePath(std::move(bundlePath)) {
        for (auto& e : index.entries) {
            mEntries.emplace(e.path, e);
        }
    }

    bool Exists(const std::string& path) const override {
        if (mEntries.find(NormalizeKey(path)) != mEntries.end()) return true;
        return mFallback && mFallback->Exists(path);
    }

    std::vector<uint8_t> ReadAll(const std::string& path) const override {
        auto it = mEntries.find(NormalizeKey(path));
        if (it == mEntries.end()) {
            return mFallback ? mFallback->ReadAll(path) : std::vector<uint8_t>{};
        }
        auto stream = mFallback->Open(mBundlePath);
        if (!stream) {
            std::cerr << "[BundleAssetSource] Failed to open bundle for: " << path << "\n";
            return {};
        }
        std::vector<uint8_t> out(static_cast<size_t>(it->second.size));
        if (!stream->Seek(static_cast<int64_t>(it->second.offset), SEEK_SET)) return {};
        size_t total = 0;
        while (total < out.size()) {
            const size_t n = stream->Read(out.data() + total, out.size() - total);
            if (n == 0) break;
            total += n;
        }
        out.resize(total);
        return out;
    }

    std::unique_ptr<AssetStream> Open(const std::string& path) const override {
        auto it = mEntries.find(NormalizeKey(path));
        if (it == mEntries.end()) {
            return mFallback ? mFallback->Open(path) : nullptr;
        }
        auto inner = mFallback->Open(mBundlePath);
        if (!inner) return nullptr;
        return std::make_unique<BundleSubStream>(std::move(inner),
                                                 static_cast<int64_t>(it->second.offset),
                                                 static_cast<int64_t>(it->second.size));
    }

private:
    std::unique_ptr<AssetSource> mFallback;
    std::string mBundlePath;
    std::unordered_map<std::string, RuntimeBundleIndexEntry> mEntries;
};

} // namespace

std::unique_ptr<AssetSource> MakeBundleAssetSource(std::unique_ptr<AssetSource> fallback,
                                                   const std::string& bundlePath,
                                                   bool verifyIntegrity,
                                                   std::string& error) {
    if (!fallback) {
        error = "Bundle mount needs a fallback asset source.";
        return nullptr;
    }
    RuntimeBundleIndex index;
    if (!ReadRuntimeBundleIndex(*fallback, bundlePath, index, error)) {
        return nullptr;
    }
    if (verifyIntegrity && !VerifyRuntimeBundleIntegrity(*fallback, bundlePath, index, error)) {
        return nullptr;
    }
    return std::make_unique<BundleAssetSource>(std::move(fallback), bundlePath, std::move(index));
}

} // namespace Modularity::Platform
