#pragma once
enum { ANDROID_LOG_WARN, ANDROID_LOG_ERROR };
inline int __android_log_print(int, const char*, const char*, ...) { return 0; }
