#pragma once

#ifdef __ANDROID__

#include <string>
#include <vector>

struct android_app;

namespace Modularity::AndroidRuntime {

void Run(android_app* app);

bool PollEvents();

bool PresentFrame();

bool HasRenderSurface();

void GetSurfaceSize(int* outWidth, int* outHeight);

const char* GetInternalDataPath();

void* GetJavaVM();
void* GetActivityObject();

void* GetEglDisplay();
void* GetEglConfig();
void* GetEglContext();

int GetPointerCount();

bool GetPointer(int i, int* outId, float* outX, float* outY);

bool GetPrimaryPointer(float* outX, float* outY, bool* outActive);

void SetSoftKeyboardVisible(bool visible);
bool PollInputChar(unsigned int* outChar);
bool PollKeyEvent(int* outKeyCode, bool* outDown);

struct FilePickerResult {
    std::vector<std::string> paths;
    std::string error;
    bool canceled = false;
};

bool SupportsFilePicker();
bool RequestFilePicker(bool allowMultiple, std::string& error);
bool PollFilePickerResult(FilePickerResult& result);

std::string GetExternalDocumentsPath();

bool HasAllFilesAccess();

bool RequestAllFilesAccess(std::string& error);

// RECORD_AUDIO is a runtime permission. The request is async: poll HasMicrophonePermission.
bool HasMicrophonePermission();
bool RequestMicrophonePermission(std::string& error);

// Lost focus or paused (a system dialog such as a permission prompt is up, or the app is in
// the background), and how many times it has come back from that. A permission request can
// compare the count to tell "prompt still showing" from "prompt answered".
bool IsPaused();
unsigned GetFocusRegainCount();

float GetDisplayDensityScale();

} // namespace Modularity::AndroidRuntime

#endif // __ANDROID__
