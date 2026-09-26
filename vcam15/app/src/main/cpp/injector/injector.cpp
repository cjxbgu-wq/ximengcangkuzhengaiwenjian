// injector.cpp
// ptrace-based .so injector for Android 15 (arm64)
// Usage: vcam_injector <library_path> [pid]
//   If pid is omitted, scans /proc for "cameraserver"
//
// Technique: ptrace attach → remote mmap → copy lib path → remote dlopen → detach

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <asm/ptrace.h>
#include <android/log.h>
#include <dlfcn.h>

// NT_PRSTATUS may not be in all NDK headers — define if missing
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

#define TAG "vcam_injector"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── Process helpers ─────────────────────────────────────────────────────────
static pid_t find_pid_by_name(const char* name) {
    DIR* d = opendir("/proc");
    if (!d) return -1;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 0) continue;
        char path[64], comm[256];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(comm, 1, sizeof(comm)-1, f);
        fclose(f);
        comm[n] = 0;
        if (strstr(comm, name)) {
            closedir(d);
            return pid;
        }
    }
    closedir(d);
    return -1;
}

// Read a module's base address from /proc/<pid>/maps
static uintptr_t get_module_base(pid_t pid, const char* module) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, module)) continue;
        uintptr_t base = strtoull(line, nullptr, 16);
        fclose(f);
        return base;
    }
    fclose(f);
    return 0;
}

// ─── Remote memory read/write via ptrace ─────────────────────────────────────
static int remote_read(pid_t pid, uintptr_t addr, void* buf, size_t len) {
    struct iovec local  = { buf,         len };
    struct iovec remote = { (void*)addr, len };
    ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int remote_write(pid_t pid, uintptr_t addr, const void* buf, size_t len) {
    struct iovec local  = { (void*)buf,  len };
    struct iovec remote = { (void*)addr, len };
    ssize_t n = process_vm_writev(pid, &local, 1, &remote, 1, 0);
    return (n == (ssize_t)len) ? 0 : -1;
}

// ─── Remote function call via ptrace (ARM64) ─────────────────────────────────
// Saves registers, sets up call with up to 4 args, runs until SIGSEGV/SIGILL/SIGTRAP
// Returns register x0 (return value) or -1 on error
static long remote_call(pid_t pid, uintptr_t func,
                         uintptr_t arg0, uintptr_t arg1,
                         uintptr_t arg2, uintptr_t arg3,
                         uintptr_t fake_ret_addr) {
    // Save registers
    struct iovec iov;
    struct user_pt_regs saved_regs, regs;
    iov.iov_base = &saved_regs;
    iov.iov_len  = sizeof(saved_regs);
    if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) < 0) {
        ERR("PTRACE_GETREGSET failed: %s", strerror(errno));
        return -1;
    }
    memcpy(&regs, &saved_regs, sizeof(regs));

    // Set up call
    regs.regs[0] = arg0;
    regs.regs[1] = arg1;
    regs.regs[2] = arg2;
    regs.regs[3] = arg3;
    regs.pc      = func;
    regs.regs[30] = fake_ret_addr;  // LR = fake return address → causes signal

    iov.iov_base = &regs;
    if (ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) < 0) {
        ERR("PTRACE_SETREGSET failed: %s", strerror(errno));
        return -1;
    }

    // Run
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) < 0) {
        ERR("PTRACE_CONT failed: %s", strerror(errno));
        return -1;
    }

    // Wait for signal
    int status;
    waitpid(pid, &status, 0);

    // Read return value from x0
    struct user_pt_regs ret_regs;
    iov.iov_base = &ret_regs;
    iov.iov_len  = sizeof(ret_regs);
    ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov);
    long ret = (long)ret_regs.regs[0];

    // Restore registers
    iov.iov_base = &saved_regs;
    if (ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) < 0)
        ERR("PTRACE_SETREGSET restore failed");

    return ret;
}

