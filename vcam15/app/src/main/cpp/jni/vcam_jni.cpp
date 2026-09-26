// vcam_jni.cpp — JNI bridge between Kotlin and native layer
// Manages shared memory, frame writing, and injector execution

#include <jni.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <android/log.h>

#define TAG "vcam_jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── Shared memory layout (must match camera_hook.cpp) ───────────────────────
#define VCAM_SHM_PATH   "/data/local/tmp/vcam_shm"
#define VCAM_MAGIC      0x4D414356u
#define MAX_FRAME_W     1920
#define MAX_FRAME_H     1080
#define MAX_FRAME_BYTES (MAX_FRAME_W * MAX_FRAME_H * 4)
#define SHM_TOTAL_SIZE  (512 + MAX_FRAME_BYTES)

struct VCamShm {
    volatile uint32_t magic;
    volatile int32_t  enabled;
    volatile int32_t  width;
    volatile int32_t  height;
    volatile int32_t  format;
    volatile int32_t  mirror;
    volatile uint32_t write_seq;
    volatile uint32_t read_seq;
    uint8_t  _pad[480];
    uint8_t  frame[0];
};

static VCamShm* g_shm      = nullptr;
static size_t   g_shm_size = 0;

// ─── Shared memory init ───────────────────────────────────────────────────────
static bool ensure_shm(int width, int height) {
    if (g_shm) return true;

    g_shm_size = 512 + (size_t)width * height * 4;

    // Open or create the file
    int fd = open(VCAM_SHM_PATH, O_RDWR | O_CREAT, 0777);
    if (fd < 0) {
        LOGE("open shm failed: %s", strerror(errno));
        return false;
    }
    if (ftruncate(fd, (off_t)g_shm_size) < 0) {
        LOGE("ftruncate shm failed: %s", strerror(errno));
        close(fd);
        return false;
    }
    // Ensure 777 permissions so cameraserver can read (with setenforce 0)
    fchmod(fd, 0777);

    g_shm = (VCamShm*)mmap(nullptr, g_shm_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm == MAP_FAILED) {
        LOGE("mmap shm failed: %s", strerror(errno));
        g_shm = nullptr;
        return false;
    }

    // Initialize header
    g_shm->magic     = VCAM_MAGIC;
    g_shm->enabled   = 0;
    g_shm->width     = width;
    g_shm->height    = height;
    g_shm->format    = 1;  // RGBA
    g_shm->mirror    = 0;
    g_shm->write_seq = 0;
    g_shm->read_seq  = 0;

    LOGI("Shared memory initialized: %dx%d, %zu bytes", width, height, g_shm_size);
    return true;
}

// ─── JNI functions ────────────────────────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nvshen_chmp4_VCamManager_nativeInit(JNIEnv*, jclass, jint width, jint height) {
    return ensure_shm(width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nvshen_chmp4_VCamManager_nativeSetEnabled(JNIEnv*, jclass, jboolean enabled) {
    if (g_shm) {
        __atomic_store_n(&g_shm->enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
        LOGI("VCam enabled=%d", (int)enabled);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_nvshen_chmp4_VCamManager_nativeSetMirror(JNIEnv*, jclass, jboolean mirror) {
    if (g_shm) g_shm->mirror = mirror ? 1 : 0;
}

// Write an RGBA frame to shared memory
// data: ByteArray (RGBA, width*height*4 bytes)
extern "C" JNIEXPORT jboolean JNICALL
Java_com_nvshen_chmp4_VCamManager_nativeWriteFrame(JNIEnv* env, jclass,
                                                     jbyteArray data,
                                                     jint width, jint height) {
    if (!g_shm) {
        if (!ensure_shm(width, height)) return JNI_FALSE;
    }

    size_t frame_bytes = (size_t)width * height * 4;
    if (frame_bytes > MAX_FRAME_BYTES) {
        LOGE("Frame too large: %zu > %d", frame_bytes, MAX_FRAME_BYTES);
        return JNI_FALSE;
    }

    jbyte* buf = env->GetByteArrayElements(data, nullptr);
    if (!buf) return JNI_FALSE;

    memcpy(g_shm->frame, buf, frame_bytes);
    __atomic_add_fetch(&g_shm->write_seq, 1, __ATOMIC_RELEASE);

    // Update dimensions if changed
    g_shm->width  = width;
    g_shm->height = height;

    env->ReleaseByteArrayElements(data, buf, JNI_ABORT);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nvshen_chmp4_VCamManager_nativeDestroy(JNIEnv*, jclass) {
    if (g_shm) {
        g_shm->enabled = 0;
        munmap(g_shm, g_shm_size);
        g_shm = nullptr;
        g_shm_size = 0;
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nvshen_chmp4_VCamManager_nativeGetReadSeq(JNIEnv*, jclass) {
    if (!g_shm) return -1;
    return (jint)__atomic_load_n(&g_shm->read_seq, __ATOMIC_ACQUIRE);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nvshen_chmp4_VCamManager_nativeGetWriteSeq(JNIEnv*, jclass) {
    if (!g_shm) return -1;
    return (jint)__atomic_load_n(&g_shm->write_seq, __ATOMIC_ACQUIRE);
}
