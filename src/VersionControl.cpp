#include "VersionControl.h"

#if !MODULARITY_RUNTIME_ONLY

#include <atomic>
#include <cstring>
#include <set>
#include <thread>

#ifdef _WIN32
#include <objbase.h>
#include <shellapi.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

struct VcShared {
    mutable std::mutex mutex;
    std::string progress;
    std::atomic<bool> cancelled{false};
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
#ifdef _WIN32
    HANDLE job = nullptr;  // job object holding the running git process tree
#else
    pid_t group = -1;      // process group of the running git
#endif

    void setProgress(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        progress = line;
    }

    void kill() {
        std::lock_guard<std::mutex> lock(mutex);
        cancelled = true;
#ifdef _WIN32
        if (job) TerminateJobObject(job, 1);
#else
        if (group > 0) ::kill(-group, SIGTERM);
#endif
    }
};

namespace {

constexpr std::uintmax_t kHardFileLimit = 100ull * 1024ull * 1024ull;  // GitHub rejects anything larger

#pragma region Paths and text

#ifdef _WIN32
std::wstring Widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], len);
    return wide;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &narrow[0], len, nullptr, nullptr);
    return narrow;
}
#endif

// Git speaks UTF-8 with '/' separators; these keep that exact on every platform.
fs::path PathFromUtf8(const std::string& text) {
#ifdef _WIN32
    return fs::path(Widen(text));
#else
    return fs::path(text);
#endif
}

std::string Utf8Generic(const fs::path& path) {
#ifdef _WIN32
    return Narrow(path.generic_wstring());
#else
    return path.generic_string();
#endif
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(text);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(separator, start);
        if (end == std::string::npos) end = text.size();
        fields.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

std::time_t ToTimeT(fs::file_time_type fileTime) {
    using namespace std::chrono;
    const auto system = time_point_cast<system_clock::duration>(
        fileTime - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(system);
}

bool WriteTextFile(const fs::path& path, const std::string& text, bool append = false) {
    std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!file) return false;
    file << text;
    return static_cast<bool>(file);
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::string();
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

#pragma endregion

#pragma region Process

struct GitRun {
    bool launched = false;
    int exitCode = -1;
    std::string out;
    std::string err;

    bool ok() const { return launched && exitCode == 0; }
};

// Every call: UTF-8 paths in output instead of octal escapes, no color codes, and
// pathspecs taken literally so "Rock [1].png" is a file name rather than a glob.
const std::vector<std::string>& BaseArgs() {
    static const std::vector<std::string> args = {
        "-c", "core.quotepath=off",
        "-c", "color.ui=never",
#ifdef _WIN32
        "-c", "core.longpaths=true",
#endif
        "--literal-pathspecs",
    };
    return args;
}

// git's progress lines in the panel's words, keeping any "45% (12/30), 3 MiB | 1 MiB/s".
// git-lfs migrate prints a stage only once it has finished ("Sorting commits: ...,
// done."), so its lines are translated into the stage that is now running.
std::string FriendlyProgress(const std::string& line) {
    struct Stage {
        const char* prefix;
        const char* text;
        bool keepDetail;
    };
    static const Stage kStages[] = {
        {"Sorting commits", "Rewriting check-ins: every large file is read and stored once, which takes a few minutes for a big project", false},
        {"Rewriting commits", "Updating branches", false},
        {"Updating refs", "Refreshing project files", false},
        {"Checkout", "Cleaning up large-file storage", false},
        {"Deleting objects", "Cleaning up large-file storage", false},
        {"Uploading LFS objects", "Uploading large files", true},
        {"Downloading LFS objects", "Downloading large files", true},
        {"Filtering content", "Downloading large files", true},
        {"Enumerating objects", "Preparing the upload", false},
        {"Counting objects", "Preparing the upload", false},
        {"Delta compression", "Preparing the upload", false},
        {"Compressing objects", "Compressing", true},
        {"Writing objects", "Uploading", true},
        {"Total ", "Finishing the upload", false},
        {"remote: Resolving deltas", "Server is storing the upload", true},
        {"remote: ", "Server is preparing files", false},
        {"Receiving objects", "Downloading", true},
        {"Resolving deltas", "Unpacking", true},
        {"Updating files", "Writing files", true},
    };
    for (const Stage& stage : kStages) {
        if (!StartsWith(line, stage.prefix)) continue;
        std::string text = stage.text;
        const size_t colon = line.find(':', std::strlen(stage.prefix) - 1);
        if (stage.keepDetail && colon != std::string::npos) {
            std::string detail = Trim(line.substr(colon + 1));
            const size_t done = detail.rfind(", done.");
            if (done != std::string::npos) detail.resize(done);
            if (!detail.empty()) text += ": " + detail;
        }
        return text;
    }
    if (Contains(line, "local objects") && Contains(line, "retained")) return "Cleaning up large-file storage";
    return line;
}

// Feeds git's stderr into the progress line as it arrives. Progress updates are
// separated by '\r', finished lines by '\n'.
void TrackProgress(VcShared* shared, std::string& partial, const char* data, size_t size) {
    if (!shared) return;
    for (size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '\r' || c == '\n') {
            const std::string line = Trim(partial);
            if (!line.empty()) shared->setProgress(FriendlyProgress(line));
            partial.clear();
        } else {
            partial.push_back(c);
        }
    }
}

#ifdef _WIN32
void AppendQuotedArg(std::wstring& commandLine, const std::wstring& arg) {
    commandLine.push_back(L' ');
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        commandLine += arg;
        return;
    }
    commandLine.push_back(L'"');
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            commandLine.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            commandLine.append(backslashes * 2 + 1, L'\\');
        } else {
            commandLine.append(backslashes, L'\\');
        }
        commandLine.push_back(*it);
    }
    commandLine.push_back(L'"');
}

// The editor's environment plus the variables that keep git from waiting on a
// terminal nobody can see. interactive=false also stops Git Credential Manager
// from opening a login window, for background fetches.
std::vector<wchar_t> BuildGitEnvironment(bool interactive) {
    std::vector<std::wstring> vars;
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (LPWCH entry = block; *entry; entry += wcslen(entry) + 1) {
            if (_wcsnicmp(entry, L"GIT_TERMINAL_PROMPT=", 20) == 0 ||
                _wcsnicmp(entry, L"GIT_MERGE_AUTOEDIT=", 19) == 0 ||
                _wcsnicmp(entry, L"GCM_INTERACTIVE=", 16) == 0) {
                continue;
            }
            vars.emplace_back(entry);
        }
        FreeEnvironmentStringsW(block);
    }
    vars.emplace_back(L"GIT_TERMINAL_PROMPT=0");
    vars.emplace_back(L"GIT_MERGE_AUTOEDIT=no");
    if (!interactive) vars.emplace_back(L"GCM_INTERACTIVE=never");
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (const std::wstring& var : vars) {
        block.insert(block.end(), var.begin(), var.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

GitRun RunGit(VcShared* shared, const fs::path& cwd, const std::vector<std::string>& args,
              bool progress, bool interactive) {
    GitRun run;
    if (shared && shared->cancelled) {
        run.err = "Cancelled.";
        return run;
    }

    std::wstring commandLine = L"git";
    for (const std::string& arg : BaseArgs()) AppendQuotedArg(commandLine, Widen(arg));
    for (const std::string& arg : args) AppendQuotedArg(commandLine, Widen(arg));
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    std::vector<wchar_t> environment = BuildGitEnvironment(interactive);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        run.err = "Could not create a pipe for git.";
        return run;
    }
    if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        run.err = "Could not create a pipe for git.";
        return run;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    // stdin is NUL so nothing git runs can sit waiting for input.
    HANDLE nulInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nulInput != INVALID_HANDLE_VALUE ? nulInput : nullptr;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;

    // git.exe on PATH is usually a launcher for the real git, which in turn starts
    // helpers (remote-https, lfs, the credential manager). A job object lets
    // Cancel stop the whole tree instead of orphaning the part doing the work.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    const std::wstring workingDir = cwd.wstring();
    PROCESS_INFORMATION pi{};
    const BOOL launched = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        environment.data(), workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
    const DWORD launchError = launched ? 0 : GetLastError();
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (nulInput != INVALID_HANDLE_VALUE) CloseHandle(nulInput);

    if (!launched) {
        CloseHandle(outRead);
        CloseHandle(errRead);
        if (job) CloseHandle(job);
        run.err = (launchError == ERROR_FILE_NOT_FOUND || launchError == ERROR_PATH_NOT_FOUND)
            ? "git was not found. Install Git from https://git-scm.com and restart Modularity."
            : "Could not start git (Windows error " + std::to_string(launchError) + ").";
        return run;
    }
    run.launched = true;
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    if (shared) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->job = job;
        if (shared->cancelled && job) TerminateJobObject(job, 1);
    }
    ResumeThread(pi.hThread);

    std::thread errReader([&]() {
        char buffer[4096];
        std::string partial;
        DWORD read = 0;
        while (ReadFile(errRead, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            run.err.append(buffer, read);
            if (progress) TrackProgress(shared, partial, buffer, read);
        }
    });
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(outRead, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        run.out.append(buffer, read);
    }
    errReader.join();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    run.exitCode = static_cast<int>(exitCode);
    if (shared) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->job = nullptr;
    }
    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return run;
}
#else
void DrainFd(int fd, std::string& sink, VcShared* shared, bool progress) {
    char buffer[4096];
    std::string partial;
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            sink.append(buffer, static_cast<size_t>(n));
            if (progress) TrackProgress(shared, partial, buffer, static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

GitRun RunGit(VcShared* shared, const fs::path& cwd, const std::vector<std::string>& args,
              bool progress, bool interactive) {
    GitRun run;
    if (shared && shared->cancelled) {
        run.err = "Cancelled.";
        return run;
    }

    std::vector<std::string> argStorage = {"git"};
    argStorage.insert(argStorage.end(), BaseArgs().begin(), BaseArgs().end());
    argStorage.insert(argStorage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : argStorage) argv.push_back(&arg[0]);
    argv.push_back(nullptr);

    // Built before fork(): the child may only call async-signal-safe functions.
    std::vector<std::string> envStorage;
    for (char** entry = environ; entry && *entry; ++entry) {
        if (StartsWith(*entry, "GIT_TERMINAL_PROMPT=") || StartsWith(*entry, "GIT_MERGE_AUTOEDIT=") ||
            StartsWith(*entry, "GCM_INTERACTIVE=") || StartsWith(*entry, "SSH_ASKPASS_REQUIRE=")) {
            continue;
        }
        envStorage.emplace_back(*entry);
    }
    envStorage.emplace_back("GIT_TERMINAL_PROMPT=0");
    envStorage.emplace_back("GIT_MERGE_AUTOEDIT=no");
    if (!interactive) {
        envStorage.emplace_back("GCM_INTERACTIVE=never");
        envStorage.emplace_back("SSH_ASKPASS_REQUIRE=never");
    }
    std::vector<char*> envp;
    for (std::string& var : envStorage) envp.push_back(&var[0]);
    envp.push_back(nullptr);
    const std::string workingDir = cwd.string();

    static std::atomic<bool> sigpipeIgnored{false};
    if (!sigpipeIgnored.exchange(true)) signal(SIGPIPE, SIG_IGN);

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        if (outPipe[0] >= 0) { ::close(outPipe[0]); ::close(outPipe[1]); }
        run.err = "Could not create a pipe for git.";
        return run;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);
        run.err = "Could not start git (fork failed).";
        return run;
    }
    if (pid == 0) {
        setpgid(0, 0);
        const int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);
        if (!workingDir.empty() && chdir(workingDir.c_str()) != 0) _exit(126);
        environ = envp.data();
        execvp("git", argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    if (shared) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->group = pid;
        if (shared->cancelled) ::kill(-pid, SIGTERM);
    }

    std::thread errReader([&]() { DrainFd(errPipe[0], run.err, shared, progress); });
    DrainFd(outPipe[0], run.out, nullptr, false);
    errReader.join();
    ::close(outPipe[0]);
    ::close(errPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (shared) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->group = -1;
    }
    run.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    run.launched = run.exitCode != 127;
    if (run.exitCode == 127) {
        run.err = "git was not found. Install Git from your package manager and restart Modularity.";
    }
    return run;
}
#endif

