// camera_hook.cpp
// Injected into cameraserver to replace camera frames with virtual camera feed.
// Android 15 (API 35), arm64-v8a
//
// Hook targets in /system/lib64/libcameraservice.so:
//   _ZN7android7camera319Camera3OutputStream21queueBufferToConsumerE...  (frame replacement)
//   _ZN7android13Camera3Device22initializeCommonLockedEv               (stream detection)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <elf.h>
#include <android/log.h>
#include <android/hardware_buffer.h>

#include "inline_hook.h"

#define TAG "vcam_hook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── Shared memory layout (must match vcam_jni.cpp) ──────────────────────────
#define VCAM_SHM_PATH   "/data/local/tmp/vcam_shm"
#define VCAM_MAGIC      0x4D414356u  // "VCAM"

struct VCamShm {
    volatile uint32_t magic;
    volatile int32_t  enabled;
    volatile int32_t  width;
    volatile int32_t  height;
    volatile int32_t  format;       // 1 = RGBA_8888, 2 = NV21
    volatile int32_t  mirror;       // 1 = horizontal mirror
    volatile uint32_t write_seq;    // incremented by app each frame
    volatile uint32_t read_seq;     // updated by hook after reading
    uint8_t  _pad[480];             // pad header to 512 bytes
    uint8_t  frame[0];              // frame data starts here
};

// ─── Hook state ──────────────────────────────────────────────────────────────
static VCamShm* g_shm      = nullptr;
static size_t   g_shm_size = 0;

// Trampoline memory (RWX page)
static uint8_t* g_tramp_page     = nullptr;
static uint8_t  g_backup_queue[HOOK_STUB_SIZE];
static uint8_t  g_backup_init [HOOK_STUB_SIZE];

// Original function pointers (via trampolines)
typedef int (*queueBuffer_t)(void* self, void** consumer,
                             void* buffer, int fenceFd, const void* surface_ids);
typedef int (*initCommon_t) (void* self);
static queueBuffer_t g_orig_queue = nullptr;
static initCommon_t  g_orig_init  = nullptr;

// ─── AHardwareBuffer helpers (runtime dlopen to avoid NDK version issues) ────
typedef int  (*AHB_lock_t)  (AHardwareBuffer*, uint64_t, int32_t, const ARect*, void**);
typedef void (*AHB_unlock_t)(AHardwareBuffer*, int32_t*);
typedef AHardwareBuffer* (*ANW_getHB_t)(void*);

static AHB_lock_t   g_ahb_lock   = nullptr;
static AHB_unlock_t g_ahb_unlock = nullptr;
static ANW_getHB_t  g_anw_getHB  = nullptr;

static void load_ahb_symbols() {
    void* libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!libandroid) libandroid = dlopen("libandroid.so", RTLD_NOW);
    if (!libandroid) { LOGE("Cannot open libandroid.so"); return; }
    g_ahb_lock   = (AHB_lock_t)  dlsym(libandroid, "AHardwareBuffer_lock");
    g_ahb_unlock = (AHB_unlock_t)dlsym(libandroid, "AHardwareBuffer_unlock");
    g_anw_getHB  = (ANW_getHB_t) dlsym(libandroid, "ANativeWindowBuffer_getHardwareBuffer");
    if (!g_ahb_lock || !g_ahb_unlock || !g_anw_getHB)
        LOGE("Missing AHardwareBuffer symbols: lock=%p unlock=%p getHB=%p",
             g_ahb_lock, g_ahb_unlock, g_anw_getHB);
    else
        LOGI("AHardwareBuffer symbols loaded OK");
}

// ─── ELF symbol resolver ─────────────────────────────────────────────────────
static uintptr_t get_lib_base(const char* lib_name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, lib_name)) continue;
        uintptr_t base = strtoull(line, nullptr, 16);
        fclose(f);
        return base;
    }
    fclose(f);
    return 0;
}

static char* get_lib_path(const char* lib_name, char* out, size_t out_len) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, lib_name)) continue;
        // last field is the path
        char* path = strrchr(line, ' ');
        if (!path) continue;
        path++;
        size_t len = strlen(path);
        while (len > 0 && (path[len-1] == '\n' || path[len-1] == '\r')) path[--len] = 0;
        snprintf(out, out_len, "%s", path);
        fclose(f);
        return out;
    }
    fclose(f);
    return nullptr;
}

