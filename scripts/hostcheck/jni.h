#pragma once
// Minimal jni.h stand-in so the beatsaber-hook/cordl headers can be parsed by a
// host compiler. Only the surface those headers touch is declared.
#include <cstdint>
typedef int8_t jbyte; typedef uint8_t jboolean; typedef uint16_t jchar;
typedef int16_t jshort; typedef int32_t jint; typedef int64_t jlong;
typedef float jfloat; typedef double jdouble; typedef jint jsize;
typedef void* jobject; typedef jobject jclass; typedef jobject jstring;
typedef jobject jarray; typedef jobject jthrowable; typedef jobject jobjectArray;
struct _jmethodID; typedef struct _jmethodID* jmethodID;
struct _jfieldID; typedef struct _jfieldID* jfieldID;
struct JNIEnv { void* functions; };
struct JavaVMAttachArgs { jint version; const char* name; jobject group; };
struct JavaVM {
  jint AttachCurrentThread(JNIEnv** env, void* args);
  jint DetachCurrentThread();
  jint GetEnv(void** env, jint version);
};
#define JNI_VERSION_1_6 0x00010006