#pragma endregion

#pragma region Friendly errors

// The line of git's output that says what went wrong, without its "fatal: " prefix.
std::string KeyLine(const GitRun& run) {
    const std::vector<std::string> lines = SplitLines(run.err.empty() ? run.out : run.err);
    for (const std::string& line : lines) {
        for (const char* prefix : {"fatal: ", "error: ", "remote: error: "}) {
            if (StartsWith(line, prefix)) return line.substr(std::strlen(prefix));
        }
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::string line = Trim(*it);
        if (!line.empty() && !StartsWith(line, "hint:")) return line;
    }
    return std::string();
}

std::string Explain(const GitRun& run, const std::string& action) {
    const std::string& e = run.err;
    if (Contains(e, "Cancelled.")) return action + " was cancelled.";
    if (!run.launched) return run.err;
    // Before the sign-in check: a quota refusal can come back as an HTTP 403 too.
    if (Contains(e, "exceeded its LFS budget") || Contains(e, "over its data quota") ||
        Contains(e, "exceeded its bandwidth") || Contains(e, "LFS budget")) {
        return action + " failed: the server refused the large (Git LFS) files because this account's LFS "
               "allowance is used up. Either use Settings > Stop Using Git LFS (files over 100 MB are then "
               "split automatically), or raise the LFS budget in the account's billing settings. Nothing is "
               "lost: your check-ins stay on this computer until an upload succeeds.";
    }
    if (Contains(e, "could not read Username") || Contains(e, "terminal prompts disabled") ||
        Contains(e, "Authentication failed") || Contains(e, "Invalid username or password") ||
        Contains(e, "Permission denied (publickey") || Contains(e, "returned error: 403") ||
        Contains(e, "Credentials are incorrect or have expired")) {
        return action + " failed: the server needs you to sign in. On Windows, Git Credential Manager "
               "(part of Git for Windows) opens a login window; elsewhere use an SSH address or a "
               "personal access token.";
    }
    if (Contains(e, "Repository not found") || Contains(e, "does not appear to be a git repository") ||
        Contains(e, "returned error: 404")) {
        return action + " failed: there is no repository at that address, or this account cannot see it.";
    }
    if (Contains(e, "Could not resolve host") || Contains(e, "Failed to connect") ||
        Contains(e, "Connection timed out") || Contains(e, "Could not connect")) {
        return action + " failed: could not reach the server. Check the address and your connection.";
    }
    if (Contains(e, "[rejected]") || Contains(e, "fetch first") || Contains(e, "non-fast-forward")) {
        return action + " failed: the server has check-ins you don't have yet. Get the incoming "
               "changes first, then upload again.";
    }
    if (Contains(e, "Not possible to fast-forward") || Contains(e, "diverging branches")) {
        return "Your check-ins and the server's have both moved on. Use Merge Changes to combine them.";
    }
    if (Contains(e, "would be overwritten by")) {
        if (Contains(e, "untracked working tree files")) {
            return action + " failed: new files in the project have the same names as incoming ones. "
                   "Check them in or move them first.";
        }
        return action + " failed: you have unsaved-to-Git changes in files it needs to update. "
               "Check them in or undo them first.";
    }
    if (Contains(e, "GH001") || Contains(e, "exceeds GitHub's file size limit") || Contains(e, "file size limit")) {
        return action + " failed: the server rejected a file over its size limit. Store large files "
               "with Git LFS.";
    }
    if (Contains(e, "Please tell me who you are") || Contains(e, "Author identity unknown")) {
        return action + " failed: set your name and email in Version Control settings first.";
    }
    if (Contains(e, "not a git repository")) {
        return "This project is not under version control yet.";
    }
    const std::string key = KeyLine(run);
    return key.empty() ? action + " failed." : action + " failed: " + key;
}

// git's output for the console. LFS repeats one error per batch of files, so
// runs of the same line collapse to one with a count.
std::string Detail(const GitRun& run) {
    std::string text = Trim(run.err);
    const std::string out = Trim(run.out);
    if (!out.empty()) text = text.empty() ? out : out + "\n" + text;

    std::string collapsed;
    std::string previous;
    int repeats = 0;
    auto flush = [&]() {
        if (previous.empty()) return;
        if (!collapsed.empty()) collapsed += "\n";
        collapsed += previous;
        if (repeats > 1) collapsed += "  (x" + std::to_string(repeats) + ")";
    };
    for (const std::string& line : SplitLines(text)) {
        if (line == previous) {
            ++repeats;
            continue;
        }
        flush();
        previous = line;
        repeats = 1;
    }
    flush();
    return collapsed;
}

#pragma endregion

#pragma region Parsing

VcChangeKind ClassifyStatus(char x, char y) {
    if (x == '?' && y == '?') return VcChangeKind::Untracked;
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) return VcChangeKind::Conflicted;
    if (x == 'R' || y == 'R') return VcChangeKind::Renamed;
    if (x == 'D' || y == 'D') return VcChangeKind::Deleted;
    if (x == 'A') return VcChangeKind::Added;
    if (x == 'T' || y == 'T') return VcChangeKind::TypeChanged;
    return VcChangeKind::Modified;
}

VcChangeKind ClassifyNameStatus(char code) {
    switch (code) {
        case 'A': return VcChangeKind::Added;
        case 'D': return VcChangeKind::Deleted;
        case 'R': return VcChangeKind::Renamed;
        case 'T': return VcChangeKind::TypeChanged;
        case 'U': return VcChangeKind::Conflicted;
        default: return VcChangeKind::Modified;
    }
}

// "## main...origin/main [ahead 1, behind 2]" and its variants.
void ParseBranchLine(std::string line, VcSnapshot& s) {
    for (const char* unborn : {"No commits yet on ", "Initial commit on "}) {
        if (StartsWith(line, unborn)) {
            s.unborn = true;
            line = line.substr(std::strlen(unborn));  // may still carry "...upstream [gone]"
            break;
        }
    }
    if (StartsWith(line, "HEAD (no branch)")) {
        s.detached = true;
        s.branch = "HEAD";
        return;
    }
    std::string track;
    const size_t bracket = line.find(" [");
    if (bracket != std::string::npos) {
        track = line.substr(bracket + 2);
        if (!track.empty() && track.back() == ']') track.pop_back();
        line.resize(bracket);
    }
    const size_t dots = line.find("...");
    if (dots != std::string::npos) {
        s.branch = line.substr(0, dots);
        s.upstream = line.substr(dots + 3);
    } else {
        s.branch = line;
    }
    if (track == "gone") {
        s.upstream.clear();
        return;
    }
    for (const std::string& part : Split(track, ',')) {
        const std::string item = Trim(part);
        if (StartsWith(item, "ahead ")) s.ahead = std::atoi(item.c_str() + 6);
        if (StartsWith(item, "behind ")) s.behind = std::atoi(item.c_str() + 7);
    }
}

void ParseStatus(const std::string& out, VcSnapshot& s) {
    s.changes.clear();
    s.branch.clear();
    s.upstream.clear();
    s.ahead = 0;
    s.behind = 0;
    s.detached = false;
    s.unborn = false;
    const std::vector<std::string> fields = Split(out, '\0');
    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string& entry = fields[i];
        if (StartsWith(entry, "## ")) {
            ParseBranchLine(entry.substr(3), s);
            continue;
        }
        if (entry.size() < 4) continue;
        const char x = entry[0];
        const char y = entry[1];
        VcChange change;
        change.path = entry.substr(3);
        change.indexCode = x;
        change.worktreeCode = y;
        if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {
            if (i + 1 < fields.size()) {
                if (x == 'R' || y == 'R') change.oldPath = fields[i + 1];
                ++i;
            }
        }
        if (x == '!') continue;
        change.kind = ClassifyStatus(x, y);
        s.changes.push_back(std::move(change));
    }
}

// `git diff --name-status -z`: "M\0path\0", "R100\0old\0new\0".
std::vector<VcChange> ParseNameStatus(const std::string& out) {
    std::vector<VcChange> changes;
    const std::vector<std::string> fields = Split(out, '\0');
    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string& code = fields[i];
        if (code.empty() || i + 1 >= fields.size()) continue;
        VcChange change;
        change.kind = ClassifyNameStatus(code[0]);
        if (code[0] == 'R' || code[0] == 'C') {
            if (i + 2 >= fields.size()) break;
            if (code[0] == 'R') change.oldPath = fields[i + 1];
            change.path = fields[i + 2];
            i += 2;
        } else {
            change.path = fields[i + 1];
            i += 1;
        }
        changes.push_back(std::move(change));
    }
    return changes;
}

const char* kLogFormat = "--format=%H%x1f%h%x1f%an%x1f%ae%x1f%ct%x1f%s%x1e";

std::vector<VcCommit> ParseLog(const std::string& out) {
    std::vector<VcCommit> commits;
    for (std::string record : Split(out, '\x1e')) {
        while (!record.empty() && (record.front() == '\n' || record.front() == '\r')) record.erase(0, 1);
        const std::vector<std::string> f = Split(record, '\x1f');
        if (f.size() < 6) continue;
        VcCommit commit;
        commit.hash = f[0];
        commit.shortHash = f[1];
        commit.author = f[2];
        commit.email = f[3];
        commit.time = static_cast<std::time_t>(std::atoll(f[4].c_str()));
        commit.subject = f[5];
        commits.push_back(std::move(commit));
    }
    return commits;
}

#pragma endregion

#pragma region Worker

const char* kGitignore =
    "# Modularity project: files the editor rebuilds, or that belong to one machine.\n"
    "# Everything else in the project is backed up.\n"
    "\n"
    "# Compiled scripts, caches and temporary files (rebuilt when the project opens)\n"
    "/Library/*\n"
    "# Installed packages stay: the project needs them to open on another device.\n"
    "!/Library/InstalledPackages/\n"
    "\n"
    "# Per-user editor state: window layout and the launcher preview image\n"
    "/ProjectUserSettings/\n"
    "\n"
    "# Build output\n"
    "/Builds/\n"
    "/Build/\n"
    "/Cache/\n"
    "/Temp/\n"
    "\n"
    "# OS and tool clutter\n"
    ".DS_Store\n"
    "Thumbs.db\n"
    "desktop.ini\n"
    ".vs/\n";

const char* kAttributes =
    "# Store every file exactly as it is on disk. Scenes, materials and settings are\n"
    "# text the editor writes itself; letting Git convert line endings would make a\n"
    "# scene saved on Windows differ from the one opened on Linux.\n"
    "* -text\n";

const char* kLfsExtensions[] = {
    // Textures
    "png", "jpg", "jpeg", "tga", "bmp", "psd", "tif", "tiff", "exr", "hdr", "dds", "ktx", "ktx2", "webp", "gif",
    // Models
    "fbx", "obj", "blend", "glb", "bin", "dae", "3ds", "usd", "usdc", "usdz", "abc",
    // Audio and video
    "wav", "mp3", "ogg", "flac", "aif", "aiff", "opus", "mp4", "mov", "webm", "avi", "mkv", "m4v",
    // Fonts, archives and binaries
    "ttf", "otf", "zip", "7z", "rar", "modupak", "dll", "so", "dylib", "exe", "lib", "a", "pdb",
};

std::string LfsAttributes() {
    std::string text =
        "\n# Large binary assets live in Git LFS so the repository stays quick to clone.\n";
    for (const char* ext : kLfsExtensions) {
        text += "*.";
        text += ext;
        text += " filter=lfs diff=lfs merge=lfs -text\n";
    }
    return text;
}

struct Worker {
    VcShared& shared;
    fs::path projectRoot;
    fs::path repoRoot;
    fs::path gitDir;
    std::string prefix;

    Worker(VcShared& sharedState, const fs::path& root) : shared(sharedState), projectRoot(root) {}

    const fs::path& cwd() const { return repoRoot.empty() ? projectRoot : repoRoot; }

    GitRun git(const std::vector<std::string>& args, bool progress = false, bool interactive = true) {
        return RunGit(&shared, cwd(), args, progress, interactive);
    }

    GitRun gitAt(const fs::path& dir, const std::vector<std::string>& args, bool progress = false) {
        return RunGit(&shared, dir, args, progress, true);
    }

