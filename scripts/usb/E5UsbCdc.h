// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef E5_USB_CDC_H_
#define E5_USB_CDC_H_
#ifdef E5_USB_NATIVE
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef bool BOOLEAN;
typedef char CHAR8;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 7
#define STATIC static
#define CONST const
#define VOID void
#ifndef TRUE
#define TRUE true
#define FALSE false
#endif
#else
#include <Uefi.h>
#endif

#ifndef E5_USB_PRODUCT
#define E5_USB_PRODUCT "E5 UEFI log"
#endif
#ifndef E5_USB_SERIAL
#define E5_USB_SERIAL "E5UEFI0001"
#endif

#define E5_USB_BASE  0x64A00000
#define E5_USB_PACKET 64

// Kept separate from the DXE lifecycle so the actual EP0 and FIFO code can be
// exercised with a host-side register model.
UINT8  E5UsbRead8 (UINTN Register);
UINT16 E5UsbRead16 (UINTN Register);
VOID   E5UsbWrite8 (UINTN Register, UINT8 Value);
VOID   E5UsbWrite16 (UINTN Register, UINT16 Value);

typedef struct {
  UINT8   Configuration;
  UINT8   ControlLines;
  UINT8   Phase;
  UINT8   Action;
  UINT16  Value;
  UINT16  ReplyLength;
  UINT16  ReplyPosition;
  BOOLEAN ReplyZlp;
  UINT8   Reply[128];
  UINT8   LineCoding[7];
  BOOLEAN TxPending;
  BOOLEAN TxCompleted;
  BOOLEAN Notify;
  BOOLEAN Suspended;
  UINT32  Epoch;
  UINT32  Resets;
  UINT32  Setups;
} E5_USB_CDC;

VOID E5UsbCdcInit (E5_USB_CDC *Cdc);
VOID E5UsbCdcPoll (E5_USB_CDC *Cdc);
BOOLEAN E5UsbCdcSend (E5_USB_CDC *Cdc, CONST UINT8 *Data, UINTN Length);
VOID E5UsbCdcDisconnect (VOID);
#endif
