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

static bool mapWindow(uint64_t pa, uint32_t len, volatile uint32_t **out)
{
    IODeviceMemory *mem = IODeviceMemory::withRange(pa, len);
    if (!mem) return false;
    IOMemoryMap *map = mem->map();
    if (!map) { mem->release(); return false; }
    *out = (volatile uint32_t *)map->getVirtualAddress();
    // keep both alive for the life of the probe window
    return true;
}

static void rmw(volatile uint32_t *base, uint32_t reg, uint32_t mask, uint32_t val)
{
    uint32_t v = base[reg / 4];
    v = (v & ~mask) | (val & mask);
    base[reg / 4] = v;
}

void e5_mali_probe(void)
{
    if (!mapWindow(0x23140000, 0x4000, &sMmio))  { e5_mali_stage = 10; return; }
    if (!mapWindow(0x23000000, 0x3000, &sApb))   { e5_mali_stage = 11; return; }
    if (!mapWindow(0x23014000, 0x3000, &sDvfs))  { e5_mali_stage = 12; return; }
    if (!mapWindow(0x64910000, 0x3000, &sPmu))   { e5_mali_stage = 13; return; }
    e5_mali_stage = 1;

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

    // 4. GPU soft reset: assert 0xb98 bit2, 10us, deassert.
    rmw(sPmu, 0x0b98, 0x4, 0x4);
    IODelay(10);
    rmw(sPmu, 0x0b98, 0x4, 0x0);
    IODelay(10);
    e5_mali_stage = 5;

    // 5. Core clock gate: APB 0x0 bit2 -> 1.
    rmw(sApb, 0x0, 0x4, 0x4);
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

    // 8. Identity readback.
    e5_mali_gpu_id = sMmio[0x00 / 4];
    e5_mali_shader = sMmio[0x100 / 4];
    e5_mali_stage = 100;

    printf("E5MALI stage %u id %08x shader %08x pmic201c %08x\n",
           e5_mali_stage, e5_mali_gpu_id, e5_mali_shader,
           e5_pmic_peek(0x201c));
}