    // Finds the repository that contains the project, if any.
    bool locate() {
        repoRoot.clear();
        gitDir.clear();
        prefix.clear();
        if (projectRoot.empty()) return false;
        const GitRun run = gitAt(projectRoot, {"rev-parse", "--show-toplevel", "--absolute-git-dir"});
        if (!run.ok()) return false;
        const std::vector<std::string> lines = SplitLines(run.out);
        if (lines.size() < 2) return false;
        repoRoot = PathFromUtf8(lines[0]);
        gitDir = PathFromUtf8(lines[1]);
        std::error_code ec;
        const fs::path rel = fs::relative(projectRoot, repoRoot, ec);
        const std::string relText = ec ? std::string() : Utf8Generic(rel);
        if (!relText.empty() && relText != "." && !StartsWith(relText, "..")) prefix = relText + "/";
        return true;
    }

    fs::path absolute(const std::string& repoRelative) const { return repoRoot / PathFromUtf8(repoRelative); }

    std::string head() {
        const GitRun run = git({"rev-parse", "-q", "--verify", "HEAD"});
        return run.ok() ? Trim(run.out) : std::string();
    }

    GitRun status() {
        std::vector<std::string> args = {"status", "--porcelain=v1", "-z", "--branch", "--untracked-files=all"};
        if (!prefix.empty()) {
            args.push_back("--");
            args.push_back(prefix);
        }
        return git(args);
    }

    // A NUL-separated path list for --pathspec-from-file, which keeps long
    // selections clear of the command-line length limit.
    fs::path writePathspec(const std::vector<std::string>& paths) {
        std::string text;
        for (const std::string& path : paths) {
            text += path;
            text.push_back('\0');
        }
        const fs::path file = gitDir / "MODULARITY_PATHSPEC";
        WriteTextFile(file, text);
        return file;
    }

    std::vector<std::string> changedBetween(const std::string& from, const std::string& to) {
        if (from.empty() || to.empty() || from == to) return {};
        const GitRun run = git({"diff", "--name-only", "-z", from, to});
        std::vector<std::string> paths;
        for (const std::string& path : Split(run.out, '\0')) {
            if (!path.empty()) paths.push_back(path);
        }
        return paths;
    }
};

void StatFiles(const fs::path& repoRoot, std::vector<VcChange>& changes) {
    for (VcChange& change : changes) {
        // One stat per file: directory_entry caches what it read.
        std::error_code ec;
        const fs::directory_entry entry(repoRoot / PathFromUtf8(change.path), ec);
        if (ec) continue;
        const auto writeTime = entry.last_write_time(ec);
        if (ec) continue;
        change.modified = ToTimeT(writeTime);
        if (entry.is_regular_file(ec)) {
            const std::uintmax_t size = entry.file_size(ec);
            if (!ec) change.size = size;
        }
    }
}

void ClearRepoState(VcSnapshot& s) {
    s.isRepo = false;
    s.repoRoot.clear();
    s.projectPrefix.clear();
    s.headHash.clear();
    s.branch.clear();
    s.upstream.clear();
    s.detached = false;
    s.unborn = false;
    s.ahead = 0;
    s.behind = 0;
    s.remoteName.clear();
    s.remoteUrl.clear();
    s.mergeInProgress = false;
    s.lfsTracked = false;
    s.splitLimit = 0;
    s.splitPartSize = 0;
    s.splitJoinPending = 0;
    s.lastFetch = 0;
    s.changes.clear();
    s.history.clear();
    s.incoming.clear();
    s.incomingFiles.clear();
    s.branches.clear();
}

#pragma region Split files
// GitHub refuses any file over 100 MiB. Without LFS such a file is checked in as
// numbered parts under .modusplit/<path>.parts/, the real file is ignored, and
// every computer rebuilds it from the parts after a download. Outside this region
// a split file is just one pending row for the real path, flagged `split`.

void RemoveFiles(const std::vector<fs::path>& files);

constexpr std::uintmax_t kMiB = 1024ull * 1024ull;
const char* kSplitDir = ".modusplit";
const char* kIgnoreBegin = "# >>> Modularity split files: too big for the Git host, stored in .modusplit (managed, do not edit)";
const char* kIgnoreEnd = "# <<< Modularity split files";

struct SplitPolicy {
    std::uintmax_t limit = 0;     // files larger than this are split; 0 = never
    std::uintmax_t partSize = 0;
    bool enabled() const { return limit > 0; }
};

bool IsGitHubUrl(std::string url) {
    std::transform(url.begin(), url.end(), url.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return Contains(url, "github.com");
}

// GitHub's limit applies to github.com, and to projects with no server yet, so
// their history stays uploadable if GitHub is connected later. Other hosts set
// their own limits and keep files whole. MODULARITY_VC_SPLIT_LIMIT (bytes)
// forces a limit on any host, for testing.
SplitPolicy SplitPolicyFor(const std::string& remoteUrl) {
    SplitPolicy policy;
    if (const char* forced = std::getenv("MODULARITY_VC_SPLIT_LIMIT")) {
        policy.limit = std::strtoull(forced, nullptr, 10);
    } else if (remoteUrl.empty() || IsGitHubUrl(remoteUrl)) {
        policy.limit = kHardFileLimit;
    }
    // Parts stay just under the limit (99 MiB for GitHub) so they never sit on its edge.
    policy.partSize = policy.limit - std::min<std::uintmax_t>(policy.limit / 100, kMiB);
    return policy;
}

std::string SplitPartsRel(const std::string& path) { return std::string(kSplitDir) + "/" + path + ".parts"; }

std::string PartName(int index) {
    char name[16];
    std::snprintf(name, sizeof(name), "part%03d", index);
    return name;
}

std::string MegabyteLabel(std::uintmax_t bytes) {
    if (bytes < kMiB) return std::to_string(bytes) + " bytes";
    return std::to_string((bytes + kMiB / 2) / kMiB) + " MB";
}

struct SplitManifest {
    std::string path;  // the real file, repository-relative
    std::uintmax_t size = 0;
    std::string hash;  // git blob id of the whole file
    int parts = 0;
};

// A manifest comes from the server, so it is only trusted to rebuild a file at
// the path its own location names, and never outside the repository.
bool ReadManifest(const fs::path& repoRoot, const fs::path& file, SplitManifest& m) {
    for (const std::string& line : SplitLines(ReadTextFile(file))) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "path") m.path = value;
        else if (key == "size") m.size = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "hash") m.hash = value;
        else if (key == "parts") m.parts = std::atoi(value.c_str());
    }
    if (m.path.empty() || m.parts <= 0 || m.path[0] == '/' || Contains(m.path, "\\") || Contains(m.path, ":")) return false;
    for (const std::string& segment : Split(m.path, '/')) {
        if (segment.empty() || segment == "." || segment == "..") return false;
    }
    return (repoRoot / PathFromUtf8(SplitPartsRel(m.path)) / "manifest").lexically_normal() == file.lexically_normal();
}

std::string ManifestText(const SplitManifest& m) {
    return "# Modularity split file. The parts beside this are one file that is over the Git\n"
           "# host's size limit; the editor rebuilds it at `path` after every download.\n"
           "path=" + m.path + "\nsize=" + std::to_string(m.size) + "\nhash=" + m.hash +
           "\nparts=" + std::to_string(m.parts) + "\n";
}

std::map<std::string, SplitManifest> ReadManifests(const fs::path& repoRoot) {
    std::map<std::string, SplitManifest> manifests;
    const fs::path dir = repoRoot / kSplitDir;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return manifests;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().filename() != "manifest") continue;
        SplitManifest m;
        if (ReadManifest(repoRoot, it->path(), m)) manifests[m.path] = m;
    }
    return manifests;
}

// What each split file looked like the last time this computer split or rebuilt
// it, so a refresh can tell "you edited it" from "newer parts arrived" without
// hashing hundreds of megabytes every few seconds. Local only, kept in .git.
struct SplitState {
    std::uintmax_t size = 0;
    long long stamp = 0;
    std::string hash;
};
using SplitStates = std::map<std::string, SplitState>;

SplitStates ReadSplitStates(const fs::path& gitDir) {
    SplitStates states;
    for (const std::string& line : SplitLines(ReadTextFile(gitDir / "modularity-split-state"))) {
        const std::vector<std::string> f = Split(line, '\t');
        if (f.size() < 4) continue;
        states[f[3]] = {std::strtoull(f[0].c_str(), nullptr, 10), std::atoll(f[1].c_str()), f[2]};
    }
    return states;
}

void WriteSplitStates(const fs::path& gitDir, const SplitStates& states) {
    std::string text;
    for (const auto& entry : states) {
        text += std::to_string(entry.second.size) + "\t" + std::to_string(entry.second.stamp) + "\t" +
                entry.second.hash + "\t" + entry.first + "\n";
    }
    WriteTextFile(gitDir / "modularity-split-state", text);
}

bool StatForState(const fs::path& file, SplitState& state) {
    std::error_code ec;
    const fs::directory_entry entry(file, ec);
    if (ec || !entry.is_regular_file(ec)) return false;
    state.size = entry.file_size(ec);
    if (ec) return false;
    state.stamp = static_cast<long long>(entry.last_write_time(ec).time_since_epoch().count());
    return !ec;
}

std::string HashFile(Worker& w, const fs::path& file) {
    const GitRun run = w.git({"hash-object", "--no-filters", "--", Utf8Generic(file)});
    return run.ok() ? Trim(run.out) : std::string();
}

std::set<std::string> LfsTrackedPaths(Worker& w, const std::vector<std::string>& paths) {
    std::set<std::string> tracked;
    if (paths.empty()) return tracked;
    // -z output is "path\0attribute\0value\0" per file.
    std::vector<std::string> args = {"check-attr", "-z", "filter", "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    const std::vector<std::string> fields = Split(w.git(args).out, '\0');
    for (size_t i = 0; i + 2 < fields.size(); i += 3) {
        if (fields[i + 2] == "lfs") tracked.insert(fields[i]);
    }
    return tracked;
}

bool WriteParts(VcShared& shared, const fs::path& source, const fs::path& partsDir, std::uintmax_t partSize,
                const std::string& label, int& partCount, std::string& error) {
    std::error_code ec;
    fs::remove_all(partsDir, ec);
    fs::create_directories(partsDir, ec);
    std::ifstream in(source, std::ios::binary);
    if (ec || !in) {
        error = "could not read the file or create " + Utf8Generic(partsDir);
        return false;
    }
    std::vector<char> buffer(4 * kMiB);
    partCount = 0;
    while (in.peek() != std::char_traits<char>::eof()) {
        std::ofstream out(partsDir / PartName(partCount), std::ios::binary | std::ios::trunc);
        std::uintmax_t written = 0;
        while (written < partSize && in) {
            const std::uintmax_t want = std::min<std::uintmax_t>(buffer.size(), partSize - written);
            in.read(buffer.data(), static_cast<std::streamsize>(want));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            out.write(buffer.data(), got);
            written += static_cast<std::uintmax_t>(got);
        }
        if (!out) {
            error = "could not write " + Utf8Generic(partsDir / PartName(partCount));
            return false;
        }
        ++partCount;
        shared.setProgress("Splitting " + label + ": part " + std::to_string(partCount));
        if (shared.cancelled) {
            error = "Cancelled.";
            return false;
        }
    }
    return true;
}

// Writes next to the file and renames over it, so a failed rebuild never leaves half a file.
bool JoinParts(VcShared& shared, const fs::path& repoRoot, const SplitManifest& m, std::string& error) {
    const fs::path partsDir = repoRoot / PathFromUtf8(SplitPartsRel(m.path));
    const fs::path target = repoRoot / PathFromUtf8(m.path);
    fs::path temp = target;
    temp += ".modujoin";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = m.path + ": could not write " + Utf8Generic(temp);
            return false;
        }
        std::vector<char> buffer(4 * kMiB);
        for (int i = 0; i < m.parts; ++i) {
            std::ifstream in(partsDir / PartName(i), std::ios::binary);
            if (!in) {
                error = m.path + ": " + PartName(i) + " is missing";
                out.close();
                fs::remove(temp, ec);
                return false;
            }
            while (in) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                out.write(buffer.data(), in.gcount());
            }
            shared.setProgress("Rebuilding " + m.path + ": part " + std::to_string(i + 1) + "/" + std::to_string(m.parts));
        }
        if (!out) {
            error = m.path + ": writing failed (disk full?)";
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    if (fs::file_size(temp, ec) != m.size) {
        error = m.path + ": the parts add up to the wrong size";
        fs::remove(temp, ec);
        return false;
    }
    if (fs::exists(target, ec)) fs::remove(target, ec);
    fs::rename(temp, target, ec);
    if (ec) {
        error = m.path + ": could not replace the file (is it open in another program?)";
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

// Keeps the split files' real paths in a marked block at the end of .gitignore.
// Returns true when the file changed.
bool UpdateIgnoreBlock(const fs::path& gitignore, const std::set<std::string>& add, const std::set<std::string>& remove) {
    auto lineFor = [](const std::string& path) {
        std::string line = "/";
        for (const char c : path) {
            if (c == '\\' || c == '*' || c == '?' || c == '[') line.push_back('\\');
            line.push_back(c);
        }
        return line;
    };
    const std::string text = ReadTextFile(gitignore);
    std::string outside;
    std::set<std::string> block;
    bool inBlock = false;
    for (const std::string& raw : Split(text, '\n')) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == kIgnoreBegin) { inBlock = true; continue; }
        if (line == kIgnoreEnd) { inBlock = false; continue; }
        if (inBlock) {
            if (!line.empty()) block.insert(line);
        } else {
            outside += line + "\n";
        }
    }
    const std::set<std::string> before = block;
    for (const std::string& path : add) block.insert(lineFor(path));
    for (const std::string& path : remove) block.erase(lineFor(path));
    if (block == before) return false;

    while (outside.size() >= 2 && outside.compare(outside.size() - 2, 2, "\n\n") == 0) outside.pop_back();
    if (outside == "\n") outside.clear();
    std::string result = outside;
    if (!block.empty()) {
        if (!result.empty()) result += "\n";
        result += std::string(kIgnoreBegin) + "\n";
        for (const std::string& line : block) result += line + "\n";
        result += std::string(kIgnoreEnd) + "\n";
    }
    return WriteTextFile(gitignore, result);
}

