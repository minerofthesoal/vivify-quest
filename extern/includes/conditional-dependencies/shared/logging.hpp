#pragma once

#include <android/log.h>

#ifndef CONDDEPS_LOG_WARN
#define CONDDEPS_LOG_WARN(...) __android_log_print(ANDROID_LOG_WARN, "QuestHook[" ID "|" VERSION "]", __VA_ARGS__)
#endif

#ifndef CONDDEPS_LOG_INFO
#define CONDDEPS_LOG_INFO(...) __android_log_print(ANDROID_LOG_WARN, "QuestHook[" ID "|" VERSION "]", __VA_ARGS__)
#endif
