// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// E5MaliG1Mmu.hpp — pure Mali-G57 (Valhall JM) AArch64-4K page-table math.
//
// Host-testable by design: no kernel headers, no IOKit, no allocation beyond
// caller-provided callbacks. The kernel driver (E5MaliG1.cpp) and the host
// unit test (../tests/test_mmu.cpp) both include this file, so the tested
// code is the shipped code.
//
// Encoding sources (fixed revisions, see ../audit-report.md §2.8):
//   V = kernel_sprd_ums9158 @63b80110 (kbase r41p0 / UK 11.36)
//       mmu/mali_kbase_mmu_mode_aarch64.c:28-201 (entry encodings)
//       mmu/backend/mali_kbase_mmu_jm.c:37-59      (AS setup values)
//       mali_kbase_defs.h:2009-2014                (share bits)
//   L = linux-lts-e5 @6b355f72 (panfrost)
//       drivers/gpu/drm/panfrost/panfrost_regs.h   (register offsets)
//       drivers/gpu/drm/panfrost/panfrost_mmu.c:102-135 (lock region encode)
//
// Table geometry: 4 KiB pages, 512 entries per table, 9 bits per level,
// levels 0..3 (48-bit VA), matching kbase MIDGARD_MMU_TOPLEVEL/BOTTOMLEVEL
// (mali_kbase_defs.h:103-106). Only page ATEs (L3) and table PTEs (L0-L2)
// are emitted; block ATEs at L0-L2 exist in the architecture but are not
// used by this driver (kbase maps with 4K granularity for GPU VMs).
#pragma once

#include <stdint.h>

