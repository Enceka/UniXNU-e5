// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included by serial_console.c. USB has no dependency on UEFI services.
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#define E5_USB_NATIVE 1
#define E5_USB_PRODUCT "E5 XNU log"
#define E5_USB_SERIAL "E5XNU0001"
#include "E5UsbCdc.h"

void e5_usb_native_putc(char Byte);
bool e5_usb_native_active(void);
void e5_usb_native_drain(void);
EFI_STATUS E5UsbPlatformInit(void);

#define E5_USB_RING_BYTES (1024U * 1024U)
static UINT8 e5_usb_ring[E5_USB_RING_BYTES];
static UINT64 e5_usb_written, e5_usb_cursor, e5_usb_pending_cursor;
static UINT64 e5_usb_dropped, e5_usb_delivered, e5_usb_frequency;
static UINTN e5_usb_pending_length;
static volatile UINT32 e5_usb_lock;
static int e5_usb_phase; // 0: early RAM only, 1: mapped, -1: disabled/failed
static bool e5_usb_configured, e5_usb_need_zlp, e5_usb_pending_marker;
static bool e5_usb_overrun;
static UINT32 e5_usb_epoch;
static UINT16 e5_usb_tx_csr;
static E5_USB_CDC e5_usb_cdc;
static const UINT8 e5_usb_lost[] = "\r\n[XNUUSB: log bytes lost]\r\n";

static struct {
  UINTN physical, bytes;
  vm_offset_t virtual;
} e5_usb_maps[] = {
  {0x64A00000, 0x2000, 0},
  {0x64900000, 0x1000, 0},
  {0x64910000, 0x3000, 0},
  {0x64300000, 0x1000, 0},
  {0x64400000, 0x30000, 0},
};

static UINT64 e5_usb_ticks(void) {
  UINT64 Value;
  __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(Value));
  return Value;
}

static UINTN e5_usb_address(UINTN Address) {
  for (UINTN I = 0; I < sizeof(e5_usb_maps) / sizeof(e5_usb_maps[0]); I++) {
    if (Address >= e5_usb_maps[I].physical &&
        Address - e5_usb_maps[I].physical < e5_usb_maps[I].bytes) {
      return e5_usb_maps[I].virtual + Address - e5_usb_maps[I].physical;
    }
  }
  __builtin_trap(); // programming error; never access an unowned MMIO address
}
static UINT8 MmioRead8(UINTN Address) { return *(volatile UINT8 *)e5_usb_address(Address); }
static UINT16 MmioRead16(UINTN Address) { return *(volatile UINT16 *)e5_usb_address(Address); }
static UINT32 MmioRead32(UINTN Address) { return *(volatile UINT32 *)e5_usb_address(Address); }
static void MmioWrite8(UINTN Address, UINT8 Value) { *(volatile UINT8 *)e5_usb_address(Address) = Value; }
static void MmioWrite16(UINTN Address, UINT16 Value) { *(volatile UINT16 *)e5_usb_address(Address) = Value; }
static void MmioWrite32(UINTN Address, UINT32 Value) { *(volatile UINT32 *)e5_usb_address(Address) = Value; }
static void MmioOr32(UINTN Address, UINT32 Value) { MmioWrite32(Address, MmioRead32(Address) | Value); }
static void MmioAnd32(UINTN Address, UINT32 Value) { MmioWrite32(Address, MmioRead32(Address) & Value); }
static void MmioAndThenOr32(UINTN Address, UINT32 Mask, UINT32 Value) {
  MmioWrite32(Address, (MmioRead32(Address) & Mask) | Value);
}
static void MicroSecondDelay(UINTN Microseconds) {
  UINT64 Start = e5_usb_ticks();
  UINT64 Delta = (e5_usb_frequency * Microseconds + 999999) / 1000000;
  while (e5_usb_ticks() - Start < Delta) { __asm__ volatile ("yield"); }
}
UINT8 E5UsbRead8(UINTN Register) { return MmioRead8(E5_USB_BASE + Register); }
UINT16 E5UsbRead16(UINTN Register) { return MmioRead16(E5_USB_BASE + Register); }
void E5UsbWrite8(UINTN Register, UINT8 Value) { MmioWrite8(E5_USB_BASE + Register, Value); }
void E5UsbWrite16(UINTN Register, UINT16 Value) { MmioWrite16(E5_USB_BASE + Register, Value); }

