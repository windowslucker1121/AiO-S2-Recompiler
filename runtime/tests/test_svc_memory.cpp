// SPDX-License-Identifier: GPL-3.0-or-later
// Memory SVC group (src/kernel/svc_memory.cpp), driven through nxrt::svc_dispatch with a local Ctx.
#include "test_framework.h"

#include "nxrt/abi.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/svc.h"
#include "test_platform.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>

NXTEST_GROUP(svc_memory)

namespace nxrt {
void svc_memory_reset_for_tests();  // src/kernel/svc_memory.cpp: releases alias windows and the MapMemory list
}

namespace {

namespace mem = nxrt::mem;
namespace kernel = nxrt::kernel;

constexpr uint64_t kPg = 0x1000;
constexpr uint64_t kMiB = 0x100000;
constexpr uint32_t kRcCa01 = 0xCA01, kRcCc01 = 0xCC01, kRcD001 = 0xD001, kRcD401 = 0xD401, kRcD801 = 0xD801, kRcE401 = 0xE401;
constexpr uint32_t kTypeFree = 0x00, kTypeHeap = 0x05, kTypeStack = 0x0B, kTypeTransfer = 0x0D, kTypeAlias = 0x0E;

// Output of the SVC: w0 result and x1.
struct Call {
    uint32_t rc;
    uint64_t x1;
};

// Runs one SVC through the dispatcher. x0..x3 are the inputs in ABI order.
Call svc(uint32_t num, uint64_t x0 = 0, uint64_t x1 = 0, uint64_t x2 = 0, uint64_t x3 = 0) {
    ::Ctx c{};
    c.x[0] = x0;
    c.x[1] = x1;
    c.x[2] = x2;
    c.x[3] = x3;
    nxrt::svc_dispatch(&c, num);
    return Call{static_cast<uint32_t>(c.x[0]), c.x[1]};
}

// MemoryInfo as QueryMemory writes it (0x28 bytes).
struct MemInfo {
    uint64_t base;
    uint64_t size;
    uint32_t type;
    uint32_t attr;
    uint32_t perm;
    uint32_t ipc;
    uint32_t dev;
    uint32_t pad;
};
static_assert(sizeof(MemInfo) == 0x28, "MemoryInfo is 0x28 bytes");

MemInfo query(uint64_t addr) {
    MemInfo mi{};
    svc(0x06, reinterpret_cast<uint64_t>(&mi), 0, addr);
    return mi;
}

// The access itself lives in a non-inlined callee: clang-cl's SEH only models faults at call sites (without
// -fasync-exceptions), so a plain store inside the __try block would not reliably reach the handler.
// A write probe stores back the byte it read, so the contents are unchanged.
__declspec(noinline) void access_byte(uint64_t addr, bool write) {
    if (write) {
        volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(addr);
        const uint8_t old = *p;
        *p = old;
    } else {
        volatile uint8_t v = *reinterpret_cast<volatile uint8_t*>(addr);
        (void)v;
    }
}

// 1 when one byte at addr can be read (write = false) or written (write = true), 0 when it raises an access violation.
__declspec(noinline) int touch(uint64_t addr, bool write) {
    int ok = 1;
    __try {
        access_byte(addr, write);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok;
}

uint32_t* word_at(uint64_t addr) { return reinterpret_cast<uint32_t*>(addr); }

// Each case starts from a fresh memory module, a fresh handle table and fresh SVC state.
void setup() {
    nxrt::svc_memory_reset_for_tests();
    mem::shutdown();
    CHECK(mem::init());
    nxrt::svc_init();
    kernel::handles_reset();
}

void teardown() {
    nxrt::svc_memory_reset_for_tests();
    mem::shutdown();
}

}  // namespace

NXTEST(heap_is_zero_before_first_set) {
    setup();
    CHECK_EQ(nxrt::svc_current_heap_size(), 0ull);
    teardown();
}

NXTEST(set_heap_size_grow_keeps_contents) {
    setup();
    Call r = svc(0x01, 0, 2 * kMiB);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, mem::kHeapBase);
    CHECK_EQ(nxrt::svc_current_heap_size(), 2 * kMiB);
    *word_at(mem::kHeapBase) = 0x11223344u;
    *word_at(mem::kHeapBase + 2 * kMiB - 4) = 0x55667788u;