namespace E5MaliG1Mmu {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
static constexpr unsigned kEntriesPerTable = 512;   // KBASE_MMU_PAGE_ENTRIES
static constexpr unsigned kLevelBits = 9;
static constexpr uint64_t kPageSize = 4096;
static constexpr int kTopLevel = 0;                 // MIDGARD_MMU_TOPLEVEL
static constexpr int kBottomLevel = 3;              // MIDGARD_MMU_BOTTOMLEVEL
static constexpr uint64_t kFullVaBits = 48;

static constexpr uint64_t alignDown(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}
static constexpr uint64_t alignUp(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}
static constexpr int fls64(uint64_t v) {  // 0 for v==0, else index of top bit + 1
    int n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

// Index of va inside the table at `level` (level 0 = root).
static constexpr unsigned levelIndex(uint64_t va, int level) {
    return (unsigned)((va >> (12 + kLevelBits * (kBottomLevel - level))) &
                      (kEntriesPerTable - 1));
}
// Size covered by one entry at `level`.
static constexpr uint64_t levelSpan(int level) {
    return kPageSize << (kLevelBits * (kBottomLevel - level));
}

// ---------------------------------------------------------------------------
// Entry encoding (kbase mali_kbase_mmu_mode_aarch64.c)
// ---------------------------------------------------------------------------
static constexpr uint64_t kEntryTypeMask = 3;
static constexpr uint64_t kEntryIsAteL3 = 3;        // ENTRY_IS_ATE_L3
static constexpr uint64_t kEntryIsAteL02 = 1;       // ENTRY_IS_ATE_L02 (block; unused here)
static constexpr uint64_t kEntryIsInval = 2;        // ENTRY_IS_INVAL
static constexpr uint64_t kEntryIsPte = 3;          // ENTRY_IS_PTE (table pointer, L0-2)

static constexpr uint64_t kAccessRW = (1ULL << 6);  // ENTRY_ACCESS_RW (AP[1:0]=01)
static constexpr uint64_t kAccessRO = (3ULL << 6);  // ENTRY_ACCESS_RO
static constexpr uint64_t kAccessFlag = (1ULL << 10); // ENTRY_ACCESS_BIT (AF)
static constexpr uint64_t kNoExecBit = (1ULL << 54);  // ENTRY_NX_BIT
static constexpr uint64_t kShareInner = (3ULL << 8);  // SHARE_INNER_BITS
static constexpr uint64_t kShareOuter = (2ULL << 8);  // SHARE_BOTH_BITS

// Memory attribute index selects one of the 8-byte lanes programmed into
// AS_MEMATTR (kbase mali_kbase_mmu_jm.c:43-55):
//   idx 0 = 0x88 impl-def cache policy (write-back, default)
//   idx 5 = 0x4C non-cacheable
// The index is stored in PTE bits [4:2] (KBASE_REG_MEM_ATTR_VALUE << 2).
static constexpr unsigned kMemAttrIndexDefault = 0;
static constexpr unsigned kMemAttrIndexNonCacheable = 5;

struct MapFlags {
    bool read = true;
    bool write = true;
    bool noExec = false;
    unsigned memAttrIndex = kMemAttrIndexDefault;   // lane in AS_MEMATTR
    bool shareInner = true;                         // system is IO-coherent
};

static constexpr uint64_t ateFlags(const MapFlags &f) {
    uint64_t v = (uint64_t)(f.memAttrIndex & 7) << 2;
    if (f.write) v |= kAccessRW;                    // AArch64 S1 has no WO
    else if (f.read) v |= kAccessRO;
    if (f.noExec) v |= kNoExecBit;
    if (f.shareInner) v |= kShareInner;
    return v;
}

static inline uint64_t encodeAte(uint64_t phys, const MapFlags &f, int level) {
    // kbase entry_set_ate(): phys | flags | AF | type (aarch64.c:141-154)
    const uint64_t type = (level == kBottomLevel) ? kEntryIsAteL3 : kEntryIsAteL02;
    return (phys & ~(kPageSize - 1)) | ateFlags(f) | kAccessFlag | type;
}

static inline uint64_t encodePte(uint64_t phys) {
    // kbase entry_set_pte(): (phy & PAGE_MASK) | AF | ENTRY_IS_PTE (aarch64.c:190-193)
    return (phys & ~(kPageSize - 1)) | kAccessFlag | kEntryIsPte;
}

static inline uint64_t encodeInval() { return kEntryIsInval; }

static inline bool entryIsValid(uint64_t e, int level) {
    if (level == kBottomLevel) return (e & kEntryTypeMask) == kEntryIsAteL3;
    const uint64_t t = e & kEntryTypeMask;
    return t == kEntryIsPte || t == kEntryIsAteL02;
}
static inline bool entryIsPte(uint64_t e, int level) {
    // A PTE (table pointer) cannot exist at the bottom level.
    if (level == kBottomLevel) return false;
    return (e & kEntryTypeMask) == kEntryIsPte;
}
static inline uint64_t entryPhys(uint64_t e) {
    // AArch64 4K address bits are [47:12] (Linux io-pgtable-arm.c,
    // ARM_LPAE_PTE_ADDR_MASK). Unlike a table-only decoder, this helper
    // also decodes leaves: exclude NX bit 54 as well as kbase's private
    // count bits [58:55]. R340's import check exposed the NX contamination.
    if (!(e & 1)) return 0;
    return e & (((1ull << 48) - 1) & ~(kPageSize - 1));
}

// ---------------------------------------------------------------------------
// MMU lock-region encoding (panfrost_mmu.c:102-135, AS_LOCKADDR)
// ---------------------------------------------------------------------------
static constexpr uint64_t kLockRegionMinSize = 1ULL << 15;  // AS_LOCK_REGION_MIN_SIZE

// Encode (start, size) as width | naturally-aligned start:
//   width = max(fls64(start ^ (end-1)), log2(1<<15)) - 1   (panfrost_mmu.c:120-121)
static inline uint64_t lockRegionEncode(uint64_t start, uint64_t size) {
    if (size == 0) return 0;
    const uint64_t end = start + size;
    unsigned width = (unsigned)fls64(start ^ (end - 1));
    if (width < 15) width = 15;   // const_ilog2(AS_LOCK_REGION_MIN_SIZE)
    width -= 1;
    return (uint64_t)width | alignDown(start, 1ULL << width);
}

// ---------------------------------------------------------------------------
// VA window allocator: first-fit over a bitmap, 4 KiB granularity.
// Pure logic; the kernel driver guards it with its own lock.
// ---------------------------------------------------------------------------
class VaAllocator {
public:
    VaAllocator() = default;
    bool init(uint64_t base, uint64_t size) {
        base_ = alignUp(base, kPageSize);
        end_ = alignDown(base + size, kPageSize);
        if (base_ >= end_) return false;
        if ((end_ - base_) / kPageSize > kMaxPages) return false;
        cursor_ = base_;
        return true;
    }
    // Returns page-aligned VA or 0 when the window is exhausted.
    uint64_t alloc(uint64_t size) {
        if (size == 0 || size % kPageSize) return 0;
        const uint64_t pages = size / kPageSize;
        if (pages > pageCount()) return 0;
        for (int pass = 0; pass < 2; ++pass) {
            const uint64_t from = pass == 0 ? pageIndex(cursor_) : 0;
            uint64_t run = 0, candidate = 0;
            for (uint64_t i = from; i < pageCount(); ++i) {
                if (!bit(i)) {
                    if (run == 0) candidate = i;
                    if (++run >= pages) return take(candidate, pages);
                } else {
                    run = 0;
                }
            }
        }
        return 0;
    }
    bool free(uint64_t va, uint64_t size) {
        if (size == 0 || size % kPageSize) return false;
        if (va < base_ || va % kPageSize) return false;
        if (va > end_ || size > end_ - va) return false;
        const uint64_t first = pageIndex(va);
        const uint64_t pages = size / kPageSize;
        if (first + pages > pageCount()) return false;
        for (uint64_t i = first; i < first + pages; ++i)
            if (!bit(i)) return false;   // double free / never allocated
        for (uint64_t i = first; i < first + pages; ++i) clear(i);
        if (cursor_ > va) cursor_ = va;  // reuse the lowest hole next
        return true;
    }
    // Mark [va, va+size) allocated for a caller-chosen fixed VA. Fails when
    // the range is outside the window or overlaps anything allocated.
    bool reserveFixed(uint64_t va, uint64_t size) {
        if (size == 0 || size % kPageSize || va % kPageSize) return false;
        if (va < base_ || va > end_ || size > end_ - va) return false;
        const uint64_t first = pageIndex(va);
        const uint64_t pages = size / kPageSize;
        if (first + pages > pageCount()) return false;
        for (uint64_t i = first; i < first + pages; ++i)
            if (bit(i)) return false;
        for (uint64_t i = first; i < first + pages; ++i) setb(i);
        return true;
    }
    uint64_t base() const { return base_; }
    uint64_t size() const { return end_ - base_; }
    uint64_t freePages() const {
        uint64_t n = 0;
        for (uint64_t i = 0; i < pageCount(); ++i) n += bit(i) ? 0 : 1;
        return n;
    }

private:
    uint64_t pageIndex(uint64_t va) const { return (va - base_) / kPageSize; }
    uint64_t pageCount() const { return (end_ - base_) / kPageSize; }
    bool bit(uint64_t i) const { return (bits_[i / 64] >> (i % 64)) & 1; }
    void setb(uint64_t i) { bits_[i / 64] |= (1ULL << (i % 64)); }
    void clear(uint64_t i) { bits_[i / 64] &= ~(1ULL << (i % 64)); }
    uint64_t take(uint64_t first, uint64_t pages) {
        for (uint64_t i = first; i < first + pages; ++i) setb(i);
        cursor_ = base_ + (first + pages) * kPageSize;
        return base_ + first * kPageSize;
    }

    static constexpr uint64_t kMaxPages = 256 * 1024;  // 1 GiB / 4 KiB window
    uint64_t bits_[(kMaxPages + 63) / 64] = {};
    uint64_t base_ = 0, end_ = 0, cursor_ = 0;
};

// ---------------------------------------------------------------------------
// Page-table tree builder/tearer. Tables are 4 KiB, 512 entries, 64-bit
// little-endian descriptors. The caller provides allocation:
//   allocPage(ctx)  -> physical address of a zeroed page (0 = fail)
//   cpuOf(ctx,phys) -> stable CPU pointer valid until freePage
//   freePage(ctx,phys)
// ---------------------------------------------------------------------------
struct TableOps {
    void *ctx;
    uint64_t (*allocPage)(void *);   // must return a zeroed page
    void (*freePage)(void *, uint64_t);
    void *(*cpuOf)(void *, uint64_t);
};

class PageTable {
public:
    void init(const TableOps &ops) { ops_ = ops; root_ = 0; live_ = 0; }
    bool initRoot() {
        if (root_) return true;
        root_ = ops_.allocPage(ops_.ctx);
        return root_ != 0;
    }
    uint64_t rootPhys() const { return root_; }
    uint64_t liveAtes() const { return live_; }

    // Map [va, va+size) -> [phys, phys+size). Both page-multiple. Fails
    // without partial state when any target page is already mapped or an
    // intermediate table allocation fails.
    bool map(uint64_t va, uint64_t phys, uint64_t size, const MapFlags &flags) {
        if (size == 0 || size % kPageSize || va % kPageSize || phys % kPageSize ||
            va + size < va || phys + size < phys) return false;
        if (!root_ && !initRoot()) return false;
        // Phase 1: verify every L3 slot in the range is free (fail fast).
        for (uint64_t off = 0; off < size; off += kPageSize)
            if (lookupEntry(va + off).mapped) return false;
        // Phase 2: map page by page. Any failure unwinds everything this
        // call did (phase 1 already ruled out occupied slots, so the only
        // failure left is intermediate-table allocation).
        uint64_t done = 0;
        for (; done < size; done += kPageSize) {
            uint64_t created[4] = {};
            if (!mapOne(va + done, phys + done, flags, created)) {
                rollbackChain(va + done, created);
                for (uint64_t back = 0; back < done; back += kPageSize)
                    dropOne(va + back);
                live_ -= done / kPageSize;
                return false;
            }
        }
        return true;
    }

    // Unmap [va, va+size) (page-multiple); frees now-empty intermediate
    // tables. Returns false, changing nothing, when a page in range is not
    // currently mapped.
    bool unmap(uint64_t va, uint64_t size) {
        if (size == 0 || size % kPageSize || va % kPageSize) return false;
        if (va + size < va) return false;
        for (uint64_t off = 0; off < size; off += kPageSize)
            if (!lookupEntry(va + off).mapped) return false;
        for (uint64_t off = 0; off < size; off += kPageSize) {
            dropOne(va + off);
            --live_;
        }
        return true;
    }

    struct Lookup {
        bool mapped;    // a valid L3 ATE covers the va
        uint64_t phys;  // page address of the ATE
        uint64_t entry; // raw descriptor
    };
    Lookup lookupEntry(uint64_t va) const {
        uint64_t table = root_;
        for (int level = kTopLevel; level < kBottomLevel; ++level) {
            if (!table) return {false, 0, 0};
            uint64_t *entries = cpu(table);
            const uint64_t e = entries[levelIndex(va, level)];
            if (!entryIsPte(e, level)) return {false, 0, e};
            table = entryPhys(e);
        }
        if (!table) return {false, 0, 0};
        const uint64_t e = cpu(table)[levelIndex(va, kBottomLevel)];
        if (!entryIsValid(e, kBottomLevel)) return {false, 0, e};
        return {true, entryPhys(e), e};
    }

    // Debug: count live entries reachable from root.
    uint64_t countLive() const {
        if (!root_) return 0;
        return countFrom(root_, kTopLevel);
    }

private:
    uint64_t countFrom(uint64_t phys, int level) const {
        uint64_t *entries = cpu(phys);
        uint64_t n = 0;
        for (unsigned i = 0; i < kEntriesPerTable; ++i) {
            const uint64_t e = entries[i];
            if (level == kBottomLevel) {
                if (entryIsValid(e, level)) ++n;
            } else if (entryIsPte(e, level)) {
                n += countFrom(entryPhys(e), level + 1);
            }
        }
        return n;
    }
    uint64_t *cpu(uint64_t phys) const {
        return static_cast<uint64_t *>(ops_.cpuOf(ops_.ctx, phys));
    }
    // Returns CPU pointer of the bottom-level table for va, creating
    // intermediate tables; records every table it allocates in created[].
    uint64_t *walkCreate(uint64_t va, uint64_t created[4]) {
        uint64_t table = root_;
        for (int l = kTopLevel; l < kBottomLevel; ++l) {
            uint64_t *entries = cpu(table);
            const unsigned idx = levelIndex(va, l);
            const uint64_t e = entries[idx];
            if (entryIsPte(e, l)) {
                table = entryPhys(e);
            } else if (!entryIsValid(e, l)) {
                const uint64_t fresh = ops_.allocPage(ops_.ctx);
                if (!fresh) return nullptr;
                created[l + 1] = fresh;
                entries[idx] = encodePte(fresh);
                table = fresh;
            } else {
                return nullptr;  // block ATE in the path: unsupported
            }
        }
        return cpu(table);
    }
    bool mapOne(uint64_t va, uint64_t phys, const MapFlags &flags,
                uint64_t created[4]) {
        uint64_t *entries = walkCreate(va, created);
        if (!entries) return false;
        const unsigned idx = levelIndex(va, kBottomLevel);
        if (entryIsValid(entries[idx], kBottomLevel)) return false;  // occupied
        entries[idx] = encodeAte(phys, flags, kBottomLevel);
        ++live_;
        return true;
    }
    // Free tables this failed page created, children first, clearing the
    // parent PTE that links each one.
    void rollbackChain(uint64_t va, uint64_t created[4]) {
        for (int l = kBottomLevel; l > kTopLevel; --l) {
            if (!created[l]) continue;
            uint64_t table = root_;
            for (int m = kTopLevel; m < l - 1; ++m)
                table = entryPhys(cpu(table)[levelIndex(va, m)]);
            cpu(table)[levelIndex(va, l - 1)] = encodeInval();
            ops_.freePage(ops_.ctx, created[l]);
        }
    }
    void dropOne(uint64_t va) {
        uint64_t chain[4] = {};   // table phys at each level
        uint64_t table = root_;
        chain[kTopLevel] = table;
        for (int l = kTopLevel; l < kBottomLevel; ++l) {
            const uint64_t e = cpu(table)[levelIndex(va, l)];
            table = entryPhys(e);   // PTE guaranteed by prior validation
            chain[l + 1] = table;
        }
        cpu(chain[kBottomLevel])[levelIndex(va, kBottomLevel)] = encodeInval();
        // Free bottom-up while a table becomes entirely non-valid. Both the
        // architecture-invalid value (2) and a never-written zero entry fault
        // identically on the hardware, so both count as empty here.
        for (int l = kBottomLevel; l > kTopLevel; --l) {
            uint64_t *entries = cpu(chain[l]);
            bool empty = true;
            for (unsigned i = 0; i < kEntriesPerTable; ++i)
                if (entryIsValid(entries[i], l)) { empty = false; break; }
            if (!empty) break;
            ops_.freePage(ops_.ctx, chain[l]);
            cpu(chain[l - 1])[levelIndex(va, l - 1)] = encodeInval();
        }
    }

    TableOps ops_ = {};
    uint64_t root_ = 0;
    uint64_t live_ = 0;
};

}  // namespace E5MaliG1Mmu
