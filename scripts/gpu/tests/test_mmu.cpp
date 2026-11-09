// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host unit test for the pure MMU core the E5MaliG1 kernel driver uses.
// Builds:  clang++ -std=c++17 -Wall -Wextra -Werror -o test_mmu test_mmu.cpp
// This file has no kernel dependencies; it includes the shipped header.
#include "../E5MaliG1Mmu.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace E5MaliG1Mmu;

static int gFailures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++gFailures; \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ---------------------------------------------------------------------------
// Fake physical memory for table pages: PA = 0x81000000 + n*0x1000, backed
// by malloc'd zeroed pages, tracked so leaks/double-frees are detected.
// ---------------------------------------------------------------------------
struct FakePhys {
    uint64_t next = 0x81000000;
    std::map<uint64_t, void *> pages;         // phys -> cpu
    std::map<void *, uint64_t> reverse;
    unsigned allocs = 0, frees = 0;
    bool failAllocAfter = -1;                 // -1: never fail
    unsigned allocsBeforeFail = 0xffffffffu;

    static uint64_t allocPage(void *ctx) {
        FakePhys *f = static_cast<FakePhys *>(ctx);
        if (f->allocs >= f->allocsBeforeFail) return 0;
        void *p = std::calloc(1, kPageSize);
        if (!p) return 0;
        const uint64_t phys = f->next;
        f->next += kPageSize;
        f->pages[phys] = p;
        f->reverse[p] = phys;
        ++f->allocs;
        return phys;
    }
    static void freePage(void *ctx, uint64_t phys) {
        FakePhys *f = static_cast<FakePhys *>(ctx);
        auto it = f->pages.find(phys);
        if (it == f->pages.end()) {
            std::printf("FAIL double/unknown free of %llx\n", (unsigned long long)phys);
            ++gFailures;
            return;
        }
        std::free(it->second);
        f->reverse.erase(it->second);
        f->pages.erase(it);
        ++f->frees;
    }
    static void *cpuOf(void *ctx, uint64_t phys) {
        FakePhys *f = static_cast<FakePhys *>(ctx);
        auto it = f->pages.find(phys);
        return it == f->pages.end() ? nullptr : it->second;
    }
    TableOps ops() {
        return TableOps{this, &FakePhys::allocPage, &FakePhys::freePage,
                        &FakePhys::cpuOf};
    }
};