// ─── Remote mmap (via syscall instruction in target process) ─────────────────
// We write a "SVC #0" instruction at a scratch page, then execute it
static uintptr_t remote_mmap(pid_t pid, uintptr_t scratch, size_t length) {
    // Write SVC #0 (0xD4000001) + UDF #0 (0x00000000) to scratch
    uint32_t code[] = { 0xD4000001u, 0x00000000u }; // SVC #0, UDF #0
    remote_write(pid, scratch, code, sizeof(code));

    struct iovec iov;
    struct user_pt_regs saved_regs, regs;
    iov.iov_base = &saved_regs;
    iov.iov_len  = sizeof(saved_regs);
    ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov);
    memcpy(&regs, &saved_regs, sizeof(regs));

    // Set up mmap syscall: syscall(MMAP2, 0, length, PROT_RWX, MAP_PRIVATE|MAP_ANON, -1, 0)
    regs.regs[8] = __NR_mmap;   // syscall number
    regs.regs[0] = 0;
    regs.regs[1] = length;
    regs.regs[2] = PROT_READ | PROT_WRITE | PROT_EXEC;
    regs.regs[3] = MAP_PRIVATE | MAP_ANONYMOUS;
    regs.regs[4] = (uintptr_t)-1;
    regs.regs[5] = 0;
    regs.pc      = scratch;
    iov.iov_base = &regs;
    ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov);

    ptrace(PTRACE_CONT, pid, nullptr, nullptr);
    int status;
    waitpid(pid, &status, 0);

    struct user_pt_regs ret_regs;
    iov.iov_base = &ret_regs;
    iov.iov_len  = sizeof(ret_regs);
    ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov);
    uintptr_t result = ret_regs.regs[0];

    // Restore
    iov.iov_base = &saved_regs;
    ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov);

    return result;
}

// ─── Find dlopen in target process ───────────────────────────────────────────
static uintptr_t find_remote_dlopen(pid_t pid) {
    // Try libdl.so first, fall back to linker64
    const char* lib_names[] = { "libdl.so", "linker64", nullptr };
    const char* sym_names[] = { "dlopen", "__loader_dlopen", nullptr };

    for (int li = 0; lib_names[li]; li++) {
        uintptr_t remote_base = get_module_base(pid, lib_names[li]);
        if (!remote_base) continue;

        // Find the same library in our own process
        uintptr_t local_base = get_module_base(getpid(), lib_names[li]);
        if (!local_base) continue;

        // Open local copy and look up dlopen
        void* handle = dlopen(lib_names[li], RTLD_NOW | RTLD_NOLOAD);
        if (!handle) handle = dlopen(lib_names[li], RTLD_NOW);
        if (!handle) continue;

        for (int si = 0; sym_names[si]; si++) {
            uintptr_t local_sym = (uintptr_t)dlsym(handle, sym_names[si]);
            if (!local_sym) continue;
            uintptr_t offset = local_sym - local_base;
            uintptr_t result = remote_base + offset;
            LOG("Found %s!%s: local=0x%lx remote=0x%lx",
                lib_names[li], sym_names[si], local_sym, result);
            return result;
        }
    }

    ERR("Could not find dlopen in target process");
    return 0;
}