// Exported with this patch from e5-xnu's host-tested firmware core. The device
// repository has standalone copies; sync-e5-usb-kernel.py checks their hashes.
#include "E5UsbCdc.c"
#include "E5UsbPlatform.c"

static void e5_usb_append(UINT8 Byte) {
  e5_usb_ring[e5_usb_written++ & (E5_USB_RING_BYTES - 1)] = Byte;
}

static void e5_usb_service_locked(void) {
  UINT8 Packet[E5_USB_PACKET];
  UINTN Length;
  UINT64 Oldest;
  E5UsbCdcPoll(&e5_usb_cdc);
  Oldest = e5_usb_written > E5_USB_RING_BYTES ? e5_usb_written - E5_USB_RING_BYTES : 0;
  if (!e5_usb_cdc.Configuration) {
    e5_usb_configured = false; e5_usb_need_zlp = false;
    e5_usb_pending_length = 0;
    return;
  }
  if (!e5_usb_configured) {
    e5_usb_cursor = Oldest; e5_usb_configured = true; e5_usb_overrun = Oldest != 0;
  }
  if (e5_usb_epoch != e5_usb_cdc.Epoch) {
    e5_usb_pending_length = 0; e5_usb_pending_marker = false;
    e5_usb_epoch = e5_usb_cdc.Epoch;
  }
  if (e5_usb_cdc.TxCompleted) {
    if (e5_usb_pending_marker) {
      e5_usb_overrun = false; e5_usb_pending_marker = false;
    } else {
      e5_usb_cursor = e5_usb_pending_cursor + e5_usb_pending_length;
      e5_usb_delivered += e5_usb_pending_length;
      e5_usb_need_zlp = e5_usb_pending_length == E5_USB_PACKET;
    }
    e5_usb_cdc.TxCompleted = false;
    e5_usb_pending_length = 0;
  }
  if (e5_usb_cdc.TxPending) { return; }
  if (e5_usb_cursor < Oldest) {
    e5_usb_dropped += Oldest - e5_usb_cursor;
    e5_usb_cursor = Oldest; e5_usb_overrun = true;
  }
  if (e5_usb_overrun) {
    if (E5UsbCdcSend(&e5_usb_cdc, e5_usb_lost, sizeof(e5_usb_lost) - 1)) {
      e5_usb_pending_marker = true;
    }
    return;
  }
  Length = (UINTN)(e5_usb_written - e5_usb_cursor);
  if (Length > sizeof(Packet)) { Length = sizeof(Packet); }
  if (Length || e5_usb_need_zlp) {
    for (UINTN I = 0; I < Length; I++) {
      Packet[I] = e5_usb_ring[(e5_usb_cursor + I) & (E5_USB_RING_BYTES - 1)];
    }
    if (E5UsbCdcSend(&e5_usb_cdc, Packet, Length)) {
      e5_usb_pending_cursor = e5_usb_cursor; e5_usb_pending_length = Length;
    }
  }
}

static void e5_usb_native_early_init(void) {
  e5_usb_phase = 0; e5_usb_written = 0;
}

void e5_usb_native_putc(char Byte) {
  boolean_t Interrupts;
  if (e5_usb_phase < 0) { return; }
  // Before arm_vm_init no exclusive/atomic operations or MMIO are allowed.
  if (e5_usb_phase == 0) { e5_usb_append((UINT8)Byte); return; }
  Interrupts = ml_set_interrupts_enabled(FALSE);
  if (__atomic_exchange_n(&e5_usb_lock, 1, __ATOMIC_ACQUIRE)) {
    __atomic_fetch_add(&e5_usb_dropped, 1, __ATOMIC_RELAXED);
    ml_set_interrupts_enabled(Interrupts);
    return;
  }
  e5_usb_append((UINT8)Byte);
  // Service enumeration before the scheduler/worker exists. One bounded pass
  // only: an absent/slow host cannot block a console producer.
  if (Byte == '\n' || !(e5_usb_written & 15)) { e5_usb_service_locked(); }
  __atomic_store_n(&e5_usb_lock, 0, __ATOMIC_RELEASE);
  ml_set_interrupts_enabled(Interrupts);
}

bool e5_usb_native_active(void) { return e5_usb_phase == 1; }