static void testEncodings() {
    // kbase mali_kbase_mmu_mode_aarch64.c constants.
    CHECK(encodeInval() == 2);
    MapFlags rw;
    // L3 ATE: type 3, AF set, AP RW (1<<6), memattr idx0 -> bits[4:2]=0,
    // share inner (3<<8).
    uint64_t ate = encodeAte(0x12345000, rw, 3);
    CHECK((ate & 3) == kEntryIsAteL3);
    CHECK(entryPhys(ate) == 0x12345000);
    CHECK((ate & kAccessFlag) != 0);
    CHECK((ate & kAccessRW) != 0);
    CHECK((ate & kShareInner) != 0);
    CHECK(((ate >> 2) & 7) == 0);                    // memattr index 0
    // read-only: AP[1:0] = 0b11 (ENTRY_ACCESS_RO) instead of 0b01.
    MapFlags ro; ro.write = false;
    uint64_t ateRo = encodeAte(0x12345000, ro, 3);
    CHECK(((ateRo >> 6) & 3) == 3);
    CHECK(((encodeAte(0x12345000, rw, 3) >> 6) & 3) == 1);
    // NX bit 54.
    MapFlags nx; nx.noExec = true;
    CHECK((encodeAte(0, nx, 3) & kNoExecBit) != 0);
    // R340: an NX leaf is valid data memory. Its protection bit must not
    // become physical address bit 54 when validating an imported target.
    for (uint64_t physical : {0xbe000000ull, 0xbe04a000ull, 0x123456789000ull}) {
        CHECK(entryPhys(encodeAte(physical, nx, 3)) == physical);
        CHECK(entryPhys(encodeAte(physical, rw, 3)) == physical);
        CHECK(entryPhys(encodePte(physical) | (0xfull << 55)) == physical);
    }
    // Non-cacheable lane 5.
    MapFlags nc; nc.memAttrIndex = kMemAttrIndexNonCacheable;
    CHECK(((encodeAte(0, nc, 3) >> 2) & 7) == 5);
    // PTE: type 3 at L0-2, AF, physical base.
    uint64_t pte = encodePte(0x81001000);
    CHECK((pte & 3) == kEntryIsPte);
    CHECK(entryPhys(pte) == 0x81001000);
    CHECK(entryIsPte(pte, 1));
    CHECK(!entryIsPte(pte, 3));
    // Level index math: L3 index = va[20:12], L2 = va[29:21], L1 = va[38:30],
    // L0 = va[47:39].
    CHECK(levelIndex(0x0000000000, 3) == 0);
    CHECK(levelIndex(0x001000, 3) == 1);
    CHECK(levelIndex(0x001ff000, 3) == 0x1ff);
    CHECK(levelIndex(0x00200000, 3) == 0);
    CHECK(levelIndex(0x00200000, 2) == 1);
    CHECK(levelIndex(1ULL << 30, 2) == 0);
    CHECK(levelIndex(1ULL << 30, 1) == 1);
    CHECK(levelIndex(1ULL << 39, 1) == 0);
    CHECK(levelIndex(1ULL << 39, 0) == 1);
    CHECK(levelSpan(3) == 4096);
    CHECK(levelSpan(2) == (2u << 20));
    CHECK(levelSpan(1) == (1u << 30));
    CHECK(levelSpan(0) == (512ULL << 30));
}

static void testLockRegion() {
    // panfrost lock_region: width = max(fls64(start ^ (end-1)), 15) - 1 and
    // start is truncated to the naturally aligned block of that width.
    uint64_t r = lockRegionEncode(0x10000000, 0x1000);
    unsigned width = r & 0x3f;
    CHECK(width == 14);                       // clamped at log2(32K)-1
    CHECK((r & ~0x3fULL) == 0x10000000);      // 16K-aligned start untouched
    // 1 MiB region: fls64(0) -> clamped 15 -> width 14? start^(end-1) has
    // bit 19 set => fls64 = 20 => width 19.
    r = lockRegionEncode(0x20000000, 1u << 20);
    CHECK((r & 0x3f) == 19);
    CHECK((r & ~0x3fULL) == 0x20000000);
    // Unaligned start is truncated to the naturally aligned block whose
    // width is driven by the highest differing bit: start=0x20018000,
    // end-1=0x20117fff -> fls64(xor)=21 -> width 20, base 0x20000000.
    r = lockRegionEncode(0x20018000, 1u << 20);
    CHECK((r & 0x3f) == 20);
    CHECK((r & ~0x3fULL) == 0x20000000);
    // Whole address space: fls64(0xffffff...fe) = 64 -> width 63.
    r = lockRegionEncode(0, ~0ULL);
    CHECK((r & 0x3f) == 63);
    CHECK((r & ~0x3fULL) == 0);
}

