// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register facts: E5 live-fdt.dts and e5-linux's ums9621 clock, Qogirn6lite
// PHY, UMP9620 regulator and UMS9620 ADI drivers. Preserve unrelated bits.
#ifndef E5_USB_NATIVE
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#endif

#define AON     0x64900000
#define PMU     0x64910000
#define ANALOG  0x64300000
#define ADI     0x64400000

STATIC BOOLEAN
PmicRead (UINT32 Register, UINT16 *Value)
{
  UINTN Attempt;
  UINT32 Data;
  MmioWrite32 (ADI + 0x28, Register);
  for (Attempt = 0; Attempt < 2000; Attempt++) {
    Data = MmioRead32 (ADI + 0x2C);
    if ((Data & 0x80000000U) == 0) {
      // UMS9620/r5p0 ADI has no returned-address check (unlike r2/r3).
      *Value = (UINT16)Data;
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  return FALSE;
}

STATIC BOOLEAN
PmicWrite (UINT32 Register, UINT16 Value)
{
  UINTN Attempt;
  UINT16 Verify;
  for (Attempt = 0; Attempt < 2000; Attempt++) {
    if ((MmioRead32 (ADI + 0x30) & 0xC00) == 0x400) { break; }
    MicroSecondDelay (1);
  }
  if (Attempt == 2000) { return FALSE; }
  // 17-bit slave addressing: the virtual write aperture starts at +0x20000.
  MmioWrite32 (ADI + 0x20000 + Register, Value);
  for (Attempt = 0; Attempt < 2000; Attempt++) {
    if ((MmioRead32 (ADI + 0x30) & 0x80000000U) == 0) { break; }
    MicroSecondDelay (1);
  }
  // PWR_WR_PROT contains a read-only protection-status bit as well as the
  // write key. Verify the actual voltage/power/routing registers below.
  return (Attempt != 2000) && ((Register == 0x23D0) ||
         (PmicRead (Register, &Verify) && (Verify == Value)));
}

STATIC BOOLEAN
PmicUpdate (UINT32 Register, UINT16 Mask, UINT16 Bits)
{
  UINT16 Value;
  if (!PmicRead (Register, &Value)) { return FALSE; }
  if ((Value & Mask) == Bits) { return TRUE; }
  return PmicWrite (Register, (UINT16)((Value & ~Mask) | Bits));
}

EFI_STATUS
E5UsbPlatformInit (VOID)
{
  UINTN Channel;
  // ADI and its clocks are already running: LK uses this PMIC for power and
  // reset. Do not reconfigure the shared controller or its hardware channels.
  // The stock DT has no ADI hwspinlock. Each transaction has a bounded wait.
  if (!PmicWrite (0x23D0, 0x6E7F) ||
      !PmicUpdate (0x21EC, 0xFF, 0xD2) || // USB33 = stock 3.3 V
      !PmicUpdate (0x21E8, 1, 0) ||        // USB33 power-down off
      !PmicUpdate (0x23A0, 0x4001, 0x4001)) { // route DP/DM to USB PHY
    return EFI_DEVICE_ERROR;
  }
  // USB2's reference comes from USB31PLLV, even on this USB2-only board.
  // The missing force-on bit also broke enumeration during Linux bring-up.
  MmioOr32 (PMU + 0xA58, 1);
  MmioOr32 (AON + 0x04, 0x300);   // USB2 top + PHY gates
  MmioOr32 (AON + 0x138, 0x1400); // OTG + DPHY reference gates
  MmioAnd32 (AON + 0x350, ~3U);   // PHY PD_L / PD_S
  MmioAnd32 (ANALOG + 0x1C, ~1U); // analog isolation
  MmioAnd32 (AON + 0x190, ~0x18U); // AON controller mux, isolation
  MmioOr32 (AON + 0x204, 0x01000000); // VBUS valid
  MmioAndThenOr32 (ANALOG + 0x04, ~0xFFFFU, 0x10010000);
  MmioOr32 (AON + 0x208, 0x40000008); // 16-bit UTMI, peripheral ID
  MmioOr32 (ANALOG + 0x20, 0x0C); // software controls DP/DM pull-downs
  MmioAnd32 (ANALOG + 0x0C, ~0x18U); // peripheral: no host pull-downs
  MmioWrite32 (ANALOG + 0x10, 0x067BD1C0); // E5 DT device eye pattern
  MmioOr32 (AON + 0x10, 0x300);
  MicroSecondDelay (20000); // vendor PHY requires 20..30 ms reset
  MmioAnd32 (AON + 0x10, ~0x300U);
  MicroSecondDelay (2000);
  // Quiesce any LK DMA descriptors before we take ownership of the FIFOs.
  for (Channel = 0; Channel < 30; Channel++) {
    MmioAnd32 (0x64A01C04 + Channel * 0x20, ~1U);
  }
  return EFI_SUCCESS;
}
