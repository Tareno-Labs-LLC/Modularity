#pragma once

#include "Common.h"
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>

// Git version control for the open project (editor only).
//
// Everything goes through the git executable on PATH rather than a linked
// library. Sign-in then uses whatever credential helper the user already has
// (Git Credential Manager ships with Git for Windows and opens the browser
// login), git-lfs works when it is installed, and the repository stays an
// ordinary one the user can also drive from a terminal.
//
// Git never runs on the UI thread. Operations queue up and run one at a time on
// a worker; the UI draws the last published VcSnapshot and learns how each
// operation went through takeResult().

enum class VcChangeKind {
    Modified,
    Added,
    Deleted,
    Renamed,
    Untracked,
    Conflicted,
    TypeChanged
};

struct VcChange {
    std::string path;      // repository-relative, '/' separators, UTF-8
    std::string oldPath;   // renames only
    VcChangeKind kind = VcChangeKind::Modified;
    char indexCode = ' ';      // git status X/Y: staged and unstaged state
    char worktreeCode = ' ';
    // Over the host's per-file limit: checked in as parts under .modusplit/ and
    // rebuilt from them on other computers. `path` is still the real file.
    bool split = false;
    std::time_t modified = 0;  // 0 when the file is gone from disk
    std::uintmax_t size = 0;
};

struct VcCommit {
    std::string hash;
    std::string shortHash;
    std::string author;
    std::string email;
    std::time_t time = 0;
    std::string subject;
    bool local = false;  // not on any remote yet
};

struct VcBranch {
    std::string name;      // "main", or "origin/main" for remote branches
    std::string upstream;
    std::string track;     // "[ahead 1, behind 2]", "[gone]", or empty
    std::string subject;
    std::time_t time = 0;
    bool remote = false;
    bool current = false;
};

struct VcSnapshot {
    bool probed = false;        // git availability has been checked
    bool gitAvailable = false;
    std::string gitVersion;
    bool lfsAvailable = false;

    bool checked = false;       // a status refresh has completed for this project
    bool isRepo = false;
    fs::path repoRoot;
    std::string projectPrefix;  // project folder relative to repoRoot, '/'-terminated; empty when they match
    std::string headHash;
    std::string branch;
    bool detached = false;
    bool unborn = false;        // repository has no commits yet
    std::string upstream;
    int ahead = 0;
    int behind = 0;
    std::string remoteName;
    std::string remoteUrl;
    std::string userName;
    std::string userEmail;
    bool mergeInProgress = false;
    bool lfsTracked = false;    // .gitattributes routes files through LFS
    std::uintmax_t splitLimit = 0;     // files larger than this are split; 0 = never
    std::uintmax_t splitPartSize = 0;
    int splitJoinPending = 0;          // split files whose parts are newer than the file on disk
    std::time_t lastFetch = 0;

    std::vector<VcChange> changes;
    std::vector<VcCommit> history;
    std::vector<VcCommit> incoming;
    std::vector<VcChange> incomingFiles;
    std::vector<VcBranch> branches;
};

enum class VcJobKind {
    Probe,
    Refresh,
    Fetch,
    Init,
    SetRemote,
    SetIdentity,
    CheckIn,
    Push,
    Pull,
    Merge,
    AbortMerge,
    Discard,
    Resolve,
    CreateBranch,
    SwitchBranch,
    DeleteBranch,
    Ignore,
    CommitFiles,
    Clone,
    DisableLfs,
    JoinSplit
};

struct VcInitOptions {
    bool writeGitignore = true;
    bool useLfs = true;
    std::string remoteUrl;
};

struct VcResult {
    VcJobKind kind = VcJobKind::Refresh;
    bool ok = false;
    bool silent = false;       // background work; only worth showing when it failed
    std::string message;       // one line for the console / toast
    std::string detail;        // git's own output, for the console on failure
    // Files the operation rewrote on disk (pull, branch switch, undo), relative to repoRoot.
    std::vector<std::string> changedPaths;
    fs::path repoRoot;
    fs::path clonedProjectFile;  // Clone: the project.modu found in the new checkout
};