    r = svc(0x01, 0, 6 * kMiB);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, mem::kHeapBase);
    CHECK_EQ(nxrt::svc_current_heap_size(), 6 * kMiB);
    CHECK_EQ(*word_at(mem::kHeapBase), 0x11223344u);
    CHECK_EQ(*word_at(mem::kHeapBase + 2 * kMiB - 4), 0x55667788u);
    *word_at(mem::kHeapBase + 6 * kMiB - 4) = 0x99AABBCCu;
    CHECK_EQ(*word_at(mem::kHeapBase + 6 * kMiB - 4), 0x99AABBCCu);
    teardown();
}

NXTEST(set_heap_size_shrink_makes_tail_inaccessible) {
    setup();
    CHECK_EQ(svc(0x01, 0, 6 * kMiB).rc, 0u);
    *word_at(mem::kHeapBase + 5 * kMiB) = 0xABCDu;
    CHECK_EQ(touch(mem::kHeapBase + 5 * kMiB, false), 1);

    Call r = svc(0x01, 0, 2 * kMiB);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(nxrt::svc_current_heap_size(), 2 * kMiB);
    CHECK_EQ(touch(mem::kHeapBase + 5 * kMiB, false), 0);  // the tail is decommitted
    CHECK_EQ(touch(mem::kHeapBase, true), 1);              // the kept part still works

    // Growing again commits the tail afresh.
    CHECK_EQ(svc(0x01, 0, 4 * kMiB).rc, 0u);
    CHECK_EQ(touch(mem::kHeapBase + 3 * kMiB, true), 1);
    teardown();
}

NXTEST(set_heap_size_misaligned_gives_0xCA01) {
    setup();
    CHECK_EQ(svc(0x01, 0, 0x200001).rc, kRcCa01);
    CHECK_EQ(svc(0x01, 0, kMiB).rc, kRcCa01);
    CHECK_EQ(nxrt::svc_current_heap_size(), 0ull);
    CHECK(!mem::heap_exists());
    teardown();
}

// Switch 1 kernels refuse a heap above 4 GiB. Switch 2 applications set heaps of several GiB: there only nxrt's heap region (mem::kHeapMax)
// bounds the request. The platform is named because the test game may be either (oot is a Switch 2 title).
NXTEST(set_heap_size_too_large_gives_0xD001) {
    {
        nxtest::ScopedPlatform sw1(nxrt::game::Platform::Switch1);
        setup();
        CHECK_EQ(svc(0x01, 0, 0x100000000ull + 0x200000ull).rc, kRcD001);
        CHECK_EQ(nxrt::svc_current_heap_size(), 0ull);
        teardown();
    }
    nxtest::ScopedPlatform sw2(nxrt::game::Platform::Switch2);
    setup();
    CHECK_EQ(svc(0x01, 0, mem::kHeapMax + 0x200000ull).rc, kRcD001);
    CHECK_EQ(nxrt::svc_current_heap_size(), 0ull);
    teardown();
}

NXTEST(query_memory_heap_and_gap) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    MemInfo mi{};
    Call r = svc(0x06, reinterpret_cast<uint64_t>(&mi), 0, mem::kHeapBase + 4 * kPg);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0ull);
    CHECK_EQ(mi.base, mem::kHeapBase);
    CHECK_EQ(mi.size, 2 * kMiB);
    CHECK_EQ(mi.type, kTypeHeap);
    CHECK_EQ(mi.perm, 3u);
    CHECK_EQ(mi.attr, 0u);
    CHECK_EQ(mi.ipc, 0u);
    CHECK_EQ(mi.dev, 0u);

    // Past the end of the heap is a free gap that starts right after it.
    mi = query(mem::kHeapBase + 3 * kMiB);
    CHECK_EQ(mi.base, mem::kHeapBase + 2 * kMiB);
    CHECK_EQ(mi.type, kTypeFree);
    CHECK_EQ(mi.perm, 0u);
    teardown();
}

NXTEST(set_permission_read_only_then_fault) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t base = mem::kHeapBase;
    *word_at(base) = 0x1234u;
    CHECK_EQ(touch(base, true), 1);

    CHECK_EQ(svc(0x02, base, kPg, 1).rc, 0u);  // r--
    CHECK_EQ(query(base).perm, 1u);
    CHECK_EQ(touch(base, false), 1);
    CHECK_EQ(touch(base, true), 0);  // a write now faults
    CHECK_EQ(*word_at(base), 0x1234u);

    CHECK_EQ(svc(0x02, base, kPg, 3).rc, 0u);  // rw- again
    CHECK_EQ(touch(base, true), 1);
    teardown();
}

