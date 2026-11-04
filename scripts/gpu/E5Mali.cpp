//
//  E5Mali.cpp — G0/G1 first step: power the Mali-G57 (qogirn6lite) up with
//  the vendor sequence and read back its identity registers.
//
//  Hardware map (dumps/live-fdt.dts + panfrost_sprd.c + ums9621-clk.c):
//    GPU MMIO        0x23140000 + 0x4000   (mm bus, direct PA like the DPU)
//    GPU APB syscon  0x23000000 + 0x3000   clk_core_gpu_eb reg 0x0 bit2
//    GPU DVFS APB    0x23014000 + 0x3000   FREQ_UPD_TYPE_CFG0 0x9ac bits1:0,
//                                          DVFS_INDEX_CFG 0x794 bits2:0
//    PMU APB syscon  0x64910000 + 0x3000   gpll_frc_off/on 0xa20 bits1/0,
//                                          top_force_shutdown 0x2c4 bit25,
//                                          gpu_top_state 0x500 mask 0x1f0000,
//                                          GPU soft rst 0x0b98 bit2 (RMW)
//    GPU DCDC        PMIC register 0x201c bit3 via ADI (0 = enabled); the FDT
//                    phandle is a placeholder — the vendor kbase swaps the
//                    regmap to sprd,ump962x-syscon (see panfrost_sprd.c).
//  Readback targets: GPU_ID @0x00, SHADER_PRESENT_LO @0x100 (expect 0x5),
//    L2 features @0x04? panfrost_regs.h: GPU_ID 0x00, L2_FEATURES 0x04,
//    SHADER_PRESENT_LO 0x100. G57 dual (Valhall JM) id 0x9091 per plan.
//
//  Failure-tolerant by design: every step records a stage code, nothing
//  panics; results ride the E5MALI line and the M=<stage>:<id>:<sh> echo.
//

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSData.h>

extern "C" {
void e5_mali_probe(void);
unsigned e5_mali_stage = 0;
unsigned e5_mali_gpu_id = 0;
unsigned e5_mali_shader = 0;
}

// ADI/PMIC access owned by NativeSd (SD bring-up primitive).
extern "C" unsigned e5_pmic_peek(unsigned off);
extern "C" int e5_pmic_rmw(unsigned off, unsigned mask, unsigned val);

static volatile uint32_t *sMmio;   // GPU registers
static volatile uint32_t *sApb;    // GPU APB syscon
static volatile uint32_t *sDvfs;   // GPU DVFS APB
static volatile uint32_t *sPmu;    // PMU APB syscon

static void rmw(volatile uint32_t *base, uint32_t reg, uint32_t mask, uint32_t val)
{
    uint32_t v = base[reg / 4];
    v = (v & ~mask) | (val & mask);
    base[reg / 4] = v;
}

static IOMemoryMap *sMmioMap, *sApbMap, *sDvfsMap, *sPmuMap, *sAonApbMap;
static volatile uint32_t *sAonApb;

static bool mapWindow(uint64_t pa, uint32_t len, volatile uint32_t **out,
                      IOMemoryMap **mapOut)
{
    IODeviceMemory *mem = IODeviceMemory::withRange(pa, len);
    if (!mem) return false;
    IOMemoryMap *map = mem->map();
    if (!map) { mem->release(); return false; }
    // v0.3: a zero-length or short map is the fault signature we chased in
    // r177/r178 (translation fault on first touch). Log it and refuse.
    if (map->getLength() < len) {
        printf("E5MALI map pa=%llx want=%x got=%llx REFUSED\n",
               (unsigned long long)pa, len,
               (unsigned long long)map->getLength());
        map->release(); mem->release();
        return false;
    }
    *out = (volatile uint32_t *)map->getVirtualAddress();
    *mapOut = map;
    printf("E5MALI map pa=%llx len=%llx va=%p w0=%08x\n",
           (unsigned long long)pa, (unsigned long long)map->getLength(),
           *out, (*out)[0]);
    return true;
}

