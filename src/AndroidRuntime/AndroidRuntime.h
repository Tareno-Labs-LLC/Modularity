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

float GetDisplayDensityScale();

} // namespace Modularity::AndroidRuntime

#endif // __ANDROID__