NXTEST(set_permission_errors) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t base = mem::kHeapBase;
    CHECK_EQ(svc(0x02, base + 1, kPg, 3).rc, kRcCc01);    // misaligned address
    CHECK_EQ(svc(0x02, base, 0, 3).rc, kRcCa01);          // zero size
    CHECK_EQ(svc(0x02, base, 0x800, 3).rc, kRcCa01);      // misaligned size
    CHECK_EQ(svc(0x02, base, kPg, 4).rc, kRcD801);        // execute only
    CHECK_EQ(svc(0x02, base, kPg, 2).rc, kRcD801);        // write only
    CHECK_EQ(svc(0x02, mem::kHeapBase + 4 * kMiB, kPg, 1).rc, kRcD401);  // beyond the committed heap
    CHECK_EQ(svc(0x02, mem::kMainBase, kPg, 3).rc, kRcD401);             // not heap or stack memory
    teardown();
}

NXTEST(set_memory_attribute_mask_checks) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t base = mem::kHeapBase;
    CHECK_EQ(svc(0x03, base, kPg, 8, 8).rc, 0u);
    CHECK_EQ(svc(0x03, base, kPg, 8, 0).rc, 0u);
    CHECK_EQ(svc(0x03, base, kPg, 1, 0).rc, 0xE801u);      // unsupported bit: InvalidCombination (mesosphere)
    CHECK_EQ(svc(0x03, base, kPg, 8, 1).rc, 0xE801u);      // value outside the mask
    CHECK_EQ(svc(0x03, base, kPg, 16, 0).rc, 0xE801u);     // PermissionLocked can only be set
    CHECK_EQ(svc(0x03, base, kPg, 16, 16).rc, kRcD401);    // ... and only on module .data/.bss, not heap
    CHECK_EQ(svc(0x03, base + 1, kPg, 8, 8).rc, kRcCc01);
    CHECK_EQ(svc(0x03, base, 0, 8, 8).rc, kRcCa01);
    teardown();
}

NXTEST(map_unmap_round_trip) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t src = mem::kHeapBase;
    const uint64_t dst = mem::kStackBase + 0x10000;
    for (uint32_t i = 0; i < 16; i++) word_at(src)[i] = 0xA0000000u + i;

    Call r = svc(0x04, dst, src, 2 * kPg);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(word_at(dst)[0], 0xA0000000u);
    CHECK_EQ(word_at(dst)[15], 0xA000000Fu);
    CHECK_EQ(touch(src, false), 0);  // the source is no-access while mapped
    CHECK_EQ(touch(dst, true), 1);
    word_at(dst)[1] = 0x77777777u;
    CHECK_EQ(query(dst).type, kTypeStack);
    CHECK_EQ(query(dst).perm, 3u);

    r = svc(0x05, dst, src, 2 * kPg);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(touch(src, true), 1);
    CHECK_EQ(word_at(src)[0], 0xA0000000u);
    CHECK_EQ(word_at(src)[1], 0x77777777u);  // the write through dst came back
    CHECK_EQ(touch(dst, false), 0);          // the destination is decommitted again
    CHECK_EQ(query(src).type, kTypeHeap);
    CHECK_EQ(query(dst).type, kTypeFree);
    teardown();
}

// A module's static buffer (state CodeData) mapped as a stack and unmapped again is CodeData afterwards, not heap, and can be mapped again
// (UnmapMemory used to write heap state over any source).
NXTEST(unmap_restores_code_data_source) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t src = mem::kHeapBase + kMiB;
    const uint64_t dst = mem::kStackBase + 0x40000;
    mem::vm_set(src, 2 * kPg, mem::kMemCodeData, 3);
    CHECK_EQ(query(src).type, uint32_t(mem::kMemCodeData));
    CHECK_EQ(svc(0x04, dst, src, 2 * kPg).rc, 0u);
    CHECK_EQ(svc(0x05, dst, src, 2 * kPg).rc, 0u);
    CHECK_EQ(query(src).type, uint32_t(mem::kMemCodeData));
    CHECK_EQ(query(src).perm, 3u);
    CHECK_EQ(touch(src, true), 1);
    CHECK_EQ(svc(0x04, dst, src, 2 * kPg).rc, 0u);   // the same buffer again (a thread made a second time)
    CHECK_EQ(svc(0x05, dst, src, 2 * kPg).rc, 0u);
    CHECK_EQ(query(src).type, uint32_t(mem::kMemCodeData));
    teardown();
}