static uintptr_t find_symbol_in_lib(const char* lib_name, const char* sym_name) {
    uintptr_t base = get_lib_base(lib_name);
    if (!base) { LOGE("lib not found in maps: %s", lib_name); return 0; }

    char path[512];
    if (!get_lib_path(lib_name, path, sizeof(path))) return 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { LOGE("Cannot open %s", path); return 0; }

    struct stat st;
    fstat(fd, &st);
    void* elf = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (elf == MAP_FAILED) { LOGE("mmap failed for %s", path); return 0; }

    uintptr_t result = 0;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != ELFCLASS64)
        goto done;

    {
        Elf64_Shdr* shdrs = (Elf64_Shdr*)((char*)elf + ehdr->e_shoff);
        Elf64_Shdr* shstr_hdr = &shdrs[ehdr->e_shstrndx];
        const char* shstr = (char*)elf + shstr_hdr->sh_offset;

        Elf64_Shdr* dynsym_hdr  = nullptr;
        Elf64_Shdr* dynstr_hdr  = nullptr;
        for (int i = 0; i < ehdr->e_shnum; i++) {
            const char* name = shstr + shdrs[i].sh_name;
            if (strcmp(name, ".dynsym") == 0) dynsym_hdr = &shdrs[i];
            if (strcmp(name, ".dynstr") == 0) dynstr_hdr = &shdrs[i];
        }
        if (!dynsym_hdr || !dynstr_hdr) goto done;

        Elf64_Sym* syms = (Elf64_Sym*)((char*)elf + dynsym_hdr->sh_offset);
        const char* strs = (char*)elf + dynstr_hdr->sh_offset;
        int nsym = dynsym_hdr->sh_size / sizeof(Elf64_Sym);

        for (int i = 0; i < nsym; i++) {
            if (syms[i].st_value == 0) continue;
            const char* name = strs + syms[i].st_name;
            if (strcmp(name, sym_name) == 0) {
                result = base + syms[i].st_value;
                break;
            }
        }
    }

done:
    munmap(elf, st.st_size);
    return result;
}

// ─── Hook implementations ────────────────────────────────────────────────────
// queueBufferToConsumer hook:
// android::status_t Camera3OutputStream::queueBufferToConsumer(
//     android::sp<ANativeWindow>& consumer,
//     ANativeWindowBuffer* buffer,
//     int fenceFd,
//     const std::vector<size_t>& surface_ids)
// ARM64 ABI: x0=this, x1=&consumer, x2=buffer, x3=fenceFd, x4=&surface_ids
extern "C"
int hook_queueBufferToConsumer(void* self, void** consumer,
                                void* buffer, int fenceFd,
                                const void* surface_ids) {
    if (g_shm && g_shm->magic == VCAM_MAGIC && g_shm->enabled &&
        g_ahb_lock && g_ahb_unlock && g_anw_getHB) {

        // Cast to get width/height (ANativeWindowBuffer layout: width@8, height@12, stride@16)
        int32_t* ibuf = (int32_t*)buffer;
        // ANativeWindowBuffer: int common[6], then width, height at certain offsets
        // Layout (from system/core/include/system/window.h):
        // [0]  struct ANativeObjectBase (has 2 void*)
        // After the object base we have int fields
        // Safest: use the known offsets for AOSP:
        //   width  = *(int32_t*)((char*)buffer + 32)  on arm64 (after 4 pointers)
        //   height = *(int32_t*)((char*)buffer + 36)
        // Actually use: buffer->width = ibuf[8], height = ibuf[9] for arm64 (32-byte vtable + 2 ints before)
        // Let's use the pragmatic known layout for Android:
        // ANativeWindowBuffer in AOSP has this layout on 64-bit:
        //   +0: vtable ptr (8 bytes) — from ANativeObjectBase
        //   +8: refcount fields (8 bytes)
        //   +16: width (4)
        //   +20: height (4)
        //   +24: stride (4)
        //   +28: format (4)
        int bw = ((int32_t*)((char*)buffer + 16))[0];
        int bh = ((int32_t*)((char*)buffer + 16))[1];

        bool size_ok = (bw == g_shm->width && bh == g_shm->height);
        bool new_frame = (g_shm->write_seq != g_shm->read_seq);

        if (size_ok && new_frame) {
            AHardwareBuffer* ahb = g_anw_getHB(buffer);
            if (ahb) {
                void* data = nullptr;
                int ret = g_ahb_lock(ahb,
                    AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY,
                    -1, nullptr, &data);
                if (ret == 0 && data) {
                    size_t frame_bytes = (size_t)bw * bh * 4; // RGBA
                    memcpy(data, g_shm->frame, frame_bytes);
                    g_ahb_unlock(ahb, nullptr);
                    __atomic_store_n(&g_shm->read_seq, g_shm->write_seq, __ATOMIC_RELEASE);
                }
            }
        }
    }
    return g_orig_queue(self, consumer, buffer, fenceFd, surface_ids);
}