struct SplitScan {
    std::map<std::string, SplitManifest> manifests;
    std::vector<SplitManifest> toJoin;  // newer parts than the file here, or no file yet
};

// Folds the split bookkeeping into `changes` (git's view, with sizes from
// StatFiles): part files leave the list, and each split file that needs a
// check-in gets one row for its real path.
SplitScan ScanSplitFiles(Worker& w, std::vector<VcChange>& changes, const SplitPolicy& policy) {
    SplitScan scan;
    scan.manifests = ReadManifests(w.repoRoot);

    const std::string splitPrefix = std::string(kSplitDir) + "/";
    std::set<std::string> conflicted;
    std::map<std::string, bool> dirtyParts;  // real path -> parts are all new (never checked in)
    changes.erase(std::remove_if(changes.begin(), changes.end(), [&](const VcChange& c) {
        if (!StartsWith(c.path, splitPrefix.c_str())) return false;
        const size_t cut = c.path.rfind(".parts/");
        if (cut != std::string::npos && cut > splitPrefix.size()) {
            const std::string real = c.path.substr(splitPrefix.size(), cut - splitPrefix.size());
            if (c.kind == VcChangeKind::Conflicted) conflicted.insert(real);
            const bool isNew = c.kind == VcChangeKind::Untracked || c.kind == VcChangeKind::Added;
            const auto it = dirtyParts.find(real);
            dirtyParts[real] = (it == dirtyParts.end() ? true : it->second) && isNew;
        }
        return true;
    }), changes.end());

    SplitStates states = ReadSplitStates(w.gitDir);
    bool statesChanged = false;
    for (const auto& entry : scan.manifests) {
        const SplitManifest& m = entry.second;
        VcChange row;
        row.path = m.path;
        row.split = true;
        if (conflicted.count(m.path)) {
            row.kind = VcChangeKind::Conflicted;
            changes.push_back(row);
            continue;
        }
        const auto known = states.find(m.path);
        SplitState now;
        if (!StatForState(w.absolute(m.path), now)) {
            if (known != states.end()) {
                row.kind = VcChangeKind::Deleted;  // this computer had it; the user removed it
                changes.push_back(row);
            } else {
                scan.toJoin.push_back(m);          // never rebuilt here yet
            }
            continue;
        }
        const bool unchangedHere = known != states.end() && known->second.size == now.size && known->second.stamp == now.stamp;
        now.hash = unchangedHere ? known->second.hash : HashFile(w, w.absolute(m.path));
        if (now.hash == m.hash) {
            if (!unchangedHere) {
                states[m.path] = now;
                statesChanged = true;
            }
            continue;
        }
        if (unchangedHere) {
            scan.toJoin.push_back(m);              // untouched here, newer parts arrived
            continue;
        }
        row.kind = VcChangeKind::Modified;         // edited here: the next check-in re-splits it
        row.size = now.size;
        std::error_code ec;
        row.modified = ToTimeT(fs::last_write_time(w.absolute(m.path), ec));
        changes.push_back(row);
    }
    if (statesChanged) WriteSplitStates(w.gitDir, states);

    // Parts that differ from the last check-in (say a check-in that failed after
    // splitting) still need one; hidden part files must not hide that.
    for (const auto& dirty : dirtyParts) {
        const bool listed = std::any_of(changes.begin(), changes.end(),
                                        [&](const VcChange& c) { return c.split && c.path == dirty.first; });
        if (listed || conflicted.count(dirty.first)) continue;
        VcChange row;
        row.path = dirty.first;
        row.split = true;
        row.kind = dirty.second ? VcChangeKind::Added : VcChangeKind::Modified;
        SplitState now;
        if (StatForState(w.absolute(dirty.first), now)) {
            row.size = now.size;
            std::error_code ec;
            row.modified = ToTimeT(fs::last_write_time(w.absolute(dirty.first), ec));
        } else {
            row.kind = VcChangeKind::Deleted;
        }
        changes.push_back(row);
    }

    if (policy.enabled()) {
        std::vector<std::string> oversized;
        for (const VcChange& c : changes) {
            if (!c.split && c.size > policy.limit && c.kind != VcChangeKind::Deleted && c.kind != VcChangeKind::Conflicted) {
                oversized.push_back(c.path);
            }
        }
        const std::set<std::string> lfs = LfsTrackedPaths(w, oversized);
        for (VcChange& c : changes) {
            if (!c.split && c.size > policy.limit && c.kind != VcChangeKind::Deleted &&
                c.kind != VcChangeKind::Conflicted && !lfs.count(c.path)) {
                c.split = true;
            }
        }
    }
    return scan;
}

// Brings split files in line with their parts after anything that moved files:
// rebuilds the ones with newer parts, and deletes ones whose parts were removed
// upstream if they were untouched here. Returns the paths it changed on disk.
std::vector<std::string> SyncSplitFiles(Worker& w, std::string& errors) {
    // Mid-merge a manifest or part can hold conflict markers; resolving a split
    // file rebuilds it, and the rest waits for the merge to finish.
    std::error_code ec;
    if (fs::exists(w.gitDir / "MERGE_HEAD", ec)) return {};
    std::vector<VcChange> none;
    const SplitScan scan = ScanSplitFiles(w, none, SplitPolicy());
    SplitStates states = ReadSplitStates(w.gitDir);
    std::vector<std::string> changed;
    for (const SplitManifest& m : scan.toJoin) {
        std::string error;
        if (!JoinParts(w.shared, w.repoRoot, m, error)) {
            errors += "\n  " + error;
            continue;
        }
        SplitState now;
        StatForState(w.absolute(m.path), now);
        now.hash = m.hash;
        states[m.path] = now;
        changed.push_back(m.path);
    }
    std::vector<fs::path> removed;
    for (auto it = states.begin(); it != states.end();) {
        if (scan.manifests.count(it->first)) {
            ++it;
            continue;
        }
        SplitState now;
        if (StatForState(w.absolute(it->first), now) && now.size == it->second.size && now.stamp == it->second.stamp) {
            removed.push_back(w.absolute(it->first));
            changed.push_back(it->first);
        }
        it = states.erase(it);
    }
    RemoveFiles(removed);
    WriteSplitStates(w.gitDir, states);
    return changed;
}

// Stages split rows for a check-in: (re)splits files over the limit, drops the
// parts of deleted ones, and stores files that shrank back under the limit as
// ordinary files again. Adds to the caller's `git add` and commit path lists.
bool StageSplitFiles(Worker& w, const std::vector<VcChange>& rows, const std::map<std::string, SplitManifest>& manifests,
                     const SplitPolicy& policy, std::vector<std::string>& toAdd, std::vector<std::string>& selected,
                     std::string& error) {
    SplitStates states = ReadSplitStates(w.gitDir);
    std::set<std::string> ignoreAdd;
    std::set<std::string> ignoreRemove;
    std::error_code ec;
    for (const VcChange& row : rows) {
        const std::string partsRel = SplitPartsRel(row.path);
        const fs::path partsDir = w.absolute(partsRel);
        const fs::path file = w.absolute(row.path);
        const bool fileInHead = w.git({"cat-file", "-e", "HEAD:" + row.path}).ok();
        const bool partsInHead = w.git({"cat-file", "-e", "HEAD:" + partsRel + "/manifest"}).ok();
        SplitState now;
        const bool exists = StatForState(file, now);
        if (exists && policy.enabled() && now.size > policy.limit) {
            now.hash = HashFile(w, file);
            int parts = 0;
            if (now.hash.empty()) {
                error = row.path + ": could not read it";
                return false;
            }
            if (!WriteParts(w.shared, file, partsDir, policy.partSize, row.path, parts, error)) {
                error = row.path + ": " + error;
                return false;
            }
            SplitManifest m;
            m.path = row.path;
            m.size = now.size;
            m.hash = now.hash;
            m.parts = parts;
            WriteTextFile(partsDir / "manifest", ManifestText(m));
            // Git stops tracking the whole file; the parts stand in for it.
            w.git({"rm", "--cached", "-q", "--ignore-unmatch", "--", row.path});
            ignoreAdd.insert(row.path);
            states[row.path] = now;
            toAdd.push_back(partsRel);
            selected.push_back(partsRel);
            if (fileInHead) selected.push_back(row.path);
            continue;
        }
        // Deleted, or small enough to store whole again: the parts go.
        if (manifests.count(row.path) || fs::exists(partsDir, ec)) {
            fs::remove_all(partsDir, ec);
            w.git({"rm", "-r", "-q", "--cached", "--ignore-unmatch", "--", partsRel});
            if (partsInHead) selected.push_back(partsRel);
            ignoreRemove.insert(row.path);
            states.erase(row.path);
        }
        if (exists) {
            toAdd.push_back(row.path);
            selected.push_back(row.path);
        } else if (fileInHead) {
            selected.push_back(row.path);
        }
    }
    // Before the caller's `git add`: adding a path .gitignore still lists is an error.
    if (UpdateIgnoreBlock(w.repoRoot / ".gitignore", ignoreAdd, ignoreRemove)) {
        toAdd.push_back(".gitignore");
        selected.push_back(".gitignore");
    }
    WriteSplitStates(w.gitDir, states);
    return true;
}

// Files in check-ins that are not uploaded yet which the host would refuse.
std::string OversizedUnpushed(Worker& w, const SplitPolicy& policy) {
    if (!policy.enabled()) return std::string();
    // blob:limit=N leaves out blobs of N bytes or more, and --filter-print-omitted lists them.
    const GitRun omitted = w.git({"rev-list", "--objects", "--filter=blob:limit=" + std::to_string(policy.limit + 1),
                                  "--filter-print-omitted", "HEAD", "--not", "--remotes"});
    std::set<std::string> big;
    for (const std::string& line : SplitLines(omitted.out)) {
        if (!line.empty() && line[0] == '~') big.insert(Trim(line.substr(1)));
    }
    if (big.empty()) return std::string();
    std::string list;
    for (const std::string& line : SplitLines(w.git({"rev-list", "--objects", "HEAD", "--not", "--remotes"}).out)) {
        const size_t space = line.find(' ');
        if (space != std::string::npos && big.count(line.substr(0, space))) list += "\n  " + line.substr(space + 1);
    }
    return list;
}

