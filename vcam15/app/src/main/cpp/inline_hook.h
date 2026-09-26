#pragma once
// Minimal ARM64 inline hook
// Uses: LDR X17, #8 ; BR X17 ; .8byte target_addr  (16 bytes)
// Works for functions whose first 16 bytes contain no PC-relative instructions
// (standard AAPCS64 prologues: STP X29,X30,[SP,#-N]!  etc. are safe)

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#define HOOK_STUB_SIZE  16  // bytes we overwrite
#define TRAMP_SIZE      32  // backed-up bytes + branch back

// Align address down to page boundary
static inline uintptr_t page_align_down(uintptr_t addr) {
    return addr & ~(uintptr_t)(getpagesize() - 1);
}

// Make memory region writable, install hook, restore protection
static inline bool hook_install(uintptr_t target, uintptr_t hook_fn,
                                 uint8_t* backup_out /* [HOOK_STUB_SIZE] */) {
    if (!target || !hook_fn) return false;

    // Save original 16 bytes
    memcpy(backup_out, (void*)target, HOOK_STUB_SIZE);

    // Make page writable
    uintptr_t page = page_align_down(target);
    size_t    len  = target - page + HOOK_STUB_SIZE;
    if (mprotect((void*)page, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;

    // Write: LDR X17, #8 (0x58000051) ; BR X17 (0xD61F0220) ; <8-byte addr>
    uint32_t* p32 = (uint32_t*)target;
    p32[0] = 0x58000051u; // LDR X17, #8
    p32[1] = 0xD61F0220u; // BR  X17
    uint64_t* p64 = (uint64_t*)(target + 8);
    *p64 = (uint64_t)hook_fn;

    // Flush instruction cache
    __builtin___clear_cache((char*)target, (char*)(target + HOOK_STUB_SIZE));

    // Restore protection (RX)
    mprotect((void*)page, len, PROT_READ | PROT_EXEC);
    return true;
}

// Build a trampoline that executes original first 16 bytes then jumps back
// tramp must point to at least TRAMP_SIZE bytes of RWX memory
static inline void hook_make_trampoline(uintptr_t original_target,
                                         const uint8_t* backup /* [HOOK_STUB_SIZE] */,
                                         uint8_t* tramp) {
    // Copy backed-up instructions
    memcpy(tramp, backup, HOOK_STUB_SIZE);
    // Append: LDR X17, #8 ; BR X17 ; <original+16>
    uint32_t* p32 = (uint32_t*)(tramp + HOOK_STUB_SIZE);
    p32[0] = 0x58000051u;
    p32[1] = 0xD61F0220u;
    uint64_t* p64 = (uint64_t*)(tramp + HOOK_STUB_SIZE + 8);
    *p64 = original_target + HOOK_STUB_SIZE;
    __builtin___clear_cache((char*)tramp, (char*)(tramp + TRAMP_SIZE));
}