static void testVaAllocator() {
    VaAllocator va;
    CHECK(va.init(0x10000000, 0x20000000));   // 512 MiB window
    CHECK(va.base() == 0x10000000);
    CHECK(va.size() == 0x20000000);
    uint64_t a = va.alloc(0x10000);
    uint64_t b = va.alloc(0x10000);
    CHECK(a == 0x10000000 && b == 0x10010000);
    CHECK(va.free(a, 0x10000));
    CHECK(!va.free(a, 0x10000));              // double free refused
    uint64_t c = va.alloc(0x8000);
    CHECK(c == 0x10000000);                   // lowest hole reused
    CHECK(!va.free(c, 0x10000));              // size mismatch refused
    // Exhaustion: window smaller than request.
    VaAllocator small;
    CHECK(small.init(0x10000000, 0x10000));
    CHECK(small.alloc(0x20000) == 0);
    // Full-window alloc + free roundtrip leaves allocator empty.
    CHECK(small.alloc(0x10000) == 0x10000000);
    CHECK(small.alloc(0x1000) == 0);
    CHECK(small.free(0x10000000, 0x10000));
    CHECK(small.alloc(0x1000) == 0x10000000);
    // Reject unaligned sizes.
    CHECK(va.alloc(0x1001) == 0);
    // Out-of-range free refused.
    CHECK(!va.free(0x30000000, 0x1000));
    CHECK(!va.free(0x0ffff000, 0x1000));
    CHECK(!va.free(b, 0x800000000ULL));
    // Fixed-VA reservation: exclusive, overlap-refused, then allocatable
    // past it.
    CHECK(va.reserveFixed(0x28000000, 0x20000));
    CHECK(!va.reserveFixed(0x28010000, 0x1000));   // overlap
    CHECK(!va.reserveFixed(0x0ffff000, 0x1000));   // outside window
    CHECK(va.free(0x28000000, 0x20000));
    CHECK(va.reserveFixed(0x28000000, 0x20000));   // reusable after free
}

static void testTableBuild() {
    FakePhys fp;
    PageTable t;
    t.init(fp.ops());
    CHECK(t.initRoot());
    const uint64_t root = t.rootPhys();
    CHECK(root != 0);

    MapFlags rw;
    // Map 64 KiB at VA 0x10000000 -> PA 0x90004000. This must create the
    // L2 table (VAs 0x0-0x7fffffff share L0 entry 0) plus one L3 table.
    CHECK(t.map(0x10000000, 0x90004000, 0x10000, rw));
    CHECK(t.liveAtes() == 16);
    CHECK(t.countLive() == 16);
    for (uint64_t off = 0; off < 0x10000; off += 0x1000) {
        PageTable::Lookup l = t.lookupEntry(0x10000000 + off);
        CHECK(l.mapped);
        CHECK(l.phys == 0x90004000 + off);
        CHECK(entryPhys(l.entry) == 0x90004000 + off);
    }
    // Root L0 entry 0 is a PTE pointing at an L1 table whose entry 0 points
    // at the L2 holding our range.
    uint64_t *l0 = static_cast<uint64_t *>(FakePhys::cpuOf(&fp, root));
    CHECK(entryIsPte(l0[0], 0));
    // Alloc bookkeeping: root + L1 + L2 + L3 = 4 pages (VA 0x10000000 uses
    // L0[0] -> L1[0] -> L2[0x80] -> L3).
    CHECK(fp.allocs == 4);

    // Alias mapping of the same physical range at another VA (the BO
    // "duplicate mapping" requirement). 0x20000000 shares the same L1/L2,
    // so only one more L3 appears.
    CHECK(t.map(0x20000000, 0x90004000, 0x10000, rw));
    PageTable::Lookup l2 = t.lookupEntry(0x20000000);
    CHECK(l2.mapped && l2.phys == 0x90004000);
    // Both VAs resolve to the same PA but through independent tables.
    CHECK(t.lookupEntry(0x10000000).phys == t.lookupEntry(0x20000000).phys);
    CHECK(fp.allocs == 5);

    // Remap over live page refused without side effects.
    CHECK(!t.map(0x10000000, 0xdead000, 0x1000, rw));
    CHECK(t.lookupEntry(0x10000000).phys == 0x90004000);

    // Unmap half the first range; still-live half must survive.
    CHECK(t.unmap(0x10000000, 0x8000));
    CHECK(!t.lookupEntry(0x10000000).mapped);
    CHECK(t.lookupEntry(0x10008000).mapped);
    // Unmapping unmapped range refused.
    CHECK(!t.unmap(0x10000000, 0x8000));
    // Unmap the rest: L3 table empties and is freed (L2/L1 stay, second
    // alias still lives under the same L1).
    CHECK(t.unmap(0x10008000, 0x8000));
    CHECK(fp.frees == 1);
    // Tear down the alias and confirm every table except the root goes back.
    CHECK(t.unmap(0x20000000, 0x10000));
    CHECK(fp.frees == 4);                     // 2 L3 + 2 L2
    CHECK(t.countLive() == 0);
    CHECK(t.liveAtes() == 0);

    // Cross-L2 mapping (spans two L3 tables: 0x1ffe000..0x2006000 crosses
    // the 2 MiB boundary at 0x2000000). Everything it allocates (L1+L2+2
    // L3, the earlier ones were torn down) comes back on unmap.
    const unsigned allocsBefore = fp.allocs, freesBefore = fp.frees;
    CHECK(t.map(0x1ffe000, 0x91000000, 0x8000, rw));
    CHECK(t.countLive() == 8);
    CHECK(t.unmap(0x1ffe000, 0x8000));
    CHECK(t.countLive() == 0);
    CHECK(fp.allocs - allocsBefore == fp.frees - freesBefore);
    CHECK(fp.allocs - allocsBefore == 4);

    // Page-granular scatter across a 1 GiB span (L1 boundary).
    CHECK(t.map(0, 0x92000000, 0x1000, rw));
    CHECK(t.map(1ULL << 30, 0x92001000, 0x1000, rw));
    CHECK(t.lookupEntry(0).phys == 0x92000000);
    CHECK(t.lookupEntry(1ULL << 30).phys == 0x92001000);
    CHECK(t.unmap(0, 0x1000));
    CHECK(t.unmap(1ULL << 30, 0x1000));
    CHECK(t.countLive() == 0);

    // Argument validation.
    CHECK(!t.map(0x1001, 0x9000, 0x1000, rw));   // unaligned va
    CHECK(!t.map(0x1000, 0x9001, 0x1000, rw));   // unaligned pa
    CHECK(!t.map(0x1000, 0x9000, 0x800, rw));    // size not page multiple
    CHECK(!t.map(~0ULL - 0x1000, 0x9000, 0x1000, rw));  // va overflow
}