#pragma endregion

void RefreshDetails(Worker& w, VcSnapshot& s);

// Reads the repository into `s`. The cheap part (status) runs every time; the
// rest only when asked or when the status shows HEAD or the upstream moved.
void RefreshInto(Worker& w, const VcSnapshot& base, bool full, VcSnapshot& s) {
    s = base;
    s.checked = true;
    if (!w.locate()) {
        ClearRepoState(s);
        return;
    }
    s.isRepo = true;
    s.repoRoot = w.repoRoot;
    s.projectPrefix = w.prefix;
    s.headHash = w.head();

    const GitRun status = w.status();
    if (status.ok()) {
        ParseStatus(status.out, s);
        StatFiles(w.repoRoot, s.changes);
    }
    std::error_code ec;
    s.mergeInProgress = fs::exists(w.gitDir / "MERGE_HEAD", ec);

    const bool needFull = full || !base.isRepo || base.repoRoot != s.repoRoot ||
                          base.headHash != s.headHash || base.branch != s.branch ||
                          base.upstream != s.upstream || base.behind != s.behind;
    if (needFull) RefreshDetails(w, s);

    const SplitPolicy policy = SplitPolicyFor(s.remoteUrl);
    const SplitScan scan = ScanSplitFiles(w, s.changes, policy);
    s.splitLimit = policy.limit;
    s.splitPartSize = policy.partSize;
    s.splitJoinPending = static_cast<int>(scan.toJoin.size());
}

// Everything past the status: server, identity, history, incoming, branches.
void RefreshDetails(Worker& w, VcSnapshot& s) {
    std::error_code ec;
    s.remoteName.clear();
    s.remoteUrl.clear();
    const std::vector<std::string> remotes = SplitLines(w.git({"remote"}).out);
    if (!remotes.empty()) {
        s.remoteName = std::find(remotes.begin(), remotes.end(), "origin") != remotes.end() ? "origin" : remotes.front();
        s.remoteUrl = Trim(w.git({"remote", "get-url", s.remoteName}).out);
    }
    s.userName = Trim(w.git({"config", "user.name"}).out);
    s.userEmail = Trim(w.git({"config", "user.email"}).out);
    s.lfsTracked = Contains(ReadTextFile(w.repoRoot / ".gitattributes"), "filter=lfs");
    const auto fetchTime = fs::last_write_time(w.gitDir / "FETCH_HEAD", ec);
    s.lastFetch = ec ? 0 : ToTimeT(fetchTime);

    s.history.clear();
    s.incoming.clear();
    s.incomingFiles.clear();
    if (!s.unborn && !s.headHash.empty()) {
        s.history = ParseLog(w.git({"log", "-n", "200", kLogFormat, "HEAD"}).out);
        if (!s.remoteName.empty()) {
            const GitRun local = w.git({"rev-list", "--max-count=500", "HEAD", "--not", "--remotes"});
            std::set<std::string> localHashes;
            for (const std::string& hash : SplitLines(local.out)) localHashes.insert(Trim(hash));
            for (VcCommit& commit : s.history) commit.local = localHashes.count(commit.hash) != 0;
        }
        if (!s.upstream.empty()) {
            s.incoming = ParseLog(w.git({"log", "-n", "200", kLogFormat, "HEAD..@{u}"}).out);
            if (!s.incoming.empty()) {
                s.incomingFiles = ParseNameStatus(w.git({"diff", "--name-status", "-z", "HEAD...@{u}"}).out);
            }
        }
    }

    s.branches.clear();
    const GitRun refs = w.git({"for-each-ref",
        "--format=%(refname)%1f%(refname:short)%1f%(upstream:short)%1f%(upstream:track)%1f"
        "%(committerdate:unix)%1f%(contents:subject)%1e",
        "refs/heads", "refs/remotes"});
    for (std::string record : Split(refs.out, '\x1e')) {
        while (!record.empty() && (record.front() == '\n' || record.front() == '\r')) record.erase(0, 1);
        const std::vector<std::string> f = Split(record, '\x1f');
        if (f.size() < 6) continue;
        VcBranch branch;
        branch.remote = StartsWith(f[0], "refs/remotes/");
        if (branch.remote && f[0].size() >= 5 && f[0].compare(f[0].size() - 5, 5, "/HEAD") == 0) continue;
        branch.name = f[1];
        branch.upstream = f[2];
        branch.track = f[3];
        branch.time = static_cast<std::time_t>(std::atoll(f[4].c_str()));
        branch.subject = f[5];
        branch.current = !branch.remote && !s.detached && branch.name == s.branch;
        s.branches.push_back(std::move(branch));
    }
}

void Fail(VcJobOutput& out, const std::string& message, const std::string& detail = std::string()) {
    out.result.ok = false;
    out.result.message = message;
    out.result.detail = detail;
}

void Succeed(VcJobOutput& out, const std::string& message) {
    out.result.ok = true;
    out.result.message = message;
}

// Every mutating job ends by republishing the repository state.
void Publish(Worker& w, const VcSnapshot& base, VcJobOutput& out) {
    RefreshInto(w, base, true, out.snapshot);
    out.hasSnapshot = true;
    out.result.repoRoot = w.repoRoot;
}

// The pending changes as the panel shows them, split files included.
std::map<std::string, VcChange> ChangesByPath(Worker& w, const SplitPolicy& policy, SplitScan* scanOut = nullptr) {
    VcSnapshot current;
    const GitRun run = w.status();
    if (run.ok()) ParseStatus(run.out, current);
    StatFiles(w.repoRoot, current.changes);
    SplitScan scan = ScanSplitFiles(w, current.changes, policy);
    std::map<std::string, VcChange> byPath;
    for (VcChange& change : current.changes) byPath[change.path] = change;
    if (scanOut) *scanOut = std::move(scan);
    return byPath;
}

// After a job moved files: rebuild split files and add them to the changed paths.
// Returns what failed, for ReportSplitErrors once the job has its message.
std::string SyncSplitAfter(Worker& w, VcJobOutput& out) {
    std::string errors;
    for (const std::string& path : SyncSplitFiles(w, errors)) out.result.changedPaths.push_back(path);
    return errors;
}

void ReportSplitErrors(VcJobOutput& out, const std::string& errors) {
    if (errors.empty()) return;
    out.result.ok = false;
    out.result.message += " Some large files could not be rebuilt from their parts.";
    out.result.detail += (out.result.detail.empty() ? "" : "\n") + std::string("Not rebuilt:") + errors;
}

// Sends files to the Recycle Bin where there is one: Undo on a new file should
// not be the one irreversible action in the panel.
void RemoveFiles(const std::vector<fs::path>& files) {
#ifdef _WIN32
    std::wstring list;
    for (const fs::path& file : files) {
        std::error_code ec;
        if (!fs::exists(file, ec)) continue;
        list += file.wstring();
        list.push_back(L'\0');
    }
    if (list.empty()) return;
    list.push_back(L'\0');
    // The Recycle Bin goes through the shell, which wants COM on this worker thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int shellResult = SHFileOperationW(&op);
    if (SUCCEEDED(com)) CoUninitialize();
    if (shellResult == 0) return;
#endif
    for (const fs::path& file : files) {
        std::error_code ec;
        fs::remove(file, ec);
    }
}

std::string CountLabel(size_t count, const char* noun) {
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

// Changed files as the user thinks of them: split parts are not files of theirs.
size_t CountProjectFiles(const std::vector<std::string>& paths) {
    const std::string splitPrefix = std::string(kSplitDir) + "/";
    return static_cast<size_t>(std::count_if(paths.begin(), paths.end(),
                                             [&](const std::string& p) { return !StartsWith(p, splitPrefix.c_str()); }));
}

#pragma endregion

} // namespace

#pragma region VersionControl

VersionControl::VersionControl() = default;

VersionControl::~VersionControl() {
    queue.clear();
    if (shared) shared->kill();
    if (runningActive && running.valid()) running.wait();
}

void VersionControl::update(const fs::path& root, double now, bool panelVisible) {
    if (root != projectRoot) {
        projectRoot = root;
        ++generation;
        // Work queued for the previous project no longer applies. A clone is not
        // tied to a project, so it survives the switch.
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [](const Job& job) { return job.kind != VcJobKind::Clone && job.kind != VcJobKind::Probe; }),
                    queue.end());
        VcSnapshot fresh;
        fresh.probed = snap.probed;
        fresh.gitAvailable = snap.gitAvailable;
        fresh.gitVersion = snap.gitVersion;
        fresh.lfsAvailable = snap.lfsAvailable;
        snap = std::move(fresh);
        commitFileCache.clear();
        backgroundError.clear();
        autoJoinHead.clear();
        lastStatusRefresh = -1.0;
        lastAutoFetch = -1.0;
    }

    if (runningActive && running.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        finishRunning();
    }

    const bool probing = hasQueued(VcJobKind::Probe) || (runningActive && runningJob.kind == VcJobKind::Probe);
    if (!snap.probed && !probing) probe();

    const bool idle = !runningActive && queue.empty();
    if (idle && snap.gitAvailable && !projectRoot.empty()) {
        const double interval = panelVisible ? 4.0 : 30.0;
        if (snap.isRepo && snap.splitJoinPending > 0 && snap.headHash != autoJoinHead) {
            // Parts arrived without the editor doing the download (a pull from a
            // terminal, say). Once per check-in, so a failing rebuild can't loop.
            autoJoinHead = snap.headHash;
            joinSplitFiles();
        } else if (lastStatusRefresh < 0.0 || now - lastStatusRefresh >= interval) {
            lastStatusRefresh = now;
            enqueueRefresh(!snap.checked);
        } else if (panelVisible && snap.isRepo && !snap.remoteName.empty() &&
                   (lastAutoFetch < 0.0 || now - lastAutoFetch >= 300.0)) {
            // Keeps Incoming Changes current. Never prompts for a login: a
            // window popping up on its own would be worse than a stale count.
            lastAutoFetch = now;
            fetch(true);
        }
    }

    if (!runningActive) startNext();
}

bool VersionControl::isBusy() const {
    if (runningActive && runningJob.user) return true;
    for (const Job& job : queue) {
        if (job.user) return true;
    }
    return false;
}

const char* VersionControl::busyLabel() const {
    const Job* job = runningActive ? &runningJob : (queue.empty() ? nullptr : &queue.front());
    return job ? JobLabel(job->kind) : "";
}

const char* VersionControl::JobLabel(VcJobKind kind) {
    switch (kind) {
        case VcJobKind::Probe: return "Looking for Git";
        case VcJobKind::Refresh: return "Refreshing";
        case VcJobKind::Fetch: return "Checking for incoming changes";
        case VcJobKind::Init: return "Setting up version control";
        case VcJobKind::SetRemote: return "Connecting to server";
        case VcJobKind::SetIdentity: return "Saving settings";
        case VcJobKind::CheckIn: return "Checking in";
        case VcJobKind::Push: return "Uploading";
        case VcJobKind::Pull: return "Getting changes";
        case VcJobKind::Merge: return "Merging changes";
        case VcJobKind::AbortMerge: return "Cancelling merge";
        case VcJobKind::Discard: return "Undoing changes";
        case VcJobKind::Resolve: return "Resolving conflicts";
        case VcJobKind::CreateBranch: return "Creating branch";
        case VcJobKind::SwitchBranch: return "Switching branch";
        case VcJobKind::DeleteBranch: return "Deleting branch";
        case VcJobKind::Ignore: return "Updating .gitignore";
        case VcJobKind::CommitFiles: return "Loading changeset";
        case VcJobKind::Clone: return "Downloading project";
        case VcJobKind::DisableLfs: return "Converting LFS files";
        case VcJobKind::JoinSplit: return "Rebuilding large files";
    }
    return "";
}

std::string VersionControl::progressLine() const {
    if (!runningActive || !shared) return std::string();
    std::lock_guard<std::mutex> lock(shared->mutex);
    return shared->progress;
}

double VersionControl::runningSeconds() const {
    if (!runningActive || !shared) return 0.0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - shared->started).count();
}

void VersionControl::cancel() {
    queue.erase(std::remove_if(queue.begin(), queue.end(), [](const Job& job) { return job.user; }), queue.end());
    if (runningActive && shared) shared->kill();
}