static void e5_usb_native_map(void) {
  int Enabled = 0;
  extern void e5_trace_tag(const char *);
  if (!PE_parse_boot_argn("e5-usb", &Enabled, sizeof(Enabled)) || !Enabled) {
    e5_usb_phase = -1; return;
  }
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(e5_usb_frequency));
  if (!e5_usb_frequency) { e5_usb_phase = -1; return; }
  for (UINTN I = 0; I < sizeof(e5_usb_maps) / sizeof(e5_usb_maps[0]); I++) {
    e5_usb_maps[I].virtual = ml_io_map(e5_usb_maps[I].physical, e5_usb_maps[I].bytes);
    if (!e5_usb_maps[I].virtual) {
      e5_usb_phase = -1; e5_trace_tag("XNUUSB: MMIO map failed\n"); return;
    }
  }
  if (E5UsbPlatformInit() != EFI_SUCCESS) {
    e5_usb_phase = -1; e5_trace_tag("XNUUSB: PMIC/PHY init failed\n"); return;
  }
  E5UsbCdcInit(&e5_usb_cdc);
  __atomic_store_n(&e5_usb_phase, 1, __ATOMIC_RELEASE);
  e5_trace_tag("XNUUSB: native MUSB CDC initialized, replaying early kernel log\n");
}

static void e5_usb_native_poll(void) {
  boolean_t Interrupts;
  if (e5_usb_phase != 1) { return; }
  Interrupts = ml_set_interrupts_enabled(FALSE);
  if (!__atomic_exchange_n(&e5_usb_lock, 1, __ATOMIC_ACQUIRE)) {
    e5_usb_service_locked();
    E5UsbWrite8(0x0E, 2);
    e5_usb_tx_csr = E5UsbRead16(0x12);
    E5UsbWrite8(0x0E, 0);
    __atomic_store_n(&e5_usb_lock, 0, __ATOMIC_RELEASE);
  }
  ml_set_interrupts_enabled(Interrupts);
}

void e5_usb_native_drain(void) {
  UINT64 Start;
  if (e5_usb_phase != 1) { return; }
  Start = e5_usb_ticks();
  while (e5_usb_ticks() - Start < e5_usb_frequency / 4) { e5_usb_native_poll(); }
}

static __attribute__((noreturn)) void e5_usb_worker(void *Argument, wait_result_t Result) {
  UINT64 Last = e5_usb_ticks();
  (void)Argument; (void)Result;
  for (;;) {
    e5_usb_native_poll();
    if (e5_usb_ticks() - Last >= e5_usb_frequency * 5) {
      char Status[256];
      extern void e5_trace_tag(const char *);
      Last = e5_usb_ticks();
      snprintf(Status, sizeof(Status), "XNUUSB: heartbeat written=%llu sent=%llu dropped=%llu cfg=%u dtr=%u suspend=%u phase=%u pending=%u tx=%x reset=%u setup=%u\n",
               (unsigned long long)e5_usb_written, (unsigned long long)e5_usb_delivered,
               (unsigned long long)e5_usb_dropped, e5_usb_cdc.Configuration,
               e5_usb_cdc.ControlLines, e5_usb_cdc.Suspended, e5_usb_cdc.Phase,
               e5_usb_cdc.TxPending, e5_usb_tx_csr, e5_usb_cdc.Resets, e5_usb_cdc.Setups);
      // Keep transport state observable through the independent RAM/eMMC tee
      // even when the USB path being diagnosed has stopped delivering bytes.
      e5_trace_tag(Status);
    }
    assert_wait_timeout((event_t)&e5_usb_cdc, THREAD_UNINT, 1, 1000000);
    thread_block(THREAD_CONTINUE_NULL);
  }
}

static void e5_usb_start_worker(const void *Argument) {
  thread_t Thread;
  extern void e5_trace_tag(const char *);
  (void)Argument;
  if (e5_usb_phase != 1) { return; }
  if (kernel_thread_start(e5_usb_worker, NULL, &Thread) == KERN_SUCCESS) {
    thread_set_thread_name(Thread, "e5_usb_log");
    thread_deallocate(Thread);
    e5_trace_tag("XNUUSB: background polling thread started\n");
  } else { e5_trace_tag("XNUUSB: background thread failed\n"); }
}
STARTUP_ARG(THREAD_CALL, STARTUP_RANK_LAST, e5_usb_start_worker, NULL);