NXTEST(map_memory_errors) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t src = mem::kHeapBase;
    const uint64_t dst = mem::kStackBase;
    CHECK_EQ(svc(0x04, mem::kHeapBase + kMiB, src, kPg).rc, kRcD401);            // destination not in the stack region
    CHECK_EQ(svc(0x04, dst, mem::kStackBase, kPg).rc, kRcD401);                  // source not in the heap
    CHECK_EQ(svc(0x04, dst, mem::kHeapBase + 4 * kMiB, kPg).rc, kRcD401);        // source beyond the heap
    CHECK_EQ(svc(0x04, mem::kStackBase + mem::kStackSize, src, kPg).rc, kRcD401);  // destination past the stack region
    CHECK_EQ(svc(0x04, dst + 1, src, kPg).rc, kRcCc01);
    CHECK_EQ(svc(0x04, dst, src, 0).rc, kRcCa01);
    CHECK_EQ(svc(0x04, dst, src, 0x800).rc, kRcCa01);
    CHECK_EQ(query(dst).type, kTypeFree);       // nothing was mapped by the failed calls
    CHECK_EQ(touch(src, true), 1);
    teardown();
}

NXTEST(map_partial_unmap_and_unknown_pair) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t src = mem::kHeapBase;
    const uint64_t dst = mem::kStackBase + 0x20000;
    CHECK_EQ(svc(0x04, dst, src, 2 * kPg).rc, 0u);

    // No mapping of this pair: an unknown pair, and a middle piece that is neither a prefix nor a suffix.
    CHECK_EQ(svc(0x05, mem::kStackBase + 0x80000, mem::kHeapBase + 0x10000, kPg).rc, kRcD401);
    CHECK_EQ(svc(0x05, dst + kPg, src, kPg).rc, kRcD401);

    // The last page first (suffix), then the first page (prefix). The record shrinks each time.
    CHECK_EQ(svc(0x05, dst + kPg, src + kPg, kPg).rc, 0u);
    CHECK_EQ(touch(src + kPg, true), 1);
    CHECK_EQ(touch(src, false), 0);  // the first page is still mapped
    CHECK_EQ(svc(0x05, dst, src, kPg).rc, 0u);
    CHECK_EQ(touch(src, true), 1);
    CHECK_EQ(svc(0x05, dst, src, kPg).rc, kRcD401);  // already unmapped
    teardown();
}

NXTEST(shared_memory_create_map_unmap_keeps_content) {
    setup();
    Call r = svc(0x50, 0, 0x2000, 3, 3);  // x1 size, w2 owner perm, w3 remote perm
    CHECK_EQ(r.rc, 0u);
    const uint32_t h = static_cast<uint32_t>(r.x1);
    CHECK(h != 0);
    auto s = kernel::handle_as<kernel::KSharedMemory>(h);
    REQUIRE(s != nullptr);
    CHECK_EQ(s->size, 0x2000ull);
    uint32_t* host = reinterpret_cast<uint32_t*>(s->host);
    host[0] = 0xCAFE0001u;
    host[1024] = 0x1111u;  // offset 0x1000

    const uint64_t addr = mem::kAliasBase + 0x10000;
    r = svc(0x13, h, addr, 0x2000, 3);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(word_at(addr)[0], 0xCAFE0001u);  // the guest sees what the service wrote
    CHECK_EQ(word_at(addr)[1024], 0x1111u);
    word_at(addr)[1025] = 0x2222u;            // the guest writes
    CHECK_EQ(query(addr).type, 0x06u);

    r = svc(0x14, h, addr, 0x2000);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(touch(addr, false), 0);          // the view is gone again
    CHECK_EQ(s->host, reinterpret_cast<uint64_t>(host));
    CHECK_EQ(host[1025], 0x2222u);            // the guest's write is in the object
    CHECK_EQ(host[0], 0xCAFE0001u);
    teardown();
}

NXTEST(create_shared_memory_zero_size) {
    setup();
    CHECK_EQ(svc(0x50, 0, 0, 3, 3).rc, kRcCa01);
    teardown();
}