bool VersionControl::takeResult(VcResult& out) {
    if (results.empty()) return false;
    out = std::move(results.front());
    results.pop_front();
    return true;
}

const std::vector<VcChange>* VersionControl::commitFiles(const std::string& hash) const {
    const auto it = commitFileCache.find(hash);
    return it != commitFileCache.end() ? &it->second : nullptr;
}

bool VersionControl::hasQueued(VcJobKind kind) const {
    for (const Job& job : queue) {
        if (job.kind == kind) return true;
    }
    return false;
}

void VersionControl::enqueue(VcJobKind kind, bool user, JobFn fn) {
    if (user) {
        // A user operation republishes everything when it finishes, so a
        // background refresh waiting ahead of it would only delay it.
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [](const Job& job) { return !job.user && job.kind == VcJobKind::Refresh; }),
                    queue.end());
    }
    Job job;
    job.kind = kind;
    job.user = user;
    job.fn = std::move(fn);
    queue.push_back(std::move(job));
}

void VersionControl::startNext() {
    if (runningActive || queue.empty()) return;
    runningJob = std::move(queue.front());
    queue.pop_front();
    runningActive = true;
    runningGeneration = generation;
    shared = std::make_shared<VcShared>();

    std::shared_ptr<VcShared> jobShared = shared;
    const fs::path root = projectRoot;
    const VcSnapshot base = snap;
    const JobFn fn = runningJob.fn;
    const VcJobKind kind = runningJob.kind;
    const bool user = runningJob.user;
    running = std::async(std::launch::async, [jobShared, root, base, fn, kind, user]() {
        VcJobOutput out;
        out.result.kind = kind;
        out.result.silent = !user;
        try {
            fn(root, base, *jobShared, out);
        } catch (const std::exception& e) {
            Fail(out, std::string("Version control error: ") + e.what());
        } catch (...) {
            Fail(out, "Version control error.");
        }
        if (jobShared->cancelled) {
            // Git was killed part-way, so whatever the job read afterwards is
            // wrong: a killed rev-parse looks exactly like "not a repository".
            out.cancelled = true;
            out.hasSnapshot = false;
            out.result.ok = false;
            out.result.detail.clear();
        }
        return out;
    });
}

void VersionControl::finishRunning() {
    VcJobOutput out;
    try {
        out = running.get();
    } catch (const std::exception& e) {
        out.result.kind = runningJob.kind;
        Fail(out, std::string("Version control error: ") + e.what());
    }
    runningActive = false;
    const VcJobKind kind = runningJob.kind;
    runningJob = Job();

    if (out.cancelled) {
        lastStatusRefresh = -1.0;  // re-read what git left behind
        if (out.result.silent || runningGeneration != generation) return;
        out.result.message = std::string(JobLabel(kind)) + " was cancelled.";
        results.push_back(std::move(out.result));
        return;
    }

    // The project changed while this ran; its view of the repository is stale.
    const bool current = runningGeneration == generation;
    if (kind == VcJobKind::Probe) {
        snap.probed = out.snapshot.probed;
        snap.gitAvailable = out.snapshot.gitAvailable;
        snap.gitVersion = out.snapshot.gitVersion;
        snap.lfsAvailable = out.snapshot.lfsAvailable;
        lastStatusRefresh = -1.0;
    } else if (out.hasSnapshot && current) {
        out.snapshot.probed = snap.probed;
        out.snapshot.gitAvailable = snap.gitAvailable;
        out.snapshot.gitVersion = snap.gitVersion;
        out.snapshot.lfsAvailable = snap.lfsAvailable;
        snap = std::move(out.snapshot);
    }
    // Cached even when it failed (as an empty list), or the panel would ask again every frame.
    if (kind == VcJobKind::CommitFiles && current && !out.commitHash.empty()) {
        commitFileCache[out.commitHash] = std::move(out.commitFiles);
    }
    if (!current && kind != VcJobKind::Clone && kind != VcJobKind::Probe) return;

    if (out.result.silent) {
        if (out.result.ok) backgroundError.clear();
        else backgroundError = out.result.message;
        // Background work that rewrote files still has to reach the editor.
        if (out.result.changedPaths.empty()) return;
    }
    results.push_back(std::move(out.result));
}

#pragma endregion

#pragma region Operations

void VersionControl::probe() {
    enqueue(VcJobKind::Probe, false, [](const fs::path&, const VcSnapshot&, VcShared& shared, VcJobOutput& out) {
        out.snapshot.probed = true;
        const GitRun version = RunGit(&shared, fs::path(), {"--version"}, false, false);
        out.snapshot.gitAvailable = version.ok();
        out.snapshot.gitVersion = Trim(version.out);
        if (out.snapshot.gitAvailable) {
            out.snapshot.lfsAvailable = RunGit(&shared, fs::path(), {"lfs", "version"}, false, false).ok();
            Succeed(out, out.snapshot.gitVersion);
        } else {
            Fail(out, Explain(version, "Starting git"));
        }
    });
}

void VersionControl::enqueueRefresh(bool full) {
    if (hasQueued(VcJobKind::Refresh)) return;
    enqueue(VcJobKind::Refresh, false, [full](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        RefreshInto(w, base, full, out.snapshot);
        out.hasSnapshot = true;
        out.result.ok = true;
    });
}

void VersionControl::refresh(bool full) {
    lastStatusRefresh = -1.0;
    enqueueRefresh(full);
}

void VersionControl::fetch(bool silent) {
    enqueue(VcJobKind::Fetch, !silent, [silent](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        if (base.remoteName.empty()) return Fail(out, "No server is set up for this project.");
        const GitRun run = w.git({"fetch", "--progress", "--prune", base.remoteName}, true, !silent);
        Publish(w, base, out);
        if (!run.ok()) return Fail(out, Explain(run, "Checking for changes"), Detail(run));
        const int behind = out.snapshot.behind;
        Succeed(out, behind > 0 ? CountLabel(static_cast<size_t>(behind), "incoming changeset") + " on the server."
                                : "You're up to date with the server.");
    });
}

void VersionControl::init(const VcInitOptions& options) {
    enqueue(VcJobKind::Init, true, [options](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        const GitRun init = w.gitAt(root, {"init"});
        if (!init.ok()) return Fail(out, Explain(init, "Setting up version control"), Detail(init));
        // "main", like GitHub, unless the user picked their own default. Only the
        // user's global config counts: Git for Windows' installer writes
        // init.defaultBranch=master into the system config on its own.
        const GitRun defaultBranch = w.gitAt(root, {"config", "--global", "--get", "init.defaultBranch"});
        if (Trim(defaultBranch.out).empty()) w.gitAt(root, {"symbolic-ref", "HEAD", "refs/heads/main"});
        if (!w.locate()) return Fail(out, "Git created the repository but cannot read it back.", Detail(init));

        std::error_code ec;
        if (options.writeGitignore && !fs::exists(root / ".gitignore", ec)) {
            WriteTextFile(root / ".gitignore", kGitignore);
        }
        const bool lfs = options.useLfs && base.lfsAvailable;
        if (!fs::exists(root / ".gitattributes", ec)) {
            WriteTextFile(root / ".gitattributes", std::string(kAttributes) + (lfs ? LfsAttributes() : std::string()));
        }
        if (lfs) w.git({"lfs", "install", "--local"});

        std::string note;
        if (!options.remoteUrl.empty()) {
            w.git({"remote", "add", "origin", options.remoteUrl});
            const GitRun fetch = w.git({"fetch", "--progress", "origin"}, true);
            if (!fetch.ok()) note = " " + Explain(fetch, "Connecting to the server");
        }
        Publish(w, base, out);
        Succeed(out, "Version control is set up. Check in your files to make the first backup." + note);
    });
}

void VersionControl::setRemote(const std::string& url) {
    enqueue(VcJobKind::SetRemote, true, [url](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        const std::string name = base.remoteName.empty() ? "origin" : base.remoteName;
        const GitRun set = base.remoteName.empty() ? w.git({"remote", "add", name, url})
                                                   : w.git({"remote", "set-url", name, url});
        if (!set.ok()) return Fail(out, Explain(set, "Saving the server address"), Detail(set));

        const GitRun fetch = w.git({"fetch", "--progress", "--prune", name}, true);
        if (fetch.ok() && !base.branch.empty() && !base.unborn && !base.detached && base.upstream.empty()) {
            // The server already has this branch: track it so Incoming Changes works.
            const std::string remoteBranch = name + "/" + base.branch;
            if (w.git({"rev-parse", "-q", "--verify", "refs/remotes/" + remoteBranch}).ok()) {
                w.git({"branch", "--set-upstream-to=" + remoteBranch});
            }
        }
        Publish(w, base, out);
        if (!fetch.ok()) {
            return Fail(out, "Server address saved, but " + Explain(fetch, "connecting"), Detail(fetch));
        }
        Succeed(out, "Connected to " + url);
    });
}

void VersionControl::setIdentity(const std::string& name, const std::string& email, bool global) {
    enqueue(VcJobKind::SetIdentity, true, [name, email, global](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        w.locate();
        std::vector<std::string> scope = {"config"};
        if (global) scope.push_back("--global");
        std::vector<std::string> setName = scope;
        setName.insert(setName.end(), {"user.name", name});
        std::vector<std::string> setEmail = scope;
        setEmail.insert(setEmail.end(), {"user.email", email});
        const GitRun a = w.git(setName);
        const GitRun b = w.git(setEmail);
        Publish(w, base, out);
        if (!a.ok() || !b.ok()) return Fail(out, Explain(a.ok() ? b : a, "Saving your name and email"));
        Succeed(out, "Check-ins will be signed as " + name + " <" + email + ">.");
    });
}

void VersionControl::checkIn(const std::vector<std::string>& paths, const std::string& message, bool pushAfter) {
    enqueue(VcJobKind::CheckIn, true, [paths, message, pushAfter](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        if (Trim(message).empty()) return Fail(out, "Add a comment describing what changed.");
        const fs::path messageFile = w.gitDir / "MODULARITY_CHECKIN_MSG";
        WriteTextFile(messageFile, message);

        const SplitPolicy policy = SplitPolicyFor(base.remoteUrl);
        SplitScan scan;
        const std::map<std::string, VcChange> current = ChangesByPath(w, policy, &scan);
        std::error_code ec;
        size_t fileCount = 0;
        GitRun commit;
        if (fs::exists(w.gitDir / "MERGE_HEAD", ec)) {
            // Git refuses a partial merge commit, so a merge checks in everything.
            for (const auto& entry : current) {
                if (entry.second.kind == VcChangeKind::Conflicted) {
                    return Fail(out, "Choose which version to keep for every conflicted file first.");
                }
            }
            std::vector<std::string> addArgs = {"add", "-A"};
            if (!w.prefix.empty()) addArgs.insert(addArgs.end(), {"--", w.prefix});
            const GitRun add = w.git(addArgs);
            if (!add.ok()) return Fail(out, Explain(add, "Checking in"), Detail(add));
            fileCount = current.size();
            commit = w.git({"commit", "-F", Utf8Generic(messageFile)});
        } else {
            // `git add` refuses a path that is in neither the index nor the folder,
            // which is exactly what a staged deletion or a rename's old side is;
            // those only need naming in the commit.
            std::vector<std::string> toAdd;
            std::vector<std::string> selected;
            std::vector<VcChange> splitRows;
            for (const std::string& path : paths) {
                const auto it = current.find(path);
                if (it == current.end()) continue;  // changed back since the list was drawn
                const VcChange& change = it->second;
                ++fileCount;  // rows, so a move counts once
                if (change.split) {
                    splitRows.push_back(change);
                    continue;
                }
                selected.push_back(path);
                if (change.indexCode != 'D') toAdd.push_back(path);
                if (!change.oldPath.empty()) {
                    selected.push_back(change.oldPath);
                    if (change.indexCode != 'R') toAdd.push_back(change.oldPath);
                }
            }
            if (fileCount == 0) return Fail(out, "None of the selected files have changes to check in.");

            std::string splitError;
            if (!splitRows.empty() &&
                !StageSplitFiles(w, splitRows, scan.manifests, policy, toAdd, selected, splitError)) {
                return Fail(out, "Could not split a large file for check-in: " + splitError);
            }

            if (!toAdd.empty()) {
                const std::string addSpec = "--pathspec-from-file=" + Utf8Generic(w.writePathspec(toAdd));
                const GitRun add = w.git({"add", "-A", addSpec, "--pathspec-file-nul"});
                if (!add.ok()) return Fail(out, Explain(add, "Checking in"), Detail(add));
            }
            const std::string pathspec = "--pathspec-from-file=" + Utf8Generic(w.writePathspec(selected));
            commit = w.git({"commit", "--only", pathspec, "--pathspec-file-nul", "-F", Utf8Generic(messageFile)});
            fs::remove(w.gitDir / "MODULARITY_PATHSPEC", ec);
        }
        fs::remove(messageFile, ec);
        if (!commit.ok()) {
            Publish(w, base, out);
            return Fail(out, Explain(commit, "Checking in"), Detail(commit));
        }

        std::string summary = "Checked in " + CountLabel(fileCount, "file") + ".";
        if (pushAfter && !base.remoteName.empty()) {
            const std::string oversized = OversizedUnpushed(w, policy);
            if (!oversized.empty()) {
                Publish(w, base, out);
                return Fail(out, summary + " It is saved on this computer only: some check-ins that aren't uploaded "
                                 "yet hold files over the server's " + MegabyteLabel(policy.limit) + " limit, from "
                                 "before they were split automatically.",
                            "Over the limit:" + oversized);
            }
            const GitRun push = w.git({"push", "--progress", "-u", base.remoteName, "HEAD"}, true);
            Publish(w, base, out);
            if (!push.ok()) {
                return Fail(out, summary + " It is saved on this computer only: " + Explain(push, "uploading"), Detail(push));
            }
            return Succeed(out, "Checked in and uploaded " + CountLabel(fileCount, "file") + ".");
        }
        Publish(w, base, out);
        Succeed(out, summary);
    });
}