void e5_mali_probe(void)
{
    // v0.5: the GPU's first clock (FDT clocks[0]) = aonapb-gate idx 8 =
    // bit 8 of the AON_APB APB_EB register at syscon@64900000+0x0. r194
    // proved 0x64910000 maps+reads while 0x23140000/0x23000000 fault on
    // first touch: the GPU APB/MEM windows sit behind that AON gate bit.
    if (!mapWindow(0x64900000, 0x3000, &sAonApb, &sAonApbMap)) { e5_mali_stage = 14; return; }
    printf("E5MALI aonapb w0=%08x (bit8=%u) -> set\n", sAonApb[0], (sAonApb[0] >> 8) & 1u);
    // v0.8: real driver decode (ums512-clk.c lineage, DT indices match our
    // FDT exactly: CLK_GPU_EB=8): gpu_eb = aon-apb gate 0x64900000 word 0x0
    // BIT(11), SET mirror +0x1000; mm_eb = BIT(9). The DT "idx 8" is the
    // dt-bindings index, NOT the bit (r195-r197 wrote bit 8 = nothing).
    sAonApb[0x1000 / 4] = (0x1u << 11) | (0x1u << 9) | (0x1u << 8);
    IOSleep(2);
    printf("E5MALI aonapb w0 after set = %08x (want bit11/9/8)\n", sAonApb[0]);
    if (!mapWindow(0x64910000, 0x3000, &sPmu, &sPmuMap))   { e5_mali_stage = 13; return; }
    // v0.6: the GPU MMIO (0x23140000) and GPU APB syscon (0x23000000/0x23014000)
    // windows live INSIDE the GPU power island — mapping them (or touching page0)
    // before the island is powered faults with a level-3 translation abort
    // (r194/r195: PMU 0x64910000 maps fine, 0x23140000 faults at map-probe time
    // even with the aonapb GPU APB gate bit set). Power up FIRST, map LAST.
    e5_mali_stage = 1;

    printf("E5MALI step1 gpll enter\n");
    // 1. GPLL: clear force-off (bit1), then force-on (bit0) at 0xa20.
    rmw(sPmu, 0xa20, 0x2, 0);
    IOSleep(1);
    rmw(sPmu, 0xa20, 0x1, 0x1);
    IOSleep(1);
    e5_mali_stage = 2;

    // 2. GPU DCDC enable: PMIC 0x201c bit3 -> 0 (0 = enabled).
    e5_pmic_rmw(0x201c, 0x8, 0x0);
    IOSleep(1);
    e5_mali_stage = 3;

    // 3. Leave forced shutdown: 0x2c4 bit25 -> 0.
    rmw(sPmu, 0x2c4, (0x1u << 25), 0);
    e5_mali_stage = 4;

    // 4. GPU soft reset SKIPPED (v0.2): the reset-controller sub-region at
    // 0x64910000+0xb98 data-aborts while 0x2c4/0xa20 in the same window are
    // fine — that sub-block carries its own clock/power gate (r177 panic at
    // E5Mali.cpp:96). A freshly powered block tolerates no explicit reset;
    // revisit with the pmu_apb clock gate if the ID readback misbehaves.
    e5_mali_stage = 5;

    // v0.6: island is up — NOW the GPU-side windows become readable.
    if (!mapWindow(0x23000000, 0x3000, &sApb, &sApbMap))   { e5_mali_stage = 11; return; }
    if (!mapWindow(0x23014000, 0x3000, &sDvfs, &sDvfsMap)) { e5_mali_stage = 12; return; }
    if (!mapWindow(0x23140000, 0x4000, &sMmio, &sMmioMap)) { e5_mali_stage = 10; return; }

    // 5. GPU clock controller (syscon@23000000, opened by gpu_eb): per
    // ums512 gpu section — core gate+clk 0x4, mem 0x8, sys 0xc; mux bit[8:10]
    // parent 1 = twpll_384m, div bit[4:6] = 0.
    printf("E5MALI step5 gpu-clk w0=%08x\n", sApb[0]);
    sApb[0x4 / 4] = (1u << 0) | (1u << 8);   // gpu_core: gate on, mux=384m
    sApb[0x8 / 4] = (1u << 0) | (1u << 8);   // gpu_mem: gate on, mux=384m
    sApb[0xc / 4] = (1u << 0);               // gpu_sys: gate on, div 1/1
    IOSleep(1);
    e5_mali_stage = 6;

    // 6. Wait for the top state to report up: 0x500 mask 0x1f0000 == 0.
    bool up = false;
    for (int i = 0; i < 200; i++) {
        uint32_t v = sPmu[0x500 / 4];
        if ((v & 0x1f0000u) == 0) { up = true; break; }
        IODelay(50);
    }
    if (!up) { e5_mali_stage = 60; return; }
    e5_mali_stage = 7;

    // 7. Hardware DVFS: handshakes on, then index 3 (384 MHz per plan).
    rmw(sDvfs, 0x9ac, 0x3, 0x1);
    rmw(sDvfs, 0x794, 0x7, 0x3);
    e5_mali_stage = 8;

    // 8. Identity + capability readback (single power-up snapshot).
    e5_mali_gpu_id = sMmio[0x00 / 4];
    e5_mali_shader = sMmio[0x100 / 4];
    e5_mali_stage = 100;

    printf("E5MALI stage %u id %08x shader %08x pmic201c %08x\n",
           e5_mali_stage, e5_mali_gpu_id, e5_mali_shader,
           e5_pmic_peek(0x201c));
    // Capability snapshot for the G0 matrix (panfrost_regs.h offsets):
    // L2 0x04 TILER 0x08 MEM 0x0C MMU 0x10; AS_{TRANSTAB,MEMATTR} 0x2000+;
    // JOBJS 0x140.. shader-ready 0x140 present-lo 0x100.
    printf("E5MALI CAP l2 %08x tiler %08x mem %08x mmu %08x as %08x:%08x js %08x\n",
           sMmio[0x04 / 4], sMmio[0x08 / 4], sMmio[0x0c / 4], sMmio[0x10 / 4],
           sMmio[(0x2000 + 0x08) / 4], sMmio[(0x2000 + 0x10) / 4],
           sMmio[0x140 / 4]);
}
