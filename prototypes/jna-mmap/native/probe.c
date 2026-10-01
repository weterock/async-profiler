#define _GNU_SOURCE
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);
static FILE *logfile;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int active;
static _Thread_local int recording;
static unsigned long sequence;

static void record(const char *op, uintptr_t addr, size_t size, int flags, int64_t result, int err) {
    if (!atomic_load(&active) || recording) return;
    recording = 1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pthread_mutex_lock(&log_lock);
    fprintf(logfile, "%lu\t%lld\t%lu\t%s\t0x%" PRIxPTR "\t%zu\t%d\t%" PRId64 "\t%d\n",
        ++sequence, (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec,
        (unsigned long)pthread_self(), op, addr, size, flags, result, err);
    fflush(logfile);
    pthread_mutex_unlock(&log_lock);
    recording = 0;
}
static void *map_hook(void *addr, size_t size, int prot, int flags, int fd, off_t offset) {
    void *p = real_mmap(addr, size, prot, flags, fd, offset);
    int err = errno;
    record("mmap", (uintptr_t)p, size, flags, (int64_t)(intptr_t)p, err);
    errno = err;
    return p;
}
static int unmap_hook(void *addr, size_t size) {
    int result = real_munmap(addr, size);
    int err = errno;
    record("munmap", (uintptr_t)addr, size, 0, result, err);
    errno = err;
    return result;
}
static void fail(JNIEnv *env, const char *msg) {
    jclass c = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (c) (*env)->ThrowNew(env, c, msg);
}
JNIEXPORT void JNICALL Java_probe_Bridge_initialize(JNIEnv *env, jclass cls, jstring path) {
    (void)cls;
    real_mmap = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_DEFAULT, "mmap");
    real_munmap = (int (*)(void *, size_t))dlsym(RTLD_DEFAULT, "munmap");
    if (!real_mmap || !real_munmap) { fail(env, "Cannot resolve libc mmap/munmap"); return; }
    const char *name = (*env)->GetStringUTFChars(env, path, NULL);
    if (!name) return;
    logfile = fopen(name, "a");
    (*env)->ReleaseStringUTFChars(env, path, name);
    if (!logfile) { fail(env, "Cannot open events.tsv"); return; }
    fprintf(logfile, "# pid=%ld mmap=%p munmap=%p\n", (long)getpid(), (void *)real_mmap, (void *)real_munmap);
    fprintf(logfile, "# seq\tmonotonic_ns\tpthread\top\taddress\trequested_length\tflags\tresult\terrno\n");
    fflush(logfile);
}
JNIEXPORT jlong JNICALL Java_probe_Bridge_redirect(JNIEnv *env, jclass cls, jlong address) {
    (void)env; (void)cls;
    if (!atomic_load(&active)) return address;
    if ((uintptr_t)address == (uintptr_t)real_mmap) return (jlong)(uintptr_t)map_hook;
    if ((uintptr_t)address == (uintptr_t)real_munmap) return (jlong)(uintptr_t)unmap_hook;
    return address;
}
JNIEXPORT void JNICALL Java_probe_Bridge_enabled(JNIEnv *env, jclass cls, jboolean value) {
    (void)env; (void)cls;
    atomic_store(&active, value != 0);
    pthread_mutex_lock(&log_lock);
    fprintf(logfile, "# %s\n", value ? "START" : "STOP");
    fflush(logfile);
    pthread_mutex_unlock(&log_lock);
}