// initializeCommonLocked hook — just log and call original
extern "C"
int hook_initializeCommonLocked(void* self) {
    LOGI("Camera3Device::initializeCommonLocked called (this=%p)", self);
    return g_orig_init(self);
}

// ─── Shared memory setup ─────────────────────────────────────────────────────
static bool open_shared_memory() {
    // File created by app (via root) before injection
    int fd = open(VCAM_SHM_PATH, O_RDWR);
    if (fd < 0) {
        LOGE("Cannot open %s: %s", VCAM_SHM_PATH, strerror(errno));
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    g_shm_size = st.st_size;
    g_shm = (VCamShm*)mmap(nullptr, g_shm_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm == MAP_FAILED) {
        LOGE("mmap of shm failed: %s", strerror(errno));
        g_shm = nullptr;
        return false;
    }
    if (g_shm->magic != VCAM_MAGIC) {
        LOGE("shm magic mismatch: got 0x%x expected 0x%x", g_shm->magic, VCAM_MAGIC);
        munmap(g_shm, g_shm_size);
        g_shm = nullptr;
        return false;
    }
    LOGI("Shared memory mapped: %zu bytes, enabled=%d, %dx%d",
         g_shm_size, g_shm->enabled, g_shm->width, g_shm->height);
    return true;
}

// ─── Constructor — called when dlopen loads this library ─────────────────────
__attribute__((constructor))
static void vcam_hook_init() {
    LOGI("vcam_hook loaded into cameraserver");

    // 1. Load AHardwareBuffer symbols
    load_ahb_symbols();

    // 2. Open shared memory
    open_shared_memory();

    // 3. Allocate RWX page for trampolines
    g_tramp_page = (uint8_t*)mmap(nullptr, 4096,
                                   PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_tramp_page == MAP_FAILED) {
        LOGE("Failed to allocate trampoline page");
        g_tramp_page = nullptr;
        return;
    }

    // 4. Find and hook queueBufferToConsumer
    const char* SYM_QUEUE =
        "_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerE"
        "RNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorI"
        "mNS8_9allocatorImEEEE";

    uintptr_t addr_queue = find_symbol_in_lib("libcameraservice.so", SYM_QUEUE);
    if (!addr_queue) {
        LOGE("Symbol not found: queueBufferToConsumer");
    } else {
        LOGI("queueBufferToConsumer @ 0x%lx", addr_queue);
        uint8_t* tramp_q = g_tramp_page;          // uses bytes 0..31
        if (hook_install(addr_queue, (uintptr_t)hook_queueBufferToConsumer, g_backup_queue)) {
            hook_make_trampoline(addr_queue, g_backup_queue, tramp_q);
            g_orig_queue = (queueBuffer_t)(void*)tramp_q;
            LOGI("queueBufferToConsumer hook installed OK");
        } else {
            LOGE("Failed to install queueBufferToConsumer hook");
        }
    }

    // 5. Find and hook initializeCommonLocked (optional, for logging)
    const char* SYM_INIT =
        "_ZN7android13Camera3Device22initializeCommonLockedEv";

    uintptr_t addr_init = find_symbol_in_lib("libcameraservice.so", SYM_INIT);
    if (!addr_init) {
        LOGE("Symbol not found: initializeCommonLocked");
    } else {
        LOGI("initializeCommonLocked @ 0x%lx", addr_init);
        uint8_t* tramp_i = g_tramp_page + 32;     // uses bytes 32..63
        if (hook_install(addr_init, (uintptr_t)hook_initializeCommonLocked, g_backup_init)) {
            hook_make_trampoline(addr_init, g_backup_init, tramp_i);
            g_orig_init = (initCommon_t)(void*)tramp_i;
            LOGI("initializeCommonLocked hook installed OK");
        } else {
            LOGE("Failed to install initializeCommonLocked hook");
        }
    }

    LOGI("vcam_hook init complete");
}