void VersionControl::push() {
    enqueue(VcJobKind::Push, true, [](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        if (base.remoteName.empty()) return Fail(out, "Set a server address in Version Control settings first.");
        if (w.head().empty()) return Fail(out, "Check something in before uploading.");
        const SplitPolicy policy = SplitPolicyFor(base.remoteUrl);
        const std::string oversized = OversizedUnpushed(w, policy);
        if (!oversized.empty()) {
            return Fail(out, "Uploading stopped: some check-ins that aren't uploaded yet hold files over the "
                             "server's " + MegabyteLabel(policy.limit) + " limit, from before they were split "
                             "automatically.",
                        "Over the limit:" + oversized);
        }
        const GitRun push = w.git({"push", "--progress", "-u", base.remoteName, "HEAD"}, true);
        Publish(w, base, out);
        if (!push.ok()) return Fail(out, Explain(push, "Uploading"), Detail(push));
        Succeed(out, "Uploaded to " + base.remoteName + ".");
    });
}

void VersionControl::pull() {
    enqueue(VcJobKind::Pull, true, [](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        if (base.upstream.empty()) return Fail(out, "This branch is not on the server yet, so there is nothing to get.");
        const std::string before = w.head();
        const GitRun fetch = w.git({"fetch", "--progress", base.remoteName.empty() ? "origin" : base.remoteName}, true);
        if (!fetch.ok()) {
            Publish(w, base, out);
            return Fail(out, Explain(fetch, "Getting changes"), Detail(fetch));
        }
        // Fast-forward only: when both sides have new check-ins the user picks Merge on purpose.
        const GitRun merge = w.git({"merge", "--ff-only", "@{u}"}, true);
        out.result.changedPaths = w.changedBetween(before, w.head());
        const std::string splitErrors = SyncSplitAfter(w, out);
        const size_t changed = CountProjectFiles(out.result.changedPaths);
        Publish(w, base, out);
        if (!merge.ok()) {
            Fail(out, Explain(merge, "Getting changes"), Detail(merge));
        } else {
            Succeed(out, changed == 0 ? "Already up to date."
                                      : "Got " + CountLabel(changed, "changed file") + " from the server.");
        }
        ReportSplitErrors(out, splitErrors);
    });
}

void VersionControl::merge() {
    enqueue(VcJobKind::Merge, true, [](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        if (base.upstream.empty()) return Fail(out, "This branch is not on the server yet.");
        const std::string before = w.head();
        const GitRun fetch = w.git({"fetch", "--progress", base.remoteName.empty() ? "origin" : base.remoteName}, true);
        if (!fetch.ok()) {
            Publish(w, base, out);
            return Fail(out, Explain(fetch, "Merging"), Detail(fetch));
        }
        // Unrelated histories happen when the server repository was created with
        // its own first commit (a README) before this project was connected.
        const GitRun merge = w.git({"merge", "--no-edit", "--allow-unrelated-histories", "@{u}"}, true);
        std::vector<std::string> touched;
        for (const std::string& path : Split(w.git({"diff", "--name-only", "-z", before.empty() ? "HEAD" : before}).out, '\0')) {
            if (!path.empty()) touched.push_back(path);
        }
        out.result.changedPaths = touched;
        const std::string splitErrors = SyncSplitAfter(w, out);  // skipped while conflicts remain
        Publish(w, base, out);
        if (out.snapshot.mergeInProgress) {
            size_t conflicts = 0;
            for (const VcChange& change : out.snapshot.changes) conflicts += change.kind == VcChangeKind::Conflicted ? 1 : 0;
            return Fail(out, CountLabel(conflicts, "file") + " changed on both sides. Pick Keep Mine or Take Theirs "
                             "for each in Pending Changes, then check in to finish the merge.");
        }
        if (!merge.ok()) return Fail(out, Explain(merge, "Merging"), Detail(merge));
        Succeed(out, "Merged the server's changes. Upload to share the result.");
        ReportSplitErrors(out, splitErrors);
    });
}

void VersionControl::abortMerge() {
    enqueue(VcJobKind::AbortMerge, true, [](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        for (const std::string& path : Split(w.git({"diff", "--name-only", "-z", "HEAD"}).out, '\0')) {
            if (!path.empty()) out.result.changedPaths.push_back(path);
        }
        const GitRun abort = w.git({"merge", "--abort"});
        const std::string splitErrors = SyncSplitAfter(w, out);
        Publish(w, base, out);
        if (!abort.ok()) return Fail(out, Explain(abort, "Cancelling the merge"), Detail(abort));
        Succeed(out, "Merge cancelled. Your files are back to how they were.");
        ReportSplitErrors(out, splitErrors);
    });
}

void VersionControl::discard(const std::vector<std::string>& paths) {
    enqueue(VcJobKind::Discard, true, [paths](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        SplitScan scan;
        const std::map<std::string, VcChange> current = ChangesByPath(w, SplitPolicyFor(base.remoteUrl), &scan);
        const bool unborn = w.head().empty();

        std::vector<std::string> restore;   // back to the last check-in
        std::vector<std::string> unstage;   // new in the index: forget, then delete
        std::vector<fs::path> remove;       // new files on disk
        std::vector<std::string> rejoin;    // split files: rebuild from the checked-in parts
        std::set<std::string> unignore;     // split files that were never checked in
        std::error_code ec;
        for (const std::string& path : paths) {
            const auto it = current.find(path);
            if (it == current.end()) continue;
            const VcChange& change = it->second;
            out.result.changedPaths.push_back(path);
            if (change.split) {
                const std::string partsRel = SplitPartsRel(path);
                if (w.git({"cat-file", "-e", "HEAD:" + partsRel + "/manifest"}).ok()) {
                    restore.push_back(partsRel);  // parts back to the last check-in, then rebuild
                    rejoin.push_back(path);
                    continue;
                }
                if (fs::exists(w.absolute(partsRel), ec)) {
                    fs::remove_all(w.absolute(partsRel), ec);  // split for a check-in that never happened
                    unstage.push_back(partsRel);
                    unignore.insert(path);
                }
            }
            switch (change.kind) {
                case VcChangeKind::Untracked:
                    remove.push_back(w.absolute(path));
                    break;
                case VcChangeKind::Added:
                    unstage.push_back(path);
                    remove.push_back(w.absolute(path));
                    break;
                case VcChangeKind::Renamed:
                    unstage.push_back(path);
                    remove.push_back(w.absolute(path));
                    restore.push_back(change.oldPath);
                    out.result.changedPaths.push_back(change.oldPath);
                    break;
                default:
                    if (unborn) {
                        unstage.push_back(path);
                        remove.push_back(w.absolute(path));
                    } else {
                        restore.push_back(path);
                    }
                    break;
            }
        }
        if (out.result.changedPaths.empty()) return Fail(out, "None of the selected files have changes to undo.");

        GitRun failed;
        failed.launched = true;
        failed.exitCode = 0;
        if (!unstage.empty()) {
            const std::string pathspec = "--pathspec-from-file=" + Utf8Generic(w.writePathspec(unstage));
            const GitRun run = w.git({"rm", "--cached", "-q", "-r", "--ignore-unmatch", pathspec, "--pathspec-file-nul"});
            if (!run.ok()) failed = run;
        }
        if (!restore.empty() && failed.ok()) {
            const std::string pathspec = "--pathspec-from-file=" + Utf8Generic(w.writePathspec(restore));
            const GitRun run = w.git({"restore", "--source=HEAD", "--staged", "--worktree", pathspec, "--pathspec-file-nul"});
            if (!run.ok()) failed = run;
        }
        fs::remove(w.gitDir / "MODULARITY_PATHSPEC", ec);
        if (failed.ok()) RemoveFiles(remove);
        if (!unignore.empty()) UpdateIgnoreBlock(w.repoRoot / ".gitignore", {}, unignore);

        std::string splitErrors;
        if (failed.ok() && !rejoin.empty()) {
            const std::map<std::string, SplitManifest> manifests = ReadManifests(w.repoRoot);
            SplitStates states = ReadSplitStates(w.gitDir);
            for (const std::string& path : rejoin) {
                const auto m = manifests.find(path);
                std::string error;
                if (m == manifests.end() || !JoinParts(shared, w.repoRoot, m->second, error)) {
                    splitErrors += "\n  " + (error.empty() ? path + ": its parts are missing" : error);
                    continue;
                }
                SplitState now;
                StatForState(w.absolute(path), now);
                now.hash = m->second.hash;
                states[path] = now;
            }
            WriteSplitStates(w.gitDir, states);
        }
        Publish(w, base, out);
        if (!failed.ok()) return Fail(out, Explain(failed, "Undoing changes"), Detail(failed));
        Succeed(out, "Undid changes to " + CountLabel(paths.size(), "file") + ".");
        ReportSplitErrors(out, splitErrors);
    });
}

void VersionControl::resolve(const std::vector<std::string>& paths, bool keepMine) {
    enqueue(VcJobKind::Resolve, true, [paths, keepMine](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        std::string failures;
        std::error_code ec;
        for (const std::string& path : paths) {
            // A split file's conflict is in its parts; they are picked as a set, then rebuilt.
            const std::string partsRel = SplitPartsRel(path);
            const bool split = fs::is_directory(w.absolute(partsRel), ec);
            const std::string target = split ? partsRel : path;
            const GitRun pick = w.git({"checkout", keepMine ? "--ours" : "--theirs", "--", target});
            // No version on the chosen side means that side deleted the file.
            const GitRun stage = pick.ok() ? w.git({"add", "-A", "--", target}) : w.git({"rm", "-r", "-q", "--", target});
            if (!stage.ok()) failures += "\n  " + path + ": " + KeyLine(stage);
            out.result.changedPaths.push_back(path);
            if (split && pick.ok() && stage.ok()) {
                const std::map<std::string, SplitManifest> manifests = ReadManifests(w.repoRoot);
                const auto m = manifests.find(path);
                std::string error;
                if (m == manifests.end() || !JoinParts(shared, w.repoRoot, m->second, error)) {
                    failures += "\n  " + (error.empty() ? path + ": its parts are missing" : error);
                    continue;
                }
                SplitStates states = ReadSplitStates(w.gitDir);
                SplitState now;
                StatForState(w.absolute(path), now);
                now.hash = m->second.hash;
                states[path] = now;
                WriteSplitStates(w.gitDir, states);
            }
        }
        Publish(w, base, out);
        if (!failures.empty()) return Fail(out, "Some conflicts could not be resolved.", failures);
        Succeed(out, std::string(keepMine ? "Kept your version of " : "Took the server's version of ") +
                         CountLabel(paths.size(), "file") + ".");
    });
}

