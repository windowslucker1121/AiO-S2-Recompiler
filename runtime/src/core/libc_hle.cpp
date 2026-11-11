// SPDX-License-Identifier: GPL-3.0-or-later
// The SDK's string and memory functions (strlen, strcmp, memcpy, ...) as host code.
//
// A game module imports these from the SDK module, so every call goes through host_import into the lifted musl code, which works one byte
// (or one word) per lifted instruction sequence. title A's boot spends seconds in one name lookup loop that calls strlen and strncmp from a
// worker while the main thread waits for it (a 2-3.4 s frame before the title). The host's own CRT does the same work with vector
// instructions. Guest pointers are host pointers (the guest address space is mapped 1:1), so the handlers call the CRT on x0..x2 directly.
// A write into a page the write tracker armed faults and is handled the same way as a write by lifted code.
//
// Only pure functions with no locale or global state are replaced; the SDK module's own internal calls stay its own code.
// Config: [hle] libc (env NXRT_HLE_LIBC), default on; off leaves the SDK's functions in charge.
#include <cstdint>
#include <cstring>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/module.h"

namespace nxrt {
namespace {

inline void* p(::Ctx* c, int i) { return reinterpret_cast<void*>(static_cast<uintptr_t>(c->x[i])); }
inline const char* s(::Ctx* c, int i) { return reinterpret_cast<const char*>(static_cast<uintptr_t>(c->x[i])); }
inline void ret_int(::Ctx* c, int v) { c->x[0] = static_cast<uint64_t>(static_cast<int64_t>(v)); }
inline void ret_ptr(::Ctx* c, const void* v) { c->x[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v)); }

void h_strlen(::Ctx* c) { c->x[0] = std::strlen(s(c, 0)); }
void h_strnlen(::Ctx* c) { c->x[0] = strnlen(s(c, 0), static_cast<size_t>(c->x[1])); }
void h_strcmp(::Ctx* c) { ret_int(c, std::strcmp(s(c, 0), s(c, 1))); }
void h_strncmp(::Ctx* c) { ret_int(c, std::strncmp(s(c, 0), s(c, 1), static_cast<size_t>(c->x[2]))); }
void h_memcmp(::Ctx* c) { ret_int(c, std::memcmp(p(c, 0), p(c, 1), static_cast<size_t>(c->x[2]))); }
// memcpy as memmove: a guest that copies overlapping ranges gets what musl's forward copy gives for the common case, never a torn copy
void h_memcpy(::Ctx* c) { std::memmove(p(c, 0), p(c, 1), static_cast<size_t>(c->x[2])); }
void h_memmove(::Ctx* c) { std::memmove(p(c, 0), p(c, 1), static_cast<size_t>(c->x[2])); }
void h_memset(::Ctx* c) { std::memset(p(c, 0), static_cast<int>(c->x[1] & 0xFF), static_cast<size_t>(c->x[2])); }   // x0 stays dst
void h_memchr(::Ctx* c) { ret_ptr(c, std::memchr(p(c, 0), static_cast<int>(c->x[1] & 0xFF), static_cast<size_t>(c->x[2]))); }
void h_strchr(::Ctx* c) { ret_ptr(c, std::strchr(s(c, 0), static_cast<int>(c->x[1] & 0xFF))); }
void h_strrchr(::Ctx* c) { ret_ptr(c, std::strrchr(s(c, 0), static_cast<int>(c->x[1] & 0xFF))); }
void h_strstr(::Ctx* c) { ret_ptr(c, std::strstr(s(c, 0), s(c, 1))); }
void h_strcpy(::Ctx* c) {
    const char* src = s(c, 1);
    std::memmove(p(c, 0), src, std::strlen(src) + 1);   // x0 stays dst
}

}  // namespace

void install_libc_hle() {
    if (!config_get_bool("hle", "libc", true)) {
        NXLOG_I("hle", "[hle] libc = 0: the SDK's string and memory functions stay lifted code");
        return;
    }
    struct Item { const char* name; HleFn fn; };
    const Item items[] = {
        {"strlen", h_strlen}, {"strnlen", h_strnlen}, {"strcmp", h_strcmp}, {"strncmp", h_strncmp}, {"memcmp", h_memcmp},
        {"memcpy", h_memcpy}, {"memmove", h_memmove}, {"memset", h_memset}, {"memchr", h_memchr}, {"strchr", h_strchr},
        {"strrchr", h_strrchr}, {"strstr", h_strstr}, {"strcpy", h_strcpy},
    };
    for (const Item& it : items) {
        hle_register(it.name, it.fn);
        hle_mark_leaf(it.name);   // called directly from host_import: the call overhead was most of the cost once the work was native
        module_override_symbol(it.name);
    }
    NXLOG_I("hle", "%zu string and memory functions of the SDK run as host code ([hle] libc)", sizeof items / sizeof items[0]);
}

}  // namespace nxrt