// What a worker hands back to the UI thread.
struct VcJobOutput {
    VcResult result;
    bool cancelled = false;
    bool hasSnapshot = false;
    VcSnapshot snapshot;
    std::string commitHash;               // CommitFiles
    std::vector<VcChange> commitFiles;
};

struct VcShared;  // progress text + cancellation, shared with the running worker

class VersionControl {
public:
    VersionControl();
    ~VersionControl();
    VersionControl(const VersionControl&) = delete;
    VersionControl& operator=(const VersionControl&) = delete;

    // Once per frame. Follows the open project, starts queued work, publishes
    // finished work and schedules background refreshes. projectRoot may be empty.
    void update(const fs::path& projectRoot, double now, bool panelVisible);

    const VcSnapshot& snapshot() const { return snap; }
    const fs::path& currentProjectRoot() const { return projectRoot; }
    bool isBusy() const;           // a user-started operation is queued or running
    const char* busyLabel() const; // what is running now, e.g. "Uploading"; empty when idle
    static const char* JobLabel(VcJobKind kind);
    std::string progressLine() const;
    double runningSeconds() const; // how long the current operation has been going; 0 when idle
    void cancel();                 // stops the running git process tree
    bool takeResult(VcResult& out);
    // Why the last background refresh or fetch failed; empty once one succeeds.
    const std::string& lastBackgroundError() const { return backgroundError; }

    void probe();
    void refresh(bool full = true);
    void fetch(bool silent = false);
    void init(const VcInitOptions& options);
    void setRemote(const std::string& url);
    // global writes ~/.gitconfig so every project picks it up; otherwise this repository only.
    void setIdentity(const std::string& name, const std::string& email, bool global);
    // Commits exactly `paths` (repo-relative); pushes afterwards when asked and a remote exists.
    void checkIn(const std::vector<std::string>& paths, const std::string& message, bool push);
    void push();
    void pull();
    void merge();
    void abortMerge();
    void discard(const std::vector<std::string>& paths);
    void resolve(const std::vector<std::string>& paths, bool keepMine);
    void createBranch(const std::string& name, bool switchTo);
    void switchBranch(const std::string& name, bool remote);
    void deleteBranch(const std::string& name);
    void ignore(const std::string& pattern);
    void clone(const std::string& url, const fs::path& destination);
    // Converts LFS-stored files to ordinary ones: rewrites check-ins that are not
    // uploaded yet and removes the LFS rules. Refuses while tracked files have edits.
    void disableLfs();
    // Rebuilds split files whose parts arrived without the editor doing the download.
    void joinSplitFiles();

    void loadCommitFiles(const std::string& hash);
    // Null until loadCommitFiles has finished for this hash.
    const std::vector<VcChange>* commitFiles(const std::string& hash) const;

private:
    using JobFn = std::function<void(const fs::path& projectRoot, const VcSnapshot& base,
                                     VcShared& shared, VcJobOutput& out)>;
    struct Job {
        VcJobKind kind = VcJobKind::Refresh;
        bool user = true;
        JobFn fn;
    };

    void enqueue(VcJobKind kind, bool user, JobFn fn);
    void enqueueRefresh(bool full);
    void startNext();
    void finishRunning();
    bool hasQueued(VcJobKind kind) const;

    fs::path projectRoot;
    VcSnapshot snap;
    std::deque<Job> queue;
    std::future<VcJobOutput> running;
    bool runningActive = false;
    Job runningJob;
    unsigned runningGeneration = 0;
    unsigned generation = 0;
    std::shared_ptr<VcShared> shared;
    std::deque<VcResult> results;
    std::string backgroundError;
    std::string autoJoinHead;  // HEAD at which split files were last rebuilt in the background
    std::map<std::string, std::vector<VcChange>> commitFileCache;
    double lastStatusRefresh = -1.0;
    double lastAutoFetch = -1.0;
};