NXTEST(transfer_memory_create_map_unmap) {
    setup();
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    const uint64_t owner = mem::kHeapBase + 0x1000;
    word_at(owner)[0] = 0xD00D0001u;

    Call r = svc(0x15, 0, owner, 0x1000, 3);  // x1 addr, x2 size, w3 perm
    CHECK_EQ(r.rc, 0u);
    const uint32_t h = static_cast<uint32_t>(r.x1);
    CHECK(h != 0);

    const uint64_t addr = mem::kAliasBase + 0x20000;
    r = svc(0x51, h, addr, 0x1000, 3);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(word_at(addr)[0], 0xD00D0001u);
    CHECK_EQ(touch(addr, true), 1);
    CHECK_EQ(query(addr).type, kTypeTransfer);

    word_at(addr)[1] = 0xFEEDu;
    r = svc(0x52, h, addr, 0x1000);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(word_at(owner)[1], 0xFEEDu);  // copied back to the owner
    CHECK_EQ(word_at(owner)[0], 0xD00D0001u);
    CHECK_EQ(touch(addr, false), 0);
    teardown();
}

NXTEST(invalid_handles_and_unmapped_views) {
    setup();
    const uint64_t addr = mem::kAliasBase + 0x10000;
    CHECK_EQ(svc(0x13, 0xDEAD, addr, 0x1000, 3).rc, kRcE401);
    CHECK_EQ(svc(0x14, 0xDEAD, addr, 0x1000).rc, kRcE401);
    CHECK_EQ(svc(0x51, 0xDEAD, addr, 0x1000, 3).rc, kRcE401);

    // A transfer-memory handle is not a shared-memory handle.
    Call r = svc(0x15, 0, mem::kHeapBase, 0x1000, 3);
    CHECK_EQ(r.rc, 0u);
    const uint32_t th = static_cast<uint32_t>(r.x1);
    CHECK_EQ(svc(0x13, th, addr, 0x1000, 3).rc, kRcE401);
    CHECK_EQ(svc(0x52, th, addr, 0x1000).rc, kRcD401);  // never mapped
    teardown();
}

NXTEST(map_physical_memory_commit_write_unmap) {
    setup();
    const uint64_t addr = mem::kAliasBase + 0x40000;
    CHECK_EQ(svc(0x2C, addr, 0x2000).rc, 0u);
    *word_at(addr) = 0x5A5Au;
    *word_at(addr + kPg) = 0xA5A5u;
    CHECK_EQ(*word_at(addr), 0x5A5Au);
    CHECK_EQ(*word_at(addr + kPg), 0xA5A5u);
    MemInfo mi = query(addr);
    CHECK_EQ(mi.type, kTypeAlias);
    CHECK_EQ(mi.size, 0x2000ull);

    // MapPhysicalMemory is page granular like the console kernel's: a range that is mapped already succeeds and keeps its contents.
    CHECK_EQ(svc(0x2C, addr, 0x2000).rc, 0u);
    CHECK_EQ(*word_at(addr), 0x5A5Au);
    CHECK_EQ(*word_at(addr + kPg), 0xA5A5u);
    CHECK_EQ(svc(0x2D, addr, 0x2000).rc, 0u);
    CHECK_EQ(touch(addr, false), 0);

    CHECK_EQ(svc(0x2C, addr, 0x2000).rc, 0u);  // the range can be mapped again after the unmap
    CHECK_EQ(svc(0x2D, addr, 0x2000).rc, 0u);
    teardown();
}

NXTEST(map_physical_memory_errors) {
    setup();
    const uint64_t addr = mem::kAliasBase + 0x40000;
    CHECK_EQ(svc(0x2C, addr + 1, kPg).rc, kRcCc01);
    CHECK_EQ(svc(0x2C, addr, 0).rc, kRcCa01);
    CHECK_EQ(svc(0x2C, addr, 0x800).rc, kRcCa01);
    CHECK_EQ(svc(0x2C, mem::kHeapBase, kPg).rc, kRcD401);  // outside the alias region
    CHECK_EQ(svc(0x2D, addr, kPg).rc, 0u);                 // never mapped: unmap releases only the mapped pages, a free page is fine
    teardown();
}

NXTEST(flush_calls_return_zero) {
    setup();
    CHECK_EQ(svc(0x2A).rc, 0u);
    CHECK_EQ(svc(0x2B, mem::kHeapBase, kPg).rc, 0u);
    teardown();
}

NXTEST(svc_call_count_counts_memory_calls) {
    setup();
    const uint64_t before = nxrt::svc_call_count(0x01);
    CHECK_EQ(svc(0x01, 0, 2 * kMiB).rc, 0u);
    CHECK_EQ(svc(0x01, 0, 4 * kMiB).rc, 0u);
    CHECK_EQ(nxrt::svc_call_count(0x01) - before, 2ull);
    teardown();
}