void VersionControl::createBranch(const std::string& name, bool switchTo) {
    enqueue(VcJobKind::CreateBranch, true, [name, switchTo](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        if (!w.git({"check-ref-format", "--branch", name}).ok()) {
            return Fail(out, "\"" + name + "\" is not a valid branch name.");
        }
        const GitRun run = switchTo ? w.git({"switch", "-c", name}) : w.git({"branch", name});
        Publish(w, base, out);
        if (!run.ok()) return Fail(out, Explain(run, "Creating the branch"), Detail(run));
        Succeed(out, switchTo ? "Created and switched to " + name + "." : "Created branch " + name + ".");
    });
}

void VersionControl::switchBranch(const std::string& name, bool remote) {
    enqueue(VcJobKind::SwitchBranch, true, [name, remote](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        const std::string before = w.head();
        std::string localName = name;
        GitRun run;
        if (remote) {
            const size_t slash = name.find('/');
            localName = slash == std::string::npos ? name : name.substr(slash + 1);
            const bool localExists = w.git({"rev-parse", "-q", "--verify", "refs/heads/" + localName}).ok();
            run = localExists ? w.git({"switch", localName}) : w.git({"switch", "--track", name});
        } else {
            run = w.git({"switch", name});
        }
        out.result.changedPaths = w.changedBetween(before, w.head());
        const std::string splitErrors = SyncSplitAfter(w, out);
        Publish(w, base, out);
        if (!run.ok()) return Fail(out, Explain(run, "Switching branch"), Detail(run));
        Succeed(out, "Switched to " + localName + ".");
        ReportSplitErrors(out, splitErrors);
    });
}

void VersionControl::deleteBranch(const std::string& name) {
    enqueue(VcJobKind::DeleteBranch, true, [name](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        const GitRun run = w.git({"branch", "-d", name});
        Publish(w, base, out);
        if (!run.ok()) {
            if (Contains(run.err, "not fully merged")) {
                return Fail(out, name + " has check-ins that are on no other branch, so it was kept.");
            }
            return Fail(out, Explain(run, "Deleting the branch"), Detail(run));
        }
        Succeed(out, "Deleted branch " + name + ".");
    });
}

void VersionControl::ignore(const std::string& pattern) {
    enqueue(VcJobKind::Ignore, true, [pattern](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        const fs::path file = root / ".gitignore";
        std::string existing = ReadTextFile(file);
        std::string addition;
        if (!existing.empty() && existing.back() != '\n') addition += "\n";
        addition += pattern + "\n";
        if (!WriteTextFile(file, addition, true)) return Fail(out, "Could not write " + Utf8Generic(file) + ".");
        Publish(w, base, out);
        Succeed(out, "Ignoring " + pattern + ".");
    });
}

void VersionControl::loadCommitFiles(const std::string& hash) {
    if (commitFileCache.count(hash)) return;
    for (const Job& job : queue) {
        if (job.kind == VcJobKind::CommitFiles) return;
    }
    if (runningActive && runningJob.kind == VcJobKind::CommitFiles) return;
    enqueue(VcJobKind::CommitFiles, false, [hash](const fs::path& root, const VcSnapshot&, VcShared& shared, VcJobOutput& out) {
        out.commitHash = hash;
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "Not a repository.");
        // Against the first parent, so a merge lists what it brought in; a root
        // commit has no parent and falls back to --root.
        GitRun run = w.git({"diff-tree", "-r", "-z", "--name-status", "--no-commit-id", hash + "^", hash});
        if (!run.ok()) run = w.git({"diff-tree", "-r", "-z", "--name-status", "--no-commit-id", "--root", hash});
        if (!run.ok()) return Fail(out, Explain(run, "Loading the changeset"));
        out.commitFiles = ParseNameStatus(run.out);
        out.result.ok = true;
    });
}

void VersionControl::clone(const std::string& url, const fs::path& destination) {
    enqueue(VcJobKind::Clone, true, [url, destination](const fs::path&, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        std::error_code ec;
        if (fs::exists(destination, ec) && !fs::is_empty(destination, ec)) {
            return Fail(out, "\"" + Utf8Generic(destination) + "\" already exists and is not empty. Pick another folder.");
        }
        const fs::path parent = destination.parent_path();
        fs::create_directories(parent, ec);
        const GitRun run = RunGit(&shared, parent, {"clone", "--progress", "--recurse-submodules", url, Utf8Generic(destination)}, true, true);
        if (!run.ok()) return Fail(out, Explain(run, "Downloading the project"), Detail(run));

        // A server whose HEAD names a branch that was never pushed (a bare
        // repository created as "master" that only ever received "main") clones
        // to an empty folder. Check out a real branch instead.
        if (!RunGit(&shared, destination, {"rev-parse", "-q", "--verify", "HEAD"}, false, true).ok()) {
            const GitRun refs = RunGit(&shared, destination, {"for-each-ref", "--format=%(refname:short)", "refs/remotes/origin"}, false, true);
            std::vector<std::string> branches;
            for (const std::string& ref : SplitLines(refs.out)) {
                if (ref != "origin/HEAD" && ref != "origin") branches.push_back(ref);
            }
            std::string pick;
            for (const char* preferred : {"origin/main", "origin/master"}) {
                if (pick.empty() && std::find(branches.begin(), branches.end(), preferred) != branches.end()) pick = preferred;
            }
            if (pick.empty() && !branches.empty()) pick = branches.front();
            if (!pick.empty()) RunGit(&shared, destination, {"switch", "--track", pick}, true, true);
        }

        fs::path projectFile = destination / "project.modu";
        if (!fs::exists(projectFile, ec)) {
            projectFile.clear();
            for (const auto& entry : fs::directory_iterator(destination, fs::directory_options::skip_permission_denied, ec)) {
                if (entry.is_directory(ec) && fs::exists(entry.path() / "project.modu", ec)) {
                    projectFile = entry.path() / "project.modu";
                    break;
                }
            }
        }
        out.result.clonedProjectFile = projectFile;
        out.result.repoRoot = destination;

        // Files over the host's limit arrive as parts; put them back together.
        Worker cloned(shared, destination);
        std::string splitErrors;
        if (cloned.locate() && fs::is_directory(destination / kSplitDir, ec)) {
            const size_t rebuilt = SyncSplitFiles(cloned, splitErrors).size();
            if (rebuilt > 0) shared.setProgress("Rebuilt " + CountLabel(rebuilt, "large file"));
        }
        if (!splitErrors.empty()) {
            return Fail(out, "Downloaded the project, but some large files could not be rebuilt from their parts.",
                        "Not rebuilt:" + splitErrors);
        }
        std::string note;
        if (!base.lfsAvailable && Contains(ReadTextFile(destination / ".gitattributes"), "filter=lfs")) {
            note = " This project stores large files with Git LFS, which is not installed, so textures and models "
                   "are placeholders. Install Git LFS, then run \"git lfs pull\" in the project folder.";
        }
        if (projectFile.empty()) {
            return Succeed(out, "Downloaded to " + Utf8Generic(destination) + ", but it has no project.modu to open." + note);
        }
        Succeed(out, "Downloaded the project to " + Utf8Generic(destination) + "." + note);
    });
}

void VersionControl::disableLfs() {
    enqueue(VcJobKind::DisableLfs, true, [](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        if (!base.lfsAvailable) {
            return Fail(out, "Git LFS is not installed, so the files it stores can't be read to convert them.");
        }
        // `git lfs migrate` silently throws away edits to tracked files (checked
        // against git-lfs 3.7), so it only runs on a clean tree.
        VcSnapshot status;
        const GitRun statusRun = w.status();
        if (!statusRun.ok()) return Fail(out, Explain(statusRun, "Reading the project"), Detail(statusRun));
        ParseStatus(statusRun.out, status);
        for (const VcChange& change : status.changes) {
            if (change.kind != VcChangeKind::Untracked) {
                return Fail(out, "Check in or undo your pending changes first (Check In Without Uploading is fine). "
                                 "Turning LFS off rewrites check-ins, and Git would drop unsaved edits while doing it.");
            }
        }

        // A file over the host's limit can't become an ordinary file in history.
        const SplitPolicy policy = SplitPolicyFor(base.remoteUrl);
        if (policy.enabled()) {
            std::string tooBig;
            for (const std::string& path : SplitLines(w.git({"lfs", "ls-files", "--name-only"}).out)) {
                std::error_code ec;
                const std::uintmax_t size = fs::file_size(w.absolute(Trim(path)), ec);
                if (!ec && size > policy.limit) tooBig += "\n  " + Trim(path) + " (" + MegabyteLabel(size) + ")";
            }
            if (!tooBig.empty()) {
                return Fail(out, "Some LFS files are over the server's " + MegabyteLabel(policy.limit) + " limit, so "
                                 "they have to stay in LFS. Nothing was changed.",
                            "Over the limit:" + tooBig);
            }
        }

        // Only check-ins that aren't uploaded yet are rewritten; uploaded ones
        // keep their LFS pointers, and new check-ins hold the real files.
        const std::string unpushed = Trim(w.git({"rev-list", "--count", "HEAD", "--not", "--remotes"}).out);
        const int rewritten = w.head().empty() ? 0 : std::atoi(unpushed.c_str());
        if (rewritten > 0) {
            shared.setProgress("Converting " + CountLabel(static_cast<size_t>(rewritten), "check-in"));
            const GitRun migrate = w.git({"lfs", "migrate", "export", "--include=*", "--yes"}, true);
            if (!migrate.ok()) {
                Publish(w, base, out);
                return Fail(out, Explain(migrate, "Converting the LFS files"), Detail(migrate));
            }
        }

        // Drop the LFS rules, and the "* !text !filter !merge !diff" line migrate
        // appends, which would also cancel "* -text".
        const fs::path attributesFile = w.repoRoot / ".gitattributes";
        std::string kept;
        for (const std::string& raw : Split(ReadTextFile(attributesFile), '\n')) {
            std::string line = raw;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (Contains(line, "filter=lfs") || Trim(line) == "* !text !filter !merge !diff" ||
                StartsWith(line, "# Large binary assets live in Git LFS")) {
                continue;
            }
            kept += line + "\n";
        }
        while (kept.size() >= 2 && kept.compare(kept.size() - 2, 2, "\n\n") == 0) kept.pop_back();
        std::error_code ec;
        if (Trim(kept).empty()) fs::remove(attributesFile, ec);
        else WriteTextFile(attributesFile, kept);
        if (rewritten > 0) {
            // The rewritten tip is not uploaded, so the cleaned rules can go into it.
            w.git({"add", "-A", "--", ".gitattributes"});
            w.git({"commit", "--amend", "--no-edit", "--only", "--", ".gitattributes"});
        }
        w.git({"lfs", "uninstall", "--local"});

        Publish(w, base, out);
        if (rewritten > 0) {
            return Succeed(out, "Large files are now stored directly in Git, and " +
                                CountLabel(static_cast<size_t>(rewritten), "check-in") +
                                " that weren't uploaded were converted. Upload when ready.");
        }
        Succeed(out, "Large files are now stored directly in Git. The converted files show up in Pending Changes; "
                     "check them in and upload.");
    });
}

void VersionControl::joinSplitFiles() {
    if (hasQueued(VcJobKind::JoinSplit) || (runningActive && runningJob.kind == VcJobKind::JoinSplit)) return;
    enqueue(VcJobKind::JoinSplit, false, [](const fs::path& root, const VcSnapshot& base, VcShared& shared, VcJobOutput& out) {
        Worker w(shared, root);
        if (!w.locate()) return Fail(out, "This project is not under version control yet.");
        const std::string errors = SyncSplitAfter(w, out);
        Publish(w, base, out);
        const size_t rebuilt = CountProjectFiles(out.result.changedPaths);
        Succeed(out, rebuilt == 0 ? std::string()
                                  : "Rebuilt " + CountLabel(rebuilt, "large file") + " from the parts it was uploaded in.");
        ReportSplitErrors(out, errors);
    });
}

#pragma endregion

#endif // !MODULARITY_RUNTIME_ONLY
