#include "CrashReporter.h"
#ifdef __ANDROID__
#include <android/log.h>
namespace Modularity::CrashReporter {
    bool HandleCrashReporterMode(int /*argc*/, char** /*argv*/) {return false;}
    void Initialize(const std::string& /*productName*/, const std::string& /*executablePath*/) {// TODO: register a signal handler that writes a tombstone to logcat before re-raising.}
    int RunProtected(const std::function<int()>& entryPoint) {return entryPoint ? entryPoint() : 0;}
    void AppendLogLine(const std::string& line) {__android_log_print(ANDROID_LOG_INFO, "Modularity", "%s", line.c_str());}
}
#else
#include "../include/Graphics/OpenGL.h"
#include "AudioSystem.h"
#include "ThirdParty/glfw/include/GLFW/glfw3.h"
#include "ThirdParty/ModuGUI/backends/imgui_impl_glfw.h"
#include "ThirdParty/ModuGUI/backends/imgui_impl_opengl3.h"
#include "ThirdParty/ModuGUI/imgui.h"
#include "../include/ThirdParty/stb_image.h"
#include "../include/Textures/StbFlipScope.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace Modularity::CrashReporter {
    namespace {
        namespace fs = std::filesystem;
        struct CrashContext {
            std::string productName = "Modularity";
            fs::path executablePath;
            fs::path sessionLogPath;
            std::mutex logMutex;
            std::ofstream logFile;
            std::streambuf* oldCout = nullptr;
            std::streambuf* oldCerr = nullptr;
            std::atomic<bool> crashHandled{false};
        };
        CrashContext& context() {static CrashContext ctx; return ctx;}
        #if defined(__linux__) || defined(__APPLE__)
        constexpr size_t kSignalPathBufferSize = 1024;
        char gSessionLogPathForSignal[kSignalPathBufferSize] = {};
        char gSignalCrashLogPath[kSignalPathBufferSize] = {};
        char gCrashSummaryPathForSignal[kSignalPathBufferSize] = {};
        volatile sig_atomic_t gSignalLoggingReady = 0;
        #endif
        fs::path executableDirectory() {
            auto& ctx = context();
            if (!ctx.executablePath.empty()) {
                std::error_code ec;
                const fs::path absolute = fs::absolute(ctx.executablePath, ec);
                if (!ec && !absolute.empty()) {return absolute.parent_path();}
                const fs::path rawPath(ctx.executablePath);
                if (rawPath.has_parent_path()) {return rawPath.parent_path();}
            }   return {};
        }
        std::string nowForFileName() {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tmNow{};
        #if defined(_WIN32)
            localtime_s(&tmNow, &time);
        #else
            localtime_r(&time, &tmNow);
        #endif
            char buffer[32] = {};
            std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tmNow);
            return buffer;
        }
        std::string nowForDisplay() {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tmNow{};
        #if defined(_WIN32)
            localtime_s(&tmNow, &time);
        #else
            localtime_r(&time, &tmNow);
        #endif
            char buffer[64] = {};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmNow);
            return buffer;
        }
        size_t signalSafeStrLen(const char* text) {if (!text) return 0; size_t n = 0; while (text[n] != '\0') ++n; return n;}
        char* appendUnsignedDec(char* out, unsigned long long value) {
            char tmp[32]; int idx = 0;
            do {tmp[idx++] = static_cast<char>('0' + (value % 10ULL)); value /= 10ULL;}
            while (value && idx < static_cast<int>(sizeof(tmp)));
            while (idx > 0) {*out++ = tmp[--idx];}
            return out;
        }
        char* appendSignedDec(char* out, long long value) {
            if (value < 0) {
                *out++ = '-';
                const unsigned long long magnitude = static_cast<unsigned long long>(-(value + 1)) + 1ULL;
                return appendUnsignedDec(out, magnitude);
            }
            return appendUnsignedDec(out, static_cast<unsigned long long>(value));
        }
        char* appendHex(char* out, uintptr_t value) {
            static constexpr char kHex[] = "0123456789abcdef";
            *out++ = '0';
            *out++ = 'x';
            bool started = false;
            for (int shift = static_cast<int>(sizeof(uintptr_t) * 8) - 4; shift >= 0; shift -= 4) {
                const unsigned nibble = static_cast<unsigned>((value >> shift) & 0xF);
                if (!started && nibble == 0 && shift > 0) continue;
                started = true;
                *out++ = kHex[nibble];
            }
            if (!started) {*out++ = '0';}
            return out;
        }
        #if defined(_WIN32)
        constexpr size_t kWinPathBufferSize = 1024;
        wchar_t gWinCrashLogPath[kWinPathBufferSize] = {};
        wchar_t gWinMiniDumpPath[kWinPathBufferSize] = {};
        volatile LONG gWinReportEntered = 0;

        struct CrashSink {
            HANDLE files[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
            int count = 0;
            void add(HANDLE handle) {
                if (handle != INVALID_HANDLE_VALUE && handle != nullptr && count < 3) {files[count++] = handle;}
            }
        };
        void sinkWrite(CrashSink& sink, const char* text, size_t length) {
            if (!text || length == 0) return;
            for (int i = 0; i < sink.count; ++i) {
                DWORD written = 0;
                WriteFile(sink.files[i], text, static_cast<DWORD>(length), &written, nullptr);
            }
        }
        void sinkWrite(CrashSink& sink, const char* text) {sinkWrite(sink, text, signalSafeStrLen(text));}
        void sinkWriteLine(CrashSink& sink, const char* text) {sinkWrite(sink, text); sinkWrite(sink, "\r\n");}
        void sinkWriteKeyText(CrashSink& sink, const char* key, const char* value) {
            sinkWrite(sink, key);
            sinkWrite(sink, "=");
            sinkWrite(sink, value);
            sinkWrite(sink, "\r\n");
        }
        void sinkWriteKeyHex(CrashSink& sink, const char* key, uintptr_t value) {
            char line[192];
            char* ptr = line;
            for (const char* p = key; *p && ptr < line + 128; ++p) *ptr++ = *p;
            *ptr++ = '=';
            ptr = appendHex(ptr, value);
            *ptr++ = '\r'; *ptr++ = '\n';
            sinkWrite(sink, line, static_cast<size_t>(ptr - line));
        }
        void sinkWriteKeyDec(CrashSink& sink, const char* key, long long value) {
            char line[192];
            char* ptr = line;
            for (const char* p = key; *p && ptr < line + 128; ++p) *ptr++ = *p;
            *ptr++ = '=';
            ptr = appendSignedDec(ptr, value);
            *ptr++ = '\r'; *ptr++ = '\n';
            sinkWrite(sink, line, static_cast<size_t>(ptr - line));
        }
        void storePathForCrash(const fs::path& path, wchar_t* outBuffer, size_t outBufferCount) {
            if (!outBuffer || outBufferCount == 0) return;
            std::wmemset(outBuffer, 0, outBufferCount);
            const std::wstring value = path.wstring();
            const size_t copyLen = value.size() < (outBufferCount - 1) ? value.size() : (outBufferCount - 1);
            std::wmemcpy(outBuffer, value.data(), copyLen);
            outBuffer[copyLen] = L'\0';
        }
        const char* exceptionCodeName(DWORD code) {
            switch (code) {
                case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION - dereferenced a bad pointer";
                case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
                case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
                case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
                case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION - executed non-code, e.g. called through a stale function pointer";
                case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR - a mapped file page could not be read (unloaded/replaced module?)";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
                case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION - executed non-code";
                case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW - unbounded recursion";
                case EXCEPTION_BREAKPOINT:            return "EXCEPTION_BREAKPOINT - a debug break with no debugger attached";
                case EXCEPTION_GUARD_PAGE:            return "EXCEPTION_GUARD_PAGE";
                case 0xC0000374u:                     return "STATUS_HEAP_CORRUPTION - double free, buffer overrun, or freeing a pointer from another heap";
                case 0xC000041Du:                     return "STATUS_FATAL_USER_CALLBACK_EXCEPTION - a crash inside a window/callback the OS invoked";
                case 0xC0000409u:                     return "STATUS_STACK_BUFFER_OVERRUN - /GS stack cookie check failed";
                case 0xE06D7363u:                     return "MSVC C++ exception - an unhandled throw";
                default:                              return "Unrecognised structured exception";
            }
        }
        bool describeAddress(uintptr_t address, char* nameBuffer, DWORD nameBufferSize, uintptr_t& offsetOut) {
            HMODULE module = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(address), &module) || module == nullptr) {
                return false;
            }
            if (GetModuleFileNameA(module, nameBuffer, nameBufferSize) == 0) return false;
            offsetOut = address - reinterpret_cast<uintptr_t>(module);
            return true;
        }
        void writeAddressLine(CrashSink& sink, const char* key, uintptr_t address) {
            static char moduleName[MAX_PATH * 2];
            sinkWriteKeyHex(sink, key, address);
            uintptr_t offset = 0;
            std::memset(moduleName, 0, sizeof(moduleName));
            if (describeAddress(address, moduleName, static_cast<DWORD>(sizeof(moduleName)), offset)) {
                char line[64];
                char* ptr = line;
                ptr = appendHex(ptr, offset);
                *ptr = '\0';
                sinkWrite(sink, key);
                sinkWrite(sink, "_module=");
                sinkWrite(sink, moduleName);
                sinkWrite(sink, "+");
                sinkWrite(sink, line);
                sinkWrite(sink, "\r\n");
            } else {
                sinkWrite(sink, key);
                sinkWriteLine(sink, "_module=<none - this address is not inside any loaded image>");
            }
        }
        void describeFaultMemory(CrashSink& sink, uintptr_t address) {
            MEMORY_BASIC_INFORMATION info;
            std::memset(&info, 0, sizeof(info));
            if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info)) {
                sinkWriteLine(sink, "fault_memory=<VirtualQuery failed - address is wildly out of range>");
                return;
            }
            switch (info.State) {
                case MEM_FREE:    sinkWriteLine(sink, "fault_memory=MEM_FREE - nothing is mapped here"); break;
                case MEM_RESERVE: sinkWriteLine(sink, "fault_memory=MEM_RESERVE - reserved but not committed"); break;
                case MEM_COMMIT:  sinkWriteLine(sink, "fault_memory=MEM_COMMIT - mapped, so the access rights were wrong rather than the address"); break;
                default:          sinkWriteKeyHex(sink, "fault_memory_state", info.State); break;
            }
            switch (info.Type) {
                case MEM_IMAGE:   sinkWriteLine(sink, "fault_region=MEM_IMAGE - inside a loaded module"); break;
                case MEM_MAPPED:  sinkWriteLine(sink, "fault_region=MEM_MAPPED - a file mapping"); break;
                case MEM_PRIVATE: sinkWriteLine(sink, "fault_region=MEM_PRIVATE - heap or stack"); break;
                default: break;
            }
            sinkWriteKeyHex(sink, "fault_region_base", reinterpret_cast<uintptr_t>(info.AllocationBase));
            sinkWriteKeyHex(sink, "fault_region_protect", info.Protect);
            if (address < 0x10000) {
                sinkWriteLine(sink, "interpretation=NULL (or near-null) DEREFERENCE. The address is a small "
                                    "offset, i.e. a field read off a null object pointer.");
            } else if (info.State == MEM_FREE) {
                sinkWriteLine(sink, "interpretation=DANGLING POINTER. This address was valid earlier and has since "
                                    "been released. If it once pointed into a script DLL, that DLL was unloaded "
                                    "(FreeLibrary) while something still held a pointer into it.");
        #if defined(_WIN64)
            } else if ((address >> 47) != 0) {
                sinkWriteLine(sink, "interpretation=CORRUPT POINTER. The value is not a canonical user-mode "
                                    "address, so the pointer itself was overwritten with garbage.");
        #endif
            } else {
                sinkWriteLine(sink, "interpretation=The page is mapped but the access was not permitted "
                                    "(wrote to read-only memory, or executed data).");
            }
        }
        void writeRegisters(CrashSink& sink, const CONTEXT* context) {
            if (!context) return;
            sinkWriteLine(sink, "-- registers --");
        #if defined(_M_X64)
            sinkWriteKeyHex(sink, "rip", static_cast<uintptr_t>(context->Rip));
            sinkWriteKeyHex(sink, "rsp", static_cast<uintptr_t>(context->Rsp));
            sinkWriteKeyHex(sink, "rbp", static_cast<uintptr_t>(context->Rbp));
            sinkWriteKeyHex(sink, "rax", static_cast<uintptr_t>(context->Rax));
            sinkWriteKeyHex(sink, "rbx", static_cast<uintptr_t>(context->Rbx));
            sinkWriteKeyHex(sink, "rcx", static_cast<uintptr_t>(context->Rcx));
            sinkWriteKeyHex(sink, "rdx", static_cast<uintptr_t>(context->Rdx));
            sinkWriteKeyHex(sink, "rsi", static_cast<uintptr_t>(context->Rsi));
            sinkWriteKeyHex(sink, "rdi", static_cast<uintptr_t>(context->Rdi));
            sinkWriteKeyHex(sink, "r8",  static_cast<uintptr_t>(context->R8));
            sinkWriteKeyHex(sink, "r9",  static_cast<uintptr_t>(context->R9));
            sinkWriteKeyHex(sink, "r10", static_cast<uintptr_t>(context->R10));
            sinkWriteKeyHex(sink, "r11", static_cast<uintptr_t>(context->R11));
            sinkWriteKeyHex(sink, "r12", static_cast<uintptr_t>(context->R12));
            sinkWriteKeyHex(sink, "r13", static_cast<uintptr_t>(context->R13));
            sinkWriteKeyHex(sink, "r14", static_cast<uintptr_t>(context->R14));
            sinkWriteKeyHex(sink, "r15", static_cast<uintptr_t>(context->R15));
        #elif defined(_M_IX86)
            sinkWriteKeyHex(sink, "eip", static_cast<uintptr_t>(context->Eip));
            sinkWriteKeyHex(sink, "esp", static_cast<uintptr_t>(context->Esp));
            sinkWriteKeyHex(sink, "ebp", static_cast<uintptr_t>(context->Ebp));
            sinkWriteKeyHex(sink, "eax", static_cast<uintptr_t>(context->Eax));
            sinkWriteKeyHex(sink, "ebx", static_cast<uintptr_t>(context->Ebx));
            sinkWriteKeyHex(sink, "ecx", static_cast<uintptr_t>(context->Ecx));
            sinkWriteKeyHex(sink, "edx", static_cast<uintptr_t>(context->Edx));
            sinkWriteKeyHex(sink, "esi", static_cast<uintptr_t>(context->Esi));
            sinkWriteKeyHex(sink, "edi", static_cast<uintptr_t>(context->Edi));
        #elif defined(_M_ARM64)
            sinkWriteKeyHex(sink, "pc", static_cast<uintptr_t>(context->Pc));
            sinkWriteKeyHex(sink, "sp", static_cast<uintptr_t>(context->Sp));
            sinkWriteKeyHex(sink, "fp", static_cast<uintptr_t>(context->Fp));
            sinkWriteKeyHex(sink, "lr", static_cast<uintptr_t>(context->Lr));
        #else
            (void)context;
        #endif
        }
        void writeFrameLine(CrashSink& sink, int index, uintptr_t pc, bool symbolsReady) {
            static char moduleName[MAX_PATH * 2];
            static char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
            char line[64];
            char* ptr = line;
            *ptr++ = '#';
            ptr = appendUnsignedDec(ptr, static_cast<unsigned long long>(index));
            *ptr++ = ' ';
            *ptr = '\0';
            sinkWrite(sink, line);
            ptr = appendHex(line, pc);
            *ptr = '\0';
            sinkWrite(sink, line);
            uintptr_t moduleOffset = 0;
            std::memset(moduleName, 0, sizeof(moduleName));
            if (describeAddress(pc, moduleName, static_cast<DWORD>(sizeof(moduleName)), moduleOffset)) {
                sinkWrite(sink, " ");
                sinkWrite(sink, moduleName);
                sinkWrite(sink, "+");
                ptr = appendHex(line, moduleOffset);
                *ptr = '\0';
                sinkWrite(sink, line);
            }
            if (symbolsReady) {
                const HANDLE process = GetCurrentProcess();
                std::memset(symbolStorage, 0, sizeof(symbolStorage));
                SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = MAX_SYM_NAME;
                DWORD64 displacement = 0;
                if (SymFromAddr(process, pc, &displacement, symbol)) {
                    sinkWrite(sink, " ");
                    sinkWrite(sink, symbol->Name);
                }
                IMAGEHLP_LINE64 sourceLine;
                std::memset(&sourceLine, 0, sizeof(sourceLine));
                sourceLine.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(process, pc, &lineDisplacement, &sourceLine)) {
                    sinkWrite(sink, " (");
                    sinkWrite(sink, sourceLine.FileName);
                    sinkWrite(sink, ":");
                    ptr = appendUnsignedDec(line, static_cast<unsigned long long>(sourceLine.LineNumber));
                    *ptr = '\0';
                    sinkWrite(sink, line);
                    sinkWrite(sink, ")");
                }
            }
            sinkWrite(sink, "\r\n");
        }
        constexpr int kMaxThrowFrames = 48;
        void* gLastThrowFrames[kMaxThrowFrames] = {};
        volatile LONG gLastThrowFrameCount = 0;
        LONG CALLBACK firstChanceThrowRecorder(EXCEPTION_POINTERS* exceptionInfo) {
            if (exceptionInfo && exceptionInfo->ExceptionRecord &&
                exceptionInfo->ExceptionRecord->ExceptionCode == 0xE06D7363u) {
                gLastThrowFrameCount = static_cast<LONG>(
                    RtlCaptureStackBackTrace(1, kMaxThrowFrames, gLastThrowFrames, nullptr));
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        void writeStackTrace(CrashSink& sink, const CONTEXT* context) {
            sinkWriteLine(sink, "-- stack (innermost first) --");
            if (!context) {sinkWriteLine(sink, "<no context record>"); return;}
            const HANDLE process = GetCurrentProcess();
            const HANDLE thread = GetCurrentThread();
            SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
            const BOOL symbolsReady = SymInitialize(process, nullptr, TRUE);
            static CONTEXT walkContext;
            std::memcpy(&walkContext, context, sizeof(CONTEXT));
            STACKFRAME64 frame;
            std::memset(&frame, 0, sizeof(frame));
            DWORD machineType = 0;
        #if defined(_M_X64)
            machineType = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = walkContext.Rip;
            frame.AddrFrame.Offset = walkContext.Rbp;
            frame.AddrStack.Offset = walkContext.Rsp;
        #elif defined(_M_IX86)
            machineType = IMAGE_FILE_MACHINE_I386;
            frame.AddrPC.Offset = walkContext.Eip;
            frame.AddrFrame.Offset = walkContext.Ebp;
            frame.AddrStack.Offset = walkContext.Esp;
        #elif defined(_M_ARM64)
            machineType = IMAGE_FILE_MACHINE_ARM64;
            frame.AddrPC.Offset = walkContext.Pc;
            frame.AddrFrame.Offset = walkContext.Fp;
            frame.AddrStack.Offset = walkContext.Sp;
        #endif
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Mode = AddrModeFlat;
            if (machineType == 0) {sinkWriteLine(sink, "<unsupported architecture for StackWalk64>"); return;}
            for (int depth = 0; depth < 64; ++depth) {
                if (!StackWalk64(machineType, process, thread, &frame, &walkContext, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                    break;
                }
                if (frame.AddrPC.Offset == 0) break;
                writeFrameLine(sink, depth, static_cast<uintptr_t>(frame.AddrPC.Offset), symbolsReady != FALSE);
            }
            if (symbolsReady) {SymCleanup(process);}
        }
        void writeThrowSiteReport(const char* reason, const char* what) {
            if (InterlockedExchange(&gWinReportEntered, 1) != 0) return;
            CrashSink sink;
            if (gWinCrashLogPath[0]) {
                sink.add(CreateFileW(gWinCrashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            }
            sink.add(GetStdHandle(STD_ERROR_HANDLE));
            sinkWriteLine(sink, "");
            sinkWriteLine(sink, "========= Modularity crash report (Windows, unhandled C++ exception) =========");
            sinkWriteKeyText(sink, "reason", reason ? reason : "");
            sinkWriteKeyText(sink, "what", what ? what : "");
            sinkWriteKeyDec(sink, "thread_id", static_cast<long long>(GetCurrentThreadId()));
            sinkWriteLine(sink, "-- stack at the throw (innermost first) --");
            const LONG count = gLastThrowFrameCount;
            if (count <= 0) {
                sinkWriteLine(sink, "<no throw was recorded - it may predate CrashReporter::Initialize>");
            } else {
                const HANDLE process = GetCurrentProcess();
                SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
                const BOOL symbolsReady = SymInitialize(process, nullptr, TRUE);
                for (LONG i = 0; i < count && i < kMaxThrowFrames; ++i) {
                    writeFrameLine(sink, static_cast<int>(i), reinterpret_cast<uintptr_t>(gLastThrowFrames[i]),
                                   symbolsReady != FALSE);
                }
                if (symbolsReady) {SymCleanup(process);}
            }
            sinkWriteLine(sink, "========= end crash report =========");
            for (int i = 0; i < sink.count; ++i) {
                if (sink.files[i] != GetStdHandle(STD_ERROR_HANDLE)) CloseHandle(sink.files[i]);
            }
        }
        void writeLoadedModules(CrashSink& sink) {
            sinkWriteLine(sink, "-- loaded modules --");
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
            if (snapshot == INVALID_HANDLE_VALUE) {sinkWriteLine(sink, "<snapshot unavailable>"); return;}
            static MODULEENTRY32 entry;
            std::memset(&entry, 0, sizeof(entry));
            entry.dwSize = sizeof(entry);
            if (Module32First(snapshot, &entry)) {
                do {
                    char line[64];
                    char* ptr = appendHex(line, reinterpret_cast<uintptr_t>(entry.modBaseAddr));
                    *ptr = '\0';
                    sinkWrite(sink, line);
                    sinkWrite(sink, " +");
                    ptr = appendHex(line, static_cast<uintptr_t>(entry.modBaseSize));
                    *ptr = '\0';
                    sinkWrite(sink, line);
                    sinkWrite(sink, " ");
                #if defined(UNICODE)
                    static char narrowPath[MAX_PATH * 2];
                    if (WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, narrowPath, sizeof(narrowPath), nullptr, nullptr) > 0) {
                        sinkWrite(sink, narrowPath);
                    }
                #else
                    sinkWrite(sink, entry.szExePath);
                #endif
                    sinkWrite(sink, "\r\n");
                } while (Module32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        void writeMiniDump(EXCEPTION_POINTERS* exceptionInfo) {
            if (!gWinMiniDumpPath[0]) return;
            const HANDLE dumpFile = CreateFileW(gWinMiniDumpPath, GENERIC_WRITE, 0, nullptr,
                                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (dumpFile == INVALID_HANDLE_VALUE) return;
            MINIDUMP_EXCEPTION_INFORMATION exceptionParam;
            std::memset(&exceptionParam, 0, sizeof(exceptionParam));
            exceptionParam.ThreadId = GetCurrentThreadId();
            exceptionParam.ExceptionPointers = exceptionInfo;
            exceptionParam.ClientPointers = FALSE;
            const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo);
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile, dumpType,
                              exceptionInfo ? &exceptionParam : nullptr, nullptr, nullptr);
            CloseHandle(dumpFile);
        }
        void emitCrashReportBody(CrashSink& sink, EXCEPTION_POINTERS* exceptionInfo) {
            const EXCEPTION_RECORD* record = exceptionInfo ? exceptionInfo->ExceptionRecord : nullptr;
            const DWORD code = record ? record->ExceptionCode : 0;
            sinkWriteLine(sink, "");
            sinkWriteLine(sink, "================ Modularity crash report (Windows) ================");
            sinkWriteKeyHex(sink, "exception_code", code);
            sinkWriteKeyText(sink, "exception_name", exceptionCodeName(code));
            sinkWriteKeyDec(sink, "thread_id", static_cast<long long>(GetCurrentThreadId()));
            sinkWriteKeyDec(sink, "process_id", static_cast<long long>(GetCurrentProcessId()));
            if (record) {
                writeAddressLine(sink, "exception_address", reinterpret_cast<uintptr_t>(record->ExceptionAddress));
                if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
                    if (record->NumberParameters >= 2) {
                        const ULONG_PTR operation = record->ExceptionInformation[0];
                        const uintptr_t faultAddress = static_cast<uintptr_t>(record->ExceptionInformation[1]);
                        if (operation == 0)      sinkWriteLine(sink, "access_type=READ");
                        else if (operation == 1) sinkWriteLine(sink, "access_type=WRITE");
                        else if (operation == 8) sinkWriteLine(sink, "access_type=EXECUTE (DEP violation)");
                        else                     sinkWriteKeyHex(sink, "access_type_raw", operation);
                        writeAddressLine(sink, "fault_address", faultAddress);
                        describeFaultMemory(sink, faultAddress);
                    }
                }
            }
            writeRegisters(sink, exceptionInfo ? exceptionInfo->ContextRecord : nullptr);
            writeStackTrace(sink, exceptionInfo ? exceptionInfo->ContextRecord : nullptr);
            writeLoadedModules(sink);
            sinkWriteLine(sink, "================ end crash report ================");
        }
        void emitCrashReportGuarded(CrashSink& sink, EXCEPTION_POINTERS* exceptionInfo) {
            __try {
                emitCrashReportBody(sink, exceptionInfo);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                sinkWriteLine(sink, "<crash report generation faulted; the fields above are still valid>");
            }
        }
        void writeMiniDumpGuarded(EXCEPTION_POINTERS* exceptionInfo) {
            __try {
                writeMiniDump(exceptionInfo);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        void reportWindowsException(EXCEPTION_POINTERS* exceptionInfo, char* summary, size_t summarySize) {
            if (summary && summarySize > 0) summary[0] = '\0';
            if (InterlockedExchange(&gWinReportEntered, 1) != 0) return;
            CrashSink sink;
            if (gWinCrashLogPath[0]) {
                sink.add(CreateFileW(gWinCrashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            }
            sink.add(GetStdHandle(STD_ERROR_HANDLE));
            emitCrashReportGuarded(sink, exceptionInfo);
            writeMiniDumpGuarded(exceptionInfo);
            for (int i = 0; i < sink.count; ++i) {
                if (sink.files[i] != GetStdHandle(STD_ERROR_HANDLE)) CloseHandle(sink.files[i]);
            }
            if (summary && summarySize > 64) {
                const EXCEPTION_RECORD* record = exceptionInfo ? exceptionInfo->ExceptionRecord : nullptr;
                const DWORD code = record ? record->ExceptionCode : 0;
                char* ptr = summary;
                char* const limit = summary + summarySize - 1;
                const char* const name = exceptionCodeName(code);
                ptr = appendHex(ptr, code);
                *ptr++ = ' ';
                for (const char* p = name; *p && ptr < limit - 96; ++p) *ptr++ = *p;
                if (record) {
                    for (const char* p = "\nat "; *p; ++p) *ptr++ = *p;
                    ptr = appendHex(ptr, reinterpret_cast<uintptr_t>(record->ExceptionAddress));
                    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2) {
                        for (const char* p = record->ExceptionInformation[0] == 1 ? "\nwriting " : "\nreading "; *p; ++p) *ptr++ = *p;
                        ptr = appendHex(ptr, static_cast<uintptr_t>(record->ExceptionInformation[1]));
                    }
                }
                *ptr = '\0';
            }
        }
        #endif
        #if defined(__linux__) || defined(__APPLE__)
        void storePathForSignal(const fs::path& path, char* outBuffer, size_t outBufferSize) {
            if (!outBuffer || outBufferSize == 0) return;
            std::memset(outBuffer, 0, outBufferSize);
            const std::string value = path.string();
            const size_t copyLen = std::min(value.size(), outBufferSize - 1);
            std::memcpy(outBuffer, value.data(), copyLen);
            outBuffer[copyLen] = '\0';
        }
        void signalSafeWrite(int fd, const char* text) {
            if (fd < 0 || !text) return;
            const size_t len = signalSafeStrLen(text);
            if (len == 0) return;
            (void)!::write(fd, text, len);
        }
        void signalWriteKeyInt(int fd, const char* key, long long value) {
            char line[128];
            char* ptr = line;
            for (const char* p = key; *p; ++p) *ptr++ = *p;
            *ptr++ = '=';
            ptr = appendSignedDec(ptr, value);
            *ptr++ = '\n';
            (void)!::write(fd, line, static_cast<size_t>(ptr - line));
        }
        void signalWriteKeyHex(int fd, const char* key, uintptr_t value) {
            char line[128];
            char* ptr = line;
            for (const char* p = key; *p; ++p) *ptr++ = *p;
            *ptr++ = '=';
            ptr = appendHex(ptr, value);
            *ptr++ = '\n';
            (void)!::write(fd, line, static_cast<size_t>(ptr - line));
        }
        void writeSignalSummaryFile(int signalValue) {
            if (!gCrashSummaryPathForSignal[0]) return;
            const int fd = ::open(gCrashSummaryPathForSignal, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) return;
            signalSafeWrite(fd, "reason=Fatal signal\n");
            signalWriteKeyInt(fd, "signal", signalValue);
            if (gSignalCrashLogPath[0]) {
                signalSafeWrite(fd, "log=");
                signalSafeWrite(fd, gSignalCrashLogPath);
                signalSafeWrite(fd, "\n");
            } else if (gSessionLogPathForSignal[0]) {
                signalSafeWrite(fd, "log=");
                signalSafeWrite(fd, gSessionLogPathForSignal);
                signalSafeWrite(fd, "\n");
            }   (void)!::close(fd);
        }
        void writeSignalCrashLog(int signalValue, siginfo_t* info) {
            int fd = -1;
            if (gSignalCrashLogPath[0]) {fd = ::open(gSignalCrashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);}
            if (fd < 0 && gSessionLogPathForSignal[0]) {fd = ::open(gSessionLogPathForSignal, O_WRONLY | O_CREAT | O_APPEND, 0644);}
            if (fd < 0) return;
            signalSafeWrite(fd, "\n[CrashReporter] Fatal signal captured on POSIX.\n");
            signalWriteKeyInt(fd, "signal", signalValue);
            signalWriteKeyInt(fd, "pid", static_cast<long long>(::getpid()));
            if (info) {
                signalWriteKeyInt(fd, "si_code", static_cast<long long>(info->si_code));
                signalWriteKeyHex(fd, "si_addr", reinterpret_cast<uintptr_t>(info->si_addr));
            }
            signalSafeWrite(fd, "stacktrace_begin\n");
            void* frames[64];
            const int count = ::backtrace(frames, 64);
            if (count > 0) {::backtrace_symbols_fd(frames, count, fd);}
            signalSafeWrite(fd, "stacktrace_end\n");
            (void)!::close(fd);
        }
        #endif
        class TeeStreamBuf final : public std::streambuf {
            public:
                TeeStreamBuf(std::streambuf* primary, std::streambuf* secondary) : primary_(primary), secondary_(secondary) {}
            protected:
                int overflow(int ch) override {
                    if (ch == EOF) return !EOF;
                    const int first = primary_ ? primary_->sputc(static_cast<char>(ch)) : ch;
                    const int second = secondary_ ? secondary_->sputc(static_cast<char>(ch)) : ch;
                    return (first == EOF || second == EOF) ? EOF : ch;
                }
                int sync() override {
                    const int first = primary_ ? primary_->pubsync() : 0;
                    const int second = secondary_ ? secondary_->pubsync() : 0;
                    return (first == 0 && second == 0) ? 0 : -1;
                }
            private:
                std::streambuf* primary_ = nullptr;
                std::streambuf* secondary_ = nullptr;
        };
        TeeStreamBuf*& coutTee() {static TeeStreamBuf* tee = nullptr; return tee;}
        TeeStreamBuf*& cerrTee() {static TeeStreamBuf* tee = nullptr; return tee;}
        std::string quoteArg(const std::string& value) {
            std::string out = "\"";
            for (char c : value) {
                if (c == '"') out += "\\\"";
                else          out += c;
            }
            out += "\"";
            return out;
        }
        void launchUrl(const std::string& url) {
        #if defined(_WIN32)
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        #elif defined(__APPLE__)
            std::string command = "open " + quoteArg(url);
            std::system(command.c_str());
        #else
            std::string command = "xdg-open " + quoteArg(url) + " >/dev/null 2>&1 &";
            std::system(command.c_str());
        #endif
        }
        void openPath(const fs::path& path) {
        #if defined(_WIN32)
            ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        #elif defined(__APPLE__)
            std::string command = "open " + quoteArg(path.string());
            std::system(command.c_str());
        #else
            std::string command = "xdg-open " + quoteArg(path.string()) + " >/dev/null 2>&1 &";
            std::system(command.c_str());
        #endif
        }
        fs::path crashDirectory() {
            fs::path baseDir = executableDirectory();
            if (baseDir.empty()) {baseDir = fs::current_path();}
            const fs::path dir = baseDir / "CrashReports";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }
        void writeCrashSummary(const std::string& reason, const std::string& details) {
            auto& ctx = context();
            const fs::path summaryPath = crashDirectory() / "last_crash.txt";
            std::ofstream summary(summaryPath, std::ios::trunc);
            if (!summary.is_open()) return;
            summary << "product=" << ctx.productName << "\n";
            summary << "timestamp=" << nowForDisplay() << "\n";
            summary << "reason=" << reason << "\n";
            summary << "details=" << details << "\n";
            summary << "log=" << ctx.sessionLogPath.string() << "\n";
        }
        void launchReporterProcess(const std::string& reason, const std::string& details) {
            auto& ctx = context();
            writeCrashSummary(reason, details);
            if (ctx.executablePath.empty()) return;
        #if defined(_WIN32)
            std::string commandLine = quoteArg(ctx.executablePath.string()) + " --crash-reporter" + " --product-name " + quoteArg(ctx.productName) + " --crash-reason " + quoteArg(reason) + " --crash-details " + quoteArg(details) + " --crash-log " + quoteArg(ctx.sessionLogPath.string());
            STARTUPINFOA startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
            std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
            mutableCommand.push_back('\0');
            if (CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &startupInfo, &processInfo)) {
                CloseHandle(processInfo.hThread);
                CloseHandle(processInfo.hProcess);
            }
        #else
            const std::string command = quoteArg(ctx.executablePath.string()) + " --crash-reporter" + " --product-name " + quoteArg(ctx.productName) + " --crash-reason " + quoteArg(reason) + " --crash-details " + quoteArg(details) + " --crash-log " + quoteArg(ctx.sessionLogPath.string()) + " >/dev/null 2>&1 &";
            std::system(command.c_str());
        #endif
        }
        [[noreturn]] void handleCrash(const std::string& reason, const std::string& details, int exitCode) {
            auto& ctx = context();
            if (!ctx.crashHandled.exchange(true)) {
            #ifdef _WIN32
                CONTEXT captured;
                std::memset(&captured, 0, sizeof(captured));
                captured.ContextFlags = CONTEXT_FULL;
                RtlCaptureContext(&captured);
                EXCEPTION_RECORD synthetic;
                std::memset(&synthetic, 0, sizeof(synthetic));
                EXCEPTION_POINTERS pointers;
                pointers.ExceptionRecord = &synthetic;
                pointers.ContextRecord = &captured;
                static char handlerSummary[1024];
                reportWindowsException(&pointers, handlerSummary, sizeof(handlerSummary));
            #endif
                AppendLogLine("[CrashReporter] Fatal crash detected.");
                AppendLogLine("[CrashReporter] Reason: " + reason);
                if (!details.empty()) {AppendLogLine("[CrashReporter] Details: " + details);}
                launchReporterProcess(reason, details);
            }
            std::_Exit(exitCode);
        }
        #if defined(_WIN32)
        void signalHandler(int signalValue) {
            static char summary[1024];
            summary[0] = '\0';
        #if defined(_MSC_VER)
            EXCEPTION_POINTERS* exceptionInfo = static_cast<EXCEPTION_POINTERS*>(_pxcptinfoptrs);
            if (exceptionInfo != nullptr) {reportWindowsException(exceptionInfo, summary, sizeof(summary));}
        #endif
            std::ostringstream details;
            if (summary[0] != '\0') {details << summary << "\n(signal " << signalValue << ")";}
            else                    {details << "signal " << signalValue << " with no structured exception attached (raised by abort() or raise())";}
            handleCrash(summary[0] != '\0' ? "Fatal exception" : "Fatal signal", details.str(), 128 + signalValue);
        }
        void invalidParameterHandler(const wchar_t* expression, const wchar_t* function,
                                     const wchar_t* file, unsigned int line, uintptr_t /*reserved*/) {
            std::string details =
                "A CRT function was called with arguments it rejects (bad index, null buffer, bad format).";
        #if defined(_WIN32)
            // The release UCRT usually passes nulls here, but when it does not these are
            // the only clue to WHICH call failed: the stack alone stops inside ucrtbase,
            // several frames below anything in this codebase.
            auto narrow = [](const wchar_t* wide) {
                if (wide == nullptr || *wide == L'\0') return std::string();
                char buffer[512] = {};
                if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer),
                                        nullptr, nullptr) <= 0) {
                    return std::string();
                }
                return std::string(buffer);
            };
            const std::string expressionText = narrow(expression);
            const std::string functionText = narrow(function);
            const std::string fileText = narrow(file);
            if (!functionText.empty()) details += "\nFunction: " + functionText;
            if (!expressionText.empty()) details += "\nExpression: " + expressionText;
            if (!fileText.empty()) {
                details += "\nAt: " + fileText + ":" + std::to_string(line);
            }
            if (functionText.empty() && expressionText.empty() && fileText.empty()) {
                details +=
                    "\nThe CRT reported no function name (normal in a release build). A common "
                    "cause is writing to stdout/stderr in a windowed build launched without a "
                    "console - see the stdio redirect in CrashReporter::Install.";
            }
        #endif
            handleCrash("CRT invalid parameter", details, EXIT_FAILURE);
        }
        void pureCallHandler() {
            handleCrash("Pure virtual call",
                        "A virtual call was made on an object with no valid vtable. This usually means the object "
                        "was destroyed, or its class lives in a DLL that has already been unloaded.",
                        EXIT_FAILURE);
        }
        #endif
        #if defined(__linux__) || defined(__APPLE__)
        void signalHandlerWithInfo(int signalValue, siginfo_t* info, void* /*ucontext*/) {
            static constexpr char kMessage[] = "[CrashReporter] Fatal signal captured. Wrote POSIX crash log.\n";
            if (gSignalLoggingReady) {
                writeSignalCrashLog(signalValue, info);
                writeSignalSummaryFile(signalValue);
            }
            (void)!::write(STDERR_FILENO, kMessage, sizeof(kMessage) - 1);
            std::signal(signalValue, SIG_DFL);
            std::raise(signalValue);
            std::_Exit(128 + signalValue);
        }
        #endif
        void terminateHandler() {
            std::string details = "No active exception.";
            if (const std::exception_ptr ex = std::current_exception()) {
                try {std::rethrow_exception(ex);}
                catch (const std::exception& e) {details = e.what();}
                catch (...) {details = "Non-standard exception";}
            }   handleCrash("Unhandled termination", details, EXIT_FAILURE);
        }
        #if defined(_WIN32)
        LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
            static char summary[1024];
            reportWindowsException(exceptionInfo, summary, sizeof(summary));
            std::ostringstream details;
            if (summary[0] != '\0') {details << summary;}
            else                    {details << "Structured exception with no accessible record.";}
            if (gWinCrashLogPath[0]) {
                char narrowPath[kWinPathBufferSize * 2] = {};
                if (WideCharToMultiByte(CP_UTF8, 0, gWinCrashLogPath, -1, narrowPath,
                                        static_cast<int>(sizeof(narrowPath)), nullptr, nullptr) > 0) {
                    details << "\nFull report: " << narrowPath;
                }
            }
            handleCrash("Unhandled SEH exception", details.str(), EXIT_FAILURE);
        }
        #endif
        std::string readFilePreview(const fs::path& path) {
            std::ifstream in(path);
            if (!in.is_open()) {return "Unable to read crash log.";}
            std::string line;
            std::deque<std::string> tail;
            while (std::getline(in, line)) {
                tail.push_back(line);
                if (tail.size() > 30) {tail.pop_front();}
            }
            std::ostringstream out;
            for (const auto& entry : tail) {out << entry << '\n';}
            return out.str();
        }
        void applyCrashReporterTheme() {
            ImGuiStyle& style = ImGui::GetStyle();
            ImVec4* colors = style.Colors;
            const ImVec4 slate = ImVec4(0.11f, 0.12f, 0.19f, 1.00f);
            const ImVec4 panel = ImVec4(0.16f, 0.16f, 0.24f, 1.00f);
            const ImVec4 overlay = ImVec4(0.10f, 0.11f, 0.17f, 0.98f);
            const ImVec4 accent = ImVec4(0.48f, 0.56f, 0.86f, 1.00f);
            const ImVec4 accentMuted = ImVec4(0.38f, 0.46f, 0.74f, 1.00f);
            const ImVec4 highlight = ImVec4(0.22f, 0.23f, 0.34f, 1.00f);
            style.WindowPadding = ImVec2(14.0f, 14.0f);
            style.FramePadding = ImVec2(10.0f, 8.0f);
            style.ItemSpacing = ImVec2(10.0f, 8.0f);
            style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
            style.ScrollbarSize = 14.0f;
            style.WindowRounding = 10.0f;
            style.ChildRounding = 10.0f;
            style.FrameRounding = 8.0f;
            style.PopupRounding = 8.0f;
            style.GrabRounding = 8.0f;
            style.TabRounding = 8.0f;
            style.WindowBorderSize = 1.0f;
            style.ChildBorderSize = 1.0f;
            style.FrameBorderSize = 0.0f;
            colors[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.97f, 1.00f);
            colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.62f, 0.70f, 1.00f);
            colors[ImGuiCol_WindowBg] = slate;
            colors[ImGuiCol_ChildBg] = panel;
            colors[ImGuiCol_PopupBg] = overlay;
            colors[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.34f, 0.70f);
            colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
            colors[ImGuiCol_MenuBarBg] = ImVec4(0.09f, 0.10f, 0.16f, 1.00f);
            colors[ImGuiCol_Header] = highlight;
            colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.28f, 0.38f, 1.00f);
            colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.30f, 0.42f, 1.00f);
            colors[ImGuiCol_Button] = ImVec4(0.22f, 0.23f, 0.32f, 1.00f);
            colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.30f, 0.42f, 1.00f);
            colors[ImGuiCol_ButtonActive] = ImVec4(0.33f, 0.36f, 0.48f, 1.00f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.21f, 0.30f, 1.00f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.28f, 0.40f, 1.00f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.34f, 0.46f, 1.00f);
            colors[ImGuiCol_TitleBg] = ImVec4(0.11f, 0.12f, 0.18f, 1.00f);
            colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.17f, 0.24f, 1.00f);
            colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.09f, 0.10f, 0.15f, 1.00f);
            colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.23f, 0.34f, 1.00f);
            colors[ImGuiCol_SeparatorHovered] = ImVec4(0.34f, 0.36f, 0.52f, 1.00f);
            colors[ImGuiCol_SeparatorActive] = ImVec4(0.44f, 0.50f, 0.70f, 1.00f);
            colors[ImGuiCol_ScrollbarBg] = ImVec4(0.11f, 0.12f, 0.18f, 1.00f);
            colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.26f, 0.36f, 1.00f);
            colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.35f, 0.48f, 1.00f);
            colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.42f, 0.58f, 1.00f);
            colors[ImGuiCol_CheckMark] = accent;
            colors[ImGuiCol_SliderGrab] = accent;
            colors[ImGuiCol_SliderGrabActive] = accentMuted;
            colors[ImGuiCol_ResizeGrip] = ImVec4(0.28f, 0.30f, 0.42f, 1.00f);
            colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.38f, 0.44f, 0.60f, 0.80f);
            colors[ImGuiCol_ResizeGripActive] = accent;
            colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.24f);
            colors[ImGuiCol_NavHighlight] = accent;
            colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.05f, 0.06f, 0.09f, 0.70f);
        }
        GLuint createTextureFromImage(const fs::path& imagePath, int& width, int& height) {
            int channels = 0;
            const StbFlipScope flipForUi(false);
            unsigned char* pixels = stbi_load(imagePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (!pixels) {return 0;}
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
            return texture;
        }
        std::string valueForArg(int argc, char** argv, const std::string& name) {
            for (int i = 1; i + 1 < argc; ++i) {
                if (std::strcmp(argv[i], name.c_str()) == 0) {return argv[i + 1];}
            }   return {};
        }
        bool hasArg(int argc, char** argv, const std::string& name) {
            for (int i = 1; i < argc; ++i) {
                if (std::strcmp(argv[i], name.c_str()) == 0) {return true;}
            }   return false;
        }
        fs::path createPreviewLog(const std::string& productName) {
            const fs::path path = crashDirectory() / (productName + "-preview.log");
            std::ofstream out(path, std::ios::trunc);
            if (!out.is_open()) {return {};}
            out << "[CrashReporter] Preview session started at " << nowForDisplay() << "\n";
            out << "[DEBUG] Renderer backend: OpenGL\n";
            out << "[INFO] Opening sample crash reporter preview window.\n";
            out << "[WARN] This is mock data for UI iteration only.\n";
            out << "[ERROR] Example exception: Failed to resolve preview resource handle.\n";
            out << "[ERROR] Stack frame 0: Modularity::Preview::OpenCrashReporter()\n";
            out << "[ERROR] Stack frame 1: Modularity::Engine::Tick()\n";
            out << "[ERROR] Stack frame 2: main\n";
            return path;
        }
        float saturate(float value) {return std::clamp(value, 0.0f, 1.0f);}
        float easeOutCubic(float value) {const float t = 1.0f - saturate(value); return 1.0f - (t * t * t);}
        int runCrashReporterWindow(const std::string& productName, const std::string& reason, const std::string& details, const fs::path& logPath) {
            if (!glfwInit()) {return EXIT_FAILURE;}
        #if MODULARITY_OPENGL_ES
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        #else
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        #endif
            GLFWwindow* window = glfwCreateWindow(920, 720, (productName + " Crash Reporter").c_str(), nullptr, nullptr);
            if (!window) {glfwTerminate(); return EXIT_FAILURE;}
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);
        #if MODULARITY_OPENGL_ES
            const bool glLoaded = true;
        #else
            const bool glLoaded = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress) != 0;
        #endif
            if (!glLoaded) {
                glfwDestroyWindow(window);
                glfwTerminate();
                return EXIT_FAILURE;
            }
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            applyCrashReporterTheme();
            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init(Modularity::OpenGLImGuiGlslVersion());
            AudioSystem audio;
            const bool audioReady = audio.init();
            if (audioReady) {audio.playPreview("Resources/Sounds/Crash Error.mp3", 0.95f, false);}
            int logoWidth = 0;
            int logoHeight = 0;
            GLuint logoTexture = createTextureFromImage(fs::current_path() / "Resources/Engine-Root/Modu-Logo.png", logoWidth, logoHeight);
            bool splash = true;
            const auto splashStart = std::chrono::steady_clock::now();
            const std::string logPreview = readFilePreview(logPath);
            const float splashDurationSeconds = 1.3f;
            const float revealDurationSeconds = 0.55f;
            while (!glfwWindowShouldClose(window)) {
                glfwPollEvents();
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                const float elapsedSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - splashStart).count();
                splash = elapsedSeconds < splashDurationSeconds;
                const float revealT = easeOutCubic((elapsedSeconds - splashDurationSeconds) / revealDurationSeconds);
                const float heroAlpha = revealT;
                const float heroOffset = (1.0f - revealT) * 24.0f;
                const float contentT = easeOutCubic((elapsedSeconds - splashDurationSeconds - 0.08f) / revealDurationSeconds);
                const float contentAlpha = contentT;
                const float contentOffset = (1.0f - contentT) * 38.0f;
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(io.DisplaySize);
                ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
                ImGui::Begin("Crash Reporter Root", nullptr, flags);
                if (splash) {
                    ImGui::Dummy(ImVec2(0.0f, 84.0f));
                    const float imageSize = 160.0f;
                    if (logoTexture != 0) {
                        ImGui::SetCursorPosX((io.DisplaySize.x - imageSize) * 0.5f);
                        ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(logoTexture)), ImVec2(imageSize, imageSize));
                    }
                    ImGui::Dummy(ImVec2(0.0f, 20.0f));
                    ImGui::SetCursorPosX((io.DisplaySize.x - 220.0f) * 0.5f);
                    ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.97f, 1.0f), "Preparing crash report...");
                    ImGui::Dummy(ImVec2(0.0f, 10.0f));
                    ImGui::SetCursorPosX((io.DisplaySize.x - 320.0f) * 0.5f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.48f, 0.56f, 0.86f, 1.0f));
                    static float progress = 0.0f;
                    progress = std::min(1.0f, progress + 0.02f);
                    ImGui::ProgressBar(progress, ImVec2(320.0f, 10.0f), "");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + heroOffset);
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, heroAlpha);
                    ImGui::BeginChild("hero", ImVec2(0, 150), true);
                    if (logoTexture != 0) {
                        ImGui::SetCursorPos(ImVec2(18.0f, 22.0f));
                        ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(logoTexture)), ImVec2(96.0f, 96.0f));
                    }
                    ImGui::SetCursorPos(ImVec2(136.0f, 28.0f));
                    ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.97f, 1.0f), "%s has crashed", productName.c_str());
                    ImGui::SetCursorPos(ImVec2(136.0f, 60.0f));
                    ImGui::TextWrapped("A crash log was captured for this session. Review the details below, then open the log or file an issue.");
                    ImGui::SetCursorPos(ImVec2(136.0f, 104.0f));
                    ImGui::TextColored(ImVec4(0.48f, 0.56f, 0.86f, 1.0f), "Crash log: %s", logPath.filename().string().c_str());
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    ImGui::Dummy(ImVec2(0.0f, 10.0f + contentOffset));
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, contentAlpha);
                    ImGui::BeginChild("reporter_content", ImVec2(0, -70), true);
                    ImGui::TextColored(ImVec4(0.48f, 0.56f, 0.86f, 1.0f), "Reason");
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", reason.c_str());
                    if (!details.empty()) {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.48f, 0.56f, 0.86f, 1.0f), "Details");
                        ImGui::Separator();
                        ImGui::BeginChild("details_panel", ImVec2(0, 110), true, ImGuiWindowFlags_HorizontalScrollbar);
                        ImGui::TextUnformatted(details.c_str());
                        ImGui::EndChild();
                    }
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.48f, 0.56f, 0.86f, 1.0f), "Recent log output");
                    ImGui::Separator();
                    ImGui::BeginChild("log_preview", ImVec2(0, 260), true, ImGuiWindowFlags_HorizontalScrollbar);
                    ImGui::TextUnformatted(logPreview.c_str());
                    ImGui::EndChild();
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    if (ImGui::Button("Open Log", ImVec2(140, 0))) {
                        openPath(logPath);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Report Issue", ImVec2(140, 0))) {
                        launchUrl("https://git.shockinteractive.xyz/Tareno-Labs-LLC/Modularity/issues");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Open Crash Folder", ImVec2(160, 0))) {
                        openPath(logPath.parent_path());
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close", ImVec2(110, 0))) {
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                }
                ImGui::End();
                ImGui::Render();
                int displayW = 0;
                int displayH = 0;
                glfwGetFramebufferSize(window, &displayW, &displayH);
                glViewport(0, 0, displayW, displayH);
                glClearColor(0.11f, 0.12f, 0.19f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glfwSwapBuffers(window);
            }
            if (logoTexture != 0) {glDeleteTextures(1, &logoTexture);}
            if (audioReady) {audio.shutdown();}
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_SUCCESS;
        }
    }
    bool HandleCrashReporterMode(int argc, char** argv) {
        const bool previewMode = hasArg(argc, argv, "--crash-reporter-preview");
        if (!previewMode && !hasArg(argc, argv, "--crash-reporter")) {return false;}
        if (argc > 0 && argv && argv[0]) {context().executablePath = argv[0];}
        const std::string productName = valueForArg(argc, argv, "--product-name");
        std::string reason = valueForArg(argc, argv, "--crash-reason");
        std::string details = valueForArg(argc, argv, "--crash-details");
        fs::path logPath = valueForArg(argc, argv, "--crash-log");
        const std::string resolvedProductName = productName.empty() ? "Modularity" : productName;
        if (previewMode) {
            if (reason.empty()) {reason = "Preview crash dialog";}
            if (details.empty()) {
                details = "This is a preview-only crash reporter window.\n" "This is used to preview the layout, text and it helps to debug the output of the text output on screen.";
            }
            if (logPath.empty()) {
                logPath = createPreviewLog(resolvedProductName);
            }
        }
        runCrashReporterWindow(productName.empty() ? "Modularity" : productName, reason.empty() ? "Unknown crash" : reason, details, logPath); return true;
    }
    void Initialize(const std::string& productName, const std::string& executablePath) {
        auto& ctx = context();
        ctx.productName = productName;
        ctx.executablePath = executablePath;
        const std::string stamp = nowForFileName();
        ctx.sessionLogPath = crashDirectory() / (productName + "-session-" + stamp + ".log");
    #if defined(_WIN32)
        storePathForCrash(crashDirectory() / (productName + "-crash-" + stamp + ".log"),
                          gWinCrashLogPath, kWinPathBufferSize);
        storePathForCrash(crashDirectory() / (productName + "-crash-" + stamp + ".dmp"),
                          gWinMiniDumpPath, kWinPathBufferSize);
    #endif
    #if defined(__linux__) || defined(__APPLE__)
        storePathForSignal(ctx.sessionLogPath, gSessionLogPathForSignal, sizeof(gSessionLogPathForSignal));
        storePathForSignal(crashDirectory() / (productName + "-signal-last.log"), gSignalCrashLogPath, sizeof(gSignalCrashLogPath));
        storePathForSignal(crashDirectory() / "last_crash.txt", gCrashSummaryPathForSignal, sizeof(gCrashSummaryPathForSignal));
        gSignalLoggingReady = 1;
    #endif
    #if defined(_WIN32)
        // Release builds are GUI-subsystem binaries (WIN32_EXECUTABLE in CMakeLists),
        // so the process only has stdout/stderr if whoever launched it supplied them.
        // Start it from a terminal and it inherits that console; start it from Explorer
        // and the C stdio streams point at nothing.
        //
        // On its own that is harmless - writing to a dead stream just fails. What makes
        // it fatal is the invalid-parameter handler installed below, which treats any
        // CRT complaint as a crash. So a bare fprintf(stderr, ...) - which Engine::init
        // does for its startup timings - takes the editor down before the window even
        // appears, reporting "CRT invalid parameter" over a stack that ends inside
        // ucrtbase and names nothing useful.
        //
        // Two things had to be right here, and the first attempt got both wrong:
        //
        //   * Test the CRT stream, not GetStdHandle. A std handle can come back non-null
        //     and still leave the stream without a usable descriptor. Checking only the
        //     handle redirected stdout, decided stderr was fine, and the crash survived
        //     unchanged. _fileno is what fprintf itself ends up using, so ask that.
        //   * One file per stream. Pointing both at a single path left the second
        //     freopen failing, and freopen closes the original stream before it tries to
        //     reopen - so a failed redirect leaves the stream deader than it found it.
        {
            auto streamIsDead = [](FILE* stream) {
                return stream == nullptr || _fileno(stream) < 0;
            };
            auto redirectStream = [](FILE* stream, const std::string& path) {
                FILE* reopened = nullptr;
                if (freopen_s(&reopened, path.c_str(), "w", stream) == 0 && reopened != nullptr) {
                    return;
                }
                // The stream is closed at this point whether or not the reopen worked, so
                // leaving it alone is not an option. NUL always opens, and a sink that
                // discards still beats a dead FILE* that takes the process down.
                FILE* nullSink = nullptr;
                freopen_s(&nullSink, "NUL", "w", stream);
            };

            const bool outDead = streamIsDead(stdout);
            const bool errDead = streamIsDead(stderr);
            if (outDead || errDead) {
                std::error_code redirectEc;
                fs::create_directories(ctx.sessionLogPath.parent_path(), redirectEc);
                const fs::path base =
                    ctx.sessionLogPath.parent_path() / ctx.sessionLogPath.stem();
                if (outDead) redirectStream(stdout, base.string() + ".stdout.log");
                if (errDead) redirectStream(stderr, base.string() + ".stderr.log");
            }
        }
    #endif
        ctx.logFile.open(ctx.sessionLogPath, std::ios::out | std::ios::trunc);
        if (ctx.logFile.is_open()) {
            ctx.oldCout = std::cout.rdbuf();
            ctx.oldCerr = std::cerr.rdbuf();
            coutTee() = new TeeStreamBuf(ctx.oldCout, ctx.logFile.rdbuf());
            cerrTee() = new TeeStreamBuf(ctx.oldCerr, ctx.logFile.rdbuf());
            std::cout.rdbuf(coutTee());
            std::cerr.rdbuf(cerrTee());
            AppendLogLine("[CrashReporter] Session started at " + nowForDisplay());
        }
        std::set_terminate(terminateHandler);
    #if defined(_WIN32)
        std::signal(SIGABRT, signalHandler);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_invalid_parameter_handler(invalidParameterHandler);
        _set_purecall_handler(pureCallHandler);
        SetUnhandledExceptionFilter(unhandledExceptionFilter);
        AddVectoredExceptionHandler(1, firstChanceThrowRecorder);
        ULONG stackGuarantee = 64 * 1024;
        SetThreadStackGuarantee(&stackGuarantee);
    #else
        struct sigaction action {};
        action.sa_sigaction = signalHandlerWithInfo;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGABRT, &action, nullptr);
        sigaction(SIGILL, &action, nullptr);
        sigaction(SIGFPE, &action, nullptr);
        sigaction(SIGSEGV, &action, nullptr);
    #if defined(SIGBUS)
        sigaction(SIGBUS, &action, nullptr);
    #endif
    #endif
    }
    int RunProtected(const std::function<int()>& entryPoint) {
        try {return entryPoint();}
        catch (const std::exception& e) {
        #if defined(_WIN32)
            writeThrowSiteReport("Unhandled exception", e.what());
        #endif
            handleCrash("Unhandled exception", e.what(), EXIT_FAILURE);
        }
        catch (...) {
        #if defined(_WIN32)
            writeThrowSiteReport("Unhandled exception", "Non-standard exception");
        #endif
            handleCrash("Unhandled exception", "Non-standard exception", EXIT_FAILURE);
        }
    }
    void AppendLogLine(const std::string& line) {
        auto& ctx = context();
        std::lock_guard<std::mutex> lock(ctx.logMutex);
        if (!ctx.logFile.is_open()) return;
        ctx.logFile << line << '\n';
        ctx.logFile.flush();
    }
}
#endif // !__ANDROID__