static void testTableUnwind() {
    // Allocation failure mid-range must leave zero partial mappings and no
    // dangling tables.
    for (unsigned failAt = 0; failAt <= 4; ++failAt) {
        FakePhys fp;
        fp.allocsBeforeFail = failAt;
        PageTable t;
        t.init(fp.ops());
        bool ok = t.map(0x10000000, 0x90000000, 0x40000, MapFlags());
        if (failAt >= 4) {
            CHECK(ok);
            CHECK(t.countLive() == 64);
        } else {
            CHECK(!ok);
            CHECK(t.countLive() == 0);
            CHECK(t.liveAtes() == 0);
            // Everything except a surviving root is unwound: the root is
            // created before the failing allocation whenever failAt >= 1.
            CHECK(fp.frees == (failAt == 0 ? 0 : fp.allocs - 1));
            // Tree must be clean: no entry maps anything.
            CHECK(!t.lookupEntry(0x10000000).mapped);
            // And a retry with unlimited allocation succeeds.
            fp.allocsBeforeFail = 0xffffffffu;
            CHECK(t.map(0x10000000, 0x90000000, 0x40000, MapFlags()));
            CHECK(t.countLive() == 64);
        }
    }
}

int main() {
    testEncodings();
    testLockRegion();
    testVaAllocator();
    testTableBuild();
    testTableUnwind();
    if (gFailures == 0) {
        std::printf("test_mmu: all checks passed\n");
        return 0;
    }
    std::printf("test_mmu: %d failure(s)\n", gFailures);
    return 1;
}