// ─── Main injection ───────────────────────────────────────────────────────────
static int inject_library(pid_t pid, const char* lib_path) {
    LOG("Injecting %s into pid %d", lib_path, pid);

    // 1. Attach
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
        ERR("PTRACE_ATTACH failed: %s", strerror(errno));
        return -1;
    }
    int status;
    waitpid(pid, &status, 0);
    LOG("Attached to pid %d", pid);

    // 2. Find a scratch page in the target (use a known-writable mapping)
    //    Use the first writable anonymous page or stack page
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE* mf = fopen(maps_path, "r");
    uintptr_t scratch = 0;
    char line[512];
    while (fgets(line, sizeof(line), mf)) {
        uintptr_t start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[1] == 'w' && perms[2] == 'x' && end - start >= 64) {
            scratch = start;
            break;
        }
    }
    fclose(mf);

    if (!scratch) {
        // Use existing RW page and temporarily add exec permission via mprotect later
        // Fallback: use first writable anonymous mapping
        mf = fopen(maps_path, "r");
        while (fgets(line, sizeof(line), mf)) {
            uintptr_t start, end;
            char perms[8], rest[512];
            int fields = sscanf(line, "%lx-%lx %7s %*s %*s %*s %511s", &start, &end, perms, rest);
            bool anon = (fields < 4) || rest[0] == '\0' || rest[0] == '\n';
            if (perms[1] == 'w' && anon && end - start >= 64) {
                scratch = start;
                break;
            }
        }
        fclose(mf);
    }

    // 3. Remote mmap a new RWX page using scratch
    uintptr_t remote_mem = 0;
    if (scratch) {
        remote_mem = remote_mmap(pid, scratch, 4096);
        if (remote_mem == 0 || remote_mem > 0xFFFFFFF000000000ULL) {
            ERR("remote_mmap failed, result=0x%lx", remote_mem);
            remote_mem = 0;
        } else {
            LOG("Remote RWX page allocated at 0x%lx", remote_mem);
        }
    }

    if (!remote_mem) {
        ERR("Cannot allocate remote memory");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }

    // 4. Write library path to remote memory
    size_t path_len = strlen(lib_path) + 1;
    uintptr_t path_addr = remote_mem;
    uintptr_t code_addr = remote_mem + 256; // put code after the path string

    if (remote_write(pid, path_addr, lib_path, path_len) < 0) {
        ERR("remote_write path failed");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }

    // 5. Write dlopen call code: SVC + UDF to catch return
    uint32_t call_code[] = {
        0xD4000001u, // SVC #0  (placeholder; we'll use PC-based call via PTRACE_SETREGS)
        0xD4200000u, // BRK #0  (causes SIGTRAP when function returns here)
    };
    remote_write(pid, code_addr, call_code, sizeof(call_code));

    // 6. Find dlopen in remote process
    uintptr_t remote_dlopen = find_remote_dlopen(pid);
    if (!remote_dlopen) {
        ERR("Cannot find dlopen in remote process");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    LOG("Remote dlopen at 0x%lx", remote_dlopen);

    // 7. Call dlopen(path, RTLD_NOW|RTLD_GLOBAL)
    long handle = remote_call(pid, remote_dlopen,
                              path_addr,
                              RTLD_NOW | RTLD_GLOBAL,
                              0, 0,
                              code_addr + 4);  // LR → BRK instruction
    LOG("dlopen returned 0x%lx", (uintptr_t)handle);

    if (!handle) {
        ERR("dlopen returned NULL — library may not have been loaded");
    }

    // 8. Detach
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    LOG("Detached from pid %d", pid);

    return (handle != 0) ? 0 : -1;
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: vcam_injector <lib_path> [pid]\n");
        printf("  lib_path: absolute path to libvcam_hook.so\n");
        printf("  pid:      optional, defaults to finding 'cameraserver'\n");
        return 1;
    }

    const char* lib_path = argv[1];
    pid_t pid;

    if (argc >= 3) {
        pid = (pid_t)atoi(argv[2]);
    } else {
        pid = find_pid_by_name("cameraserver");
        if (pid < 0) {
            ERR("Cannot find cameraserver process");
            fprintf(stderr, "Error: cameraserver not found\n");
            return 1;
        }
    }

    LOG("Target: pid=%d lib=%s", pid, lib_path);
    printf("Injecting %s into pid %d ...\n", lib_path, pid);

    int ret = inject_library(pid, lib_path);
    if (ret == 0) {
        printf("Injection successful!\n");
        LOG("Injection successful");
    } else {
        fprintf(stderr, "Injection failed (ret=%d)\n", ret);
        LOG("Injection failed");
    }
    return ret;
}
