/* Copyright The async-profiler authors. SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <string.h>
#include "incbin.h"
#include "log.h"
#include "mmapTracer.h"
#include "profiler.h"
#include "tsc.h"
#include "vmEntry.h"
#include "mmapRewriter.h"

INCLUDE_HELPER_CLASS(MMAP_BRIDGE_NAME, MMAP_BRIDGE_CLASS, "one/profiler/MmapBridge")
std::atomic<bool> MmapTracer::running(false);
std::atomic<bool> MmapTracer::transforming(false);
std::atomic<unsigned long long> MmapTracer::lost(0);
static void* (*mmap_original)(void*, size_t, int, int, int, off_t);
static int (*munmap_original)(void*, size_t);
static pthread_mutex_t mmap_mutex = PTHREAD_MUTEX_INITIALIZER;
static thread_local bool mmap_nested = false;
static bool mmap_initialized = false;
static std::atomic<bool> mmap_transform_failed(false);

// Serialize the covered calls, including the syscall, so post-call events cannot
// invert address reuse between participating threads. Not a global VM monitor.
static void record_mapping(void* address, size_t size, bool unmap) {
    if (size > (size_t)-1 - OS::page_mask) { MmapTracer::lost++; return; }
    MmapEvent event;
    event._start_time = TSC::ticks();
    event._address = (uintptr_t)address;
    event._size = (size + OS::page_mask) & ~OS::page_mask;
    event._unmap = unmap;
    // MMAP_SAMPLE uses synchronous JVMTI stack walking: this path is JNA only.
    if (!Profiler::instance()->recordSample(NULL, unmap ? 0 : event._size, MMAP_SAMPLE, &event)) {
        MmapTracer::lost++;
    }
}
static void* jna_mmap_hook(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (mmap_nested) return mmap_original(addr, size, prot, flags, fd, offset);
    mmap_nested = true;
    pthread_mutex_lock(&mmap_mutex);
    void* result = mmap_original(addr, size, prot, flags, fd, offset);
    int saved_errno = errno;
    if (MmapTracer::running && result != MAP_FAILED) {
#ifdef MAP_HUGETLB
        if (flags & MAP_HUGETLB) MmapTracer::lost++; // explicit huge pages require different rounding
        else
#endif
        record_mapping(result, size, false);
    }
    pthread_mutex_unlock(&mmap_mutex);
    mmap_nested = false;
    errno = saved_errno;
    return result;
}
static int jna_munmap_hook(void* addr, size_t size) {
    if (mmap_nested) return munmap_original(addr, size);
    mmap_nested = true;
    pthread_mutex_lock(&mmap_mutex);
    int result = munmap_original(addr, size);
    int saved_errno = errno;
    if (MmapTracer::running && result == 0) record_mapping(addr, size, true);
    pthread_mutex_unlock(&mmap_mutex);
    mmap_nested = false;
    errno = saved_errno;
    return result;
}
static jlong JNICALL mmap_address(JNIEnv* jni, jclass unused, jobject function) {
    jclass cls = jni->GetObjectClass(function);
    jfieldID field = jni->GetFieldID(cls, "peer", "J");
    jni->DeleteLocalRef(cls);
    if (field == NULL) return 0; // pending exception, never invoke an unknown address
    jlong address = jni->GetLongField(function, field);
    if (!MmapTracer::running) return address;
    if ((uintptr_t)address == (uintptr_t)mmap_original) return (jlong)(uintptr_t)jna_mmap_hook;
    if ((uintptr_t)address == (uintptr_t)munmap_original) return (jlong)(uintptr_t)jna_munmap_hook;
    return address;
}
static bool retransform_jna() {
    jvmtiEnv* jvmti = VM::jvmti();
    jint count;
    jclass* classes;
    if (jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE) return false;
    bool ok = true;
    int matched = 0;
    for (int i = 0; i < count; i++) {
        char* name = NULL;
        if (jvmti->GetClassSignature(classes[i], &name, NULL) == JVMTI_ERROR_NONE) {
            if (strcmp(name, "Lcom/sun/jna/Function;") == 0) {
                matched++;
                if (jvmti->RetransformClasses(1, classes + i) != JVMTI_ERROR_NONE) ok = false;
            }
            jvmti->Deallocate((unsigned char*)name);
        }
        VM::jni()->DeleteLocalRef(classes[i]);
    }
    jvmti->Deallocate((unsigned char*)classes);
    Log::info("mmap: JNA Function classes found: %d", matched);
    return ok;
}
Error MmapTracer::start(Arguments& args) {
    if (!VM::loaded()) return Error("mmap requires a JVM and JNA interface mapping");
    lost = 0;
    mmap_transform_failed = false;
    if (!mmap_initialized) {
        mmap_original = (decltype(mmap_original))dlsym(RTLD_DEFAULT, "mmap");
        munmap_original = (decltype(munmap_original))dlsym(RTLD_DEFAULT, "munmap");
        if (!mmap_original || !munmap_original) return Error("Cannot resolve mmap/munmap");
        JNIEnv* jni = VM::jni();
        jclass cls = jni->DefineClass(MMAP_BRIDGE_NAME, NULL, (const jbyte*)MMAP_BRIDGE_CLASS, INCBIN_SIZEOF(MMAP_BRIDGE_CLASS));
        JNINativeMethod method = {(char*)"address", (char*)"(Ljava/lang/Object;)J", (void*)mmap_address};
        if (!cls || jni->RegisterNatives(cls, &method, 1) != 0) {
            jni->ExceptionClear();
            return Error("Cannot initialize mmap bootstrap helper");
        }
        jni->DeleteLocalRef(cls);
        mmap_initialized = true;
    }
    transforming = true;
    if (VM::jvmti()->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL) != JVMTI_ERROR_NONE ||
        !retransform_jna() || mmap_transform_failed) return Error("Cannot instrument JNA Function for mmap");
    running = true;
    return Error::OK;
}
void MmapTracer::stop() {
    pthread_mutex_lock(&mmap_mutex);
    running = false;
    pthread_mutex_unlock(&mmap_mutex);
    if (transforming.exchange(false)) retransform_jna();
    if (lost) Log::warn("mmap: %llu operations lost or unsupported; leak report is incomplete", lost.load());
}
bool MmapTracer::transform(jvmtiEnv* jvmti, const char* name, jint len, const u8* data, jint* outlen, u8** out) {
    if (!transforming || !name || strcmp(name, "com/sun/jna/Function") != 0) return false;
    std::vector<u8> result;
    if (!rewriteMmapClass(data, len, result) || jvmti->Allocate(result.size(), out) != JVMTI_ERROR_NONE) {
        mmap_transform_failed = true;
        lost++;
        Log::warn("mmap: unsupported JNA Function bytecode or transformation allocation failure");
    } else {
        memcpy(*out, result.data(), result.size());
        *outlen = result.size();
    }
    return true;
}
