#pragma once

#include "Common.h"

#include <unordered_map>
#include <unordered_set>

struct ScriptToolchainStamp {
    int abiVersion = 0;
    unsigned long long layoutSignature = 0;
    std::string engineVersion;
    std::string apiStamp;

    bool valid() const { return abiVersion != 0; }
};

// failing them open keeps odd setups (no discoverable engine headers, say) from
bool scriptToolchainStampsCompatible(const ScriptToolchainStamp& built,
                                     const ScriptToolchainStamp& current);

ScriptToolchainStamp currentScriptToolchainStamp(const std::vector<fs::path>& includeDirs);

struct ScriptHistoryEntry {
    ScriptToolchainStamp stamp;
    fs::path binaryPath;
    long long binaryWriteTime = 0;
};

class ScriptHistory {
public:
    static std::string makeKey(const fs::path& path);
    static long long toTimeRep(const fs::file_time_type& time);
    static fs::path fileForOutDir(const fs::path& outDir);

    bool load(const fs::path& file);
    bool save(const fs::path& file, std::string& error);
    void reset();

    bool isMigrating() const { return migrating_; }
    bool isDirty() const { return dirty_; }

    const ScriptToolchainStamp& stamp() const { return stamp_; }
    void setStamp(const ScriptToolchainStamp& stamp);

    const ScriptHistoryEntry* find(const std::string& sourceKey) const;
    const std::unordered_map<std::string, ScriptHistoryEntry>& entries() const { return entries_; }

    void record(const std::string& sourceKey, const ScriptHistoryEntry& entry);
    void forget(const std::string& sourceKey);
    void clearEntries();
    void retainOnly(const std::unordered_set<std::string>& sourceKeys);

private:
    ScriptToolchainStamp stamp_;
    std::unordered_map<std::string, ScriptHistoryEntry> entries_;
    bool migrating_ = true;
    bool dirty_ = false;
};
