// SPDX-License-Identifier: BSD-2-Clause-Patent
// Polling, full-speed CDC ACM for the E5's Mentor/Unisoc MUSB controller.
// No DMA, IRQ handler, allocation, waiting for a host, or DebugLib calls.
#include "E5UsbCdc.h"

#define FADDR       0x00
#define POWER       0x01
#define INTRTX      0x02
#define INTRRX      0x04
#define INTRTXE     0x06
#define INTRRXE     0x08
#define INTRUSB     0x0A
#define INTRUSBE    0x0B
#define INDEX       0x0E
#define TXMAXP      0x10
#define CSR0        0x12
#define TXCSR       0x12
#define RXMAXP      0x14
#define RXCSR       0x16
#define RXCOUNT     0x18
#define E5_USB_FIFO(Ep)    (0x20 + 4 * (Ep))
#define TXFIFOSZ    0x62
#define RXFIFOSZ    0x63
#define TXFIFOADD   0x64
#define RXFIFOADD   0x66

#define RX_READY    0x01
#define TX_READY    0x02
#define SENT_STALL  0x04
#define DATA_END    0x08
#define SETUP_END   0x10
#define SEND_STALL  0x20
#define ACK_RX      0x40
#define ACK_SETUP   0x80

enum { Setup, DataIn, LineOut, StatusIn, StatusOut };
enum { NoAction, Address, Configuration, ControlLines, LineCoding };

STATIC CONST UINT8 mDevice[] = {
  18, 1, 0, 2, 0xEF, 2, 1, 64, 0x25, 0x05, 0xA7, 0xA4,
  0, 1, 1, 2, 3, 1
};
STATIC CONST UINT8 mConfiguration[] = {
  9, 2, 75, 0, 2, 1, 0, 0x80, 50,
  8, 11, 0, 2, 2, 2, 1, 0,
  9, 4, 0, 0, 1, 2, 2, 1, 0,
  5, 0x24, 0, 0x10, 1,
  5, 0x24, 1, 0, 1,
  4, 0x24, 2, 2,
  5, 0x24, 6, 0, 1,
  7, 5, 0x81, 3, 16, 0, 16,
  9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
  7, 5, 0x02, 2, 64, 0, 0,
  7, 5, 0x82, 2, 64, 0, 0
};

STATIC UINT16
Le16 (CONST UINT8 *Bytes)
{
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8));
}

STATIC VOID
ReadFifo (UINT8 Ep, UINT8 *Data, UINTN Length)
{
  while (Length-- != 0) { *Data++ = E5UsbRead8 (E5_USB_FIFO (Ep)); }
}

STATIC VOID
WriteFifo (UINT8 Ep, CONST UINT8 *Data, UINTN Length)
{
  while (Length-- != 0) { E5UsbWrite8 (E5_USB_FIFO (Ep), *Data++); }
}

STATIC VOID
Endpoints (E5_USB_CDC *Cdc)
{
  // E5_USB_FIFO addresses are in units of eight bytes. EP0 owns bytes 0..63.
  // EP1 IN: 64..79, EP2 IN: 80..143, EP2 OUT: 144..207.
  E5UsbWrite8 (INDEX, 1);
  E5UsbWrite8 (TXFIFOSZ, 1);
  E5UsbWrite16 (TXFIFOADD, 8);
  E5UsbWrite16 (TXMAXP, 16);
  E5UsbWrite16 (TXCSR, 0x2048); // TX mode, flush, clear data toggle
  E5UsbWrite16 (TXCSR, 0x2048);
  E5UsbWrite8 (INDEX, 2);
  E5UsbWrite8 (TXFIFOSZ, 3);
  E5UsbWrite16 (TXFIFOADD, 10);
  E5UsbWrite8 (RXFIFOSZ, 3);
  E5UsbWrite16 (RXFIFOADD, 18);
  E5UsbWrite16 (TXMAXP, E5_USB_PACKET);
  E5UsbWrite16 (RXMAXP, E5_USB_PACKET);
  E5UsbWrite16 (TXCSR, 0x2048);
  E5UsbWrite16 (TXCSR, 0x2048);
  E5UsbWrite16 (RXCSR, 0x0090); // flush, clear RX toggle, DMA disabled
  E5UsbWrite16 (RXCSR, 0x0090);
  Cdc->TxPending = FALSE;
  Cdc->TxCompleted = FALSE;
  Cdc->Epoch++;
  E5UsbWrite8 (INDEX, 0);
}

STATIC VOID
Reset (E5_USB_CDC *Cdc)
{
  Cdc->Configuration = 0;
  Cdc->ControlLines = 0;
  Cdc->Phase = Setup;
  Cdc->Action = NoAction;
  Cdc->Notify = FALSE;
  Cdc->Suspended = FALSE;
  E5UsbWrite8 (FADDR, 0);
  Endpoints (Cdc);
}

VOID
E5UsbCdcDisconnect (VOID)
{
  E5UsbWrite8 (POWER, 0);
  E5UsbWrite16 (INTRTXE, 0);
  E5UsbWrite16 (INTRRXE, 0);
  E5UsbWrite8 (INTRUSBE, 0);
}

VOID
E5UsbCdcInit (E5_USB_CDC *Cdc)
{
  UINTN Index;
  for (Index = 0; Index < sizeof (*Cdc); Index++) { ((UINT8 *)Cdc)[Index] = 0; }
  Cdc->LineCoding[0] = 0x00;
  Cdc->LineCoding[1] = 0xC2; // 115200, 8N1
  Cdc->LineCoding[2] = 0x01;
  Cdc->LineCoding[6] = 8;
  E5UsbCdcDisconnect ();
  E5UsbWrite8 (0x0F, 0); // TESTMODE
  E5UsbWrite8 (0x60, 0); // DEVCTL: peripheral, no host session
  E5UsbWrite8 (0x34B, (UINT8)(E5UsbRead8 (0x34B) & ~0x41));
  Reset (Cdc);
  (VOID)E5UsbRead8 (INTRUSB);
  (VOID)E5UsbRead16 (INTRTX);
  (VOID)E5UsbRead16 (INTRRX);
  // These latch endpoint events. The platform never enables the MUSB GIC SPI;
  // all status is serviced by polling. High-speed negotiation stays disabled.
  E5UsbWrite16 (INTRTXE, 7);
  E5UsbWrite16 (INTRRXE, 4);
  E5UsbWrite8 (INTRUSBE, 0x27); // suspend, resume, reset, disconnect
  E5UsbWrite8 (POWER, 0x40);   // SOFTCONN
}

STATIC VOID
Stall (E5_USB_CDC *Cdc)
{
  E5UsbWrite16 (CSR0, ACK_RX | SEND_STALL);
  Cdc->Phase = Setup;
  Cdc->Action = NoAction;
}

STATIC VOID
NextReply (E5_USB_CDC *Cdc, UINT16 Ack)
{
  UINTN Length = Cdc->ReplyLength - Cdc->ReplyPosition;
  BOOLEAN Last;
  if (Length > E5_USB_PACKET) { Length = E5_USB_PACKET; }
  WriteFifo (0, Cdc->Reply + Cdc->ReplyPosition, Length);
  Cdc->ReplyPosition += (UINT16)Length;
  Last = (Cdc->ReplyPosition == Cdc->ReplyLength) &&
         (!Cdc->ReplyZlp || (Length == 0));
  Cdc->Phase = Last ? StatusOut : DataIn;
  E5UsbWrite16 (CSR0, (UINT16)(Ack | TX_READY | (Last ? DATA_END : 0)));
}

STATIC VOID
Reply (E5_USB_CDC *Cdc, CONST UINT8 *Data, UINTN Length, UINT16 Requested)
{
  UINTN Index;
  if (Length > Requested) { Length = Requested; }
  for (Index = 0; Index < Length; Index++) { Cdc->Reply[Index] = Data[Index]; }
  Cdc->ReplyPosition = 0;
  Cdc->ReplyLength = (UINT16)Length;
  Cdc->ReplyZlp = (Length < Requested) && ((Length % E5_USB_PACKET) == 0);
  // MUSB's EP0 FIFO changes direction only after the SETUP RX packet is
  // serviced. Writing the reply first leaves it in the RX FIFO on Unisoc.
  // This is musb_read_setup()'s IN-direction transition in the E5 kernel.
  E5UsbWrite16 (CSR0, ACK_RX);
  for (Index = 0; Index < 64; Index++) {
    if (!(E5UsbRead16 (CSR0) & RX_READY)) { NextReply (Cdc, 0); return; }
  }
  Stall (Cdc); // bounded: a broken controller must never stall firmware boot
}

STATIC BOOLEAN
EndpointValid (E5_USB_CDC *Cdc, UINT16 Ep)
{
  return (Ep == 0) || (Ep == 0x80) ||
         ((Cdc->Configuration != 0) && ((Ep == 0x81) || (Ep == 2) || (Ep == 0x82)));
}

STATIC VOID
SetupPacket (E5_USB_CDC *Cdc)
{
  UINT8 Packet[8], Small[2];
  UINT16 Value, Index, Length;
  UINTN N;
  CONST CHAR8 *String;

  Cdc->Setups++;
  Cdc->Action = NoAction;
  if (E5UsbRead16 (RXCOUNT) != 8) { Stall (Cdc); return; }
  ReadFifo (0, Packet, 8);
  Value = Le16 (Packet + 2);
  Index = Le16 (Packet + 4);
  Length = Le16 (Packet + 6);
  if ((Packet[0] == 0x80) && (Packet[1] == 6) && (Length != 0)) {
    if ((Value == 0x0100) && (Index == 0)) {
      Reply (Cdc, mDevice, sizeof (mDevice), Length); return;
    }
    if ((Value == 0x0200) && (Index == 0)) {
      Reply (Cdc, mConfiguration, sizeof (mConfiguration), Length); return;
    }
    if (Value == 0x0300 && Index == 0) {
      CONST UINT8 Language[] = {4, 3, 9, 4};
      Reply (Cdc, Language, sizeof (Language), Length); return;
    }
    if (((Value >> 8) == 3) && (Index == 0x0409)) {
      switch (Value & 0xFF) {
        case 1: String = "e5-xnu"; break;
        case 2: String = E5_USB_PRODUCT; break;
        case 3: String = E5_USB_SERIAL; break;
        default: Stall (Cdc); return;
      }
      for (N = 0; String[N] != 0; N++) {
        Cdc->Reply[2 + 2 * N] = (UINT8)String[N];
        Cdc->Reply[3 + 2 * N] = 0;
      }
      Cdc->Reply[0] = (UINT8)(2 + 2 * N); Cdc->Reply[1] = 3;
      Reply (Cdc, Cdc->Reply, 2 + 2 * N, Length); return;
    }
  } else if ((Packet[1] == 0) && (Value == 0) && (Length == 2)) {
    // GET_STATUS: device, configured interface, or a valid endpoint.
    Small[0] = Small[1] = 0;
    if ((Packet[0] == 0x80) && (Index == 0)) {
      Reply (Cdc, Small, 2, Length); return;
    }
    if ((Packet[0] == 0x81) && Cdc->Configuration && (Index < 2)) {
      Reply (Cdc, Small, 2, Length); return;
    }
    if ((Packet[0] == 0x82) && EndpointValid (Cdc, Index)) {
      // No endpoint is deliberately halted by this implementation.
      Reply (Cdc, Small, 2, Length); return;
    }
  } else if ((Packet[0] == 0x80) && (Packet[1] == 8) &&
             (Value == 0) && (Index == 0) && (Length == 1)) {
    Reply (Cdc, &Cdc->Configuration, 1, Length); return;
  } else if ((Packet[0] == 0x81) && (Packet[1] == 10) &&
             Cdc->Configuration && (Value == 0) && (Index < 2) && (Length == 1)) {
    Small[0] = 0; Reply (Cdc, Small, 1, Length); return;
  } else if ((Packet[0] == 0xA1) && (Packet[1] == 0x21) &&
             Cdc->Configuration && (Value == 0) && (Index == 0) && (Length == 7)) {
    Reply (Cdc, Cdc->LineCoding, 7, Length); return;
  } else if ((Packet[0] == 0x21) && Cdc->Configuration && (Index == 0)) {
    if ((Packet[1] == 0x20) && (Value == 0) && (Length == 7)) {
      Cdc->Phase = LineOut;
      E5UsbWrite16 (CSR0, ACK_RX); return;
    }
    if ((Packet[1] == 0x22) && (Length == 0) && ((Value & ~3U) == 0)) {
      Cdc->Action = ControlLines;
    } else if ((Packet[1] != 0x23) || (Length != 0)) {
      Stall (Cdc); return;
    }
  } else if (Length == 0) {
    if ((Packet[0] == 0) && (Packet[1] == 5) && (Index == 0) && (Value < 128)) {
      Cdc->Action = Address;
    } else if ((Packet[0] == 0) && (Packet[1] == 9) && (Index == 0) && (Value <= 1)) {
      Cdc->Action = Configuration;
    } else if ((Packet[0] == 1) && (Packet[1] == 11) &&
               Cdc->Configuration && (Index < 2) && (Value == 0)) {
      // Both interfaces have only alternate setting zero.
    } else if ((Packet[0] == 2) && (Packet[1] == 1) && (Value == 0) &&
               (Index != 0) && (Index != 0x80) && EndpointValid (Cdc, Index)) {
      // CLEAR_FEATURE(ENDPOINT_HALT). Cancel unacknowledged data and reset
      // the selected toggle. The log cursor remains uncommitted for retry.
      E5UsbWrite8 (INDEX, (UINT8)(Index & 15));
      if (Index & 0x80) {
        E5UsbWrite16 (TXCSR, 0x2048); E5UsbWrite16 (TXCSR, 0x2048);
      } else {
        E5UsbWrite16 (RXCSR, 0x0090); E5UsbWrite16 (RXCSR, 0x0090);
      }
      if (Index == 0x82) {
        Cdc->TxPending = Cdc->TxCompleted = FALSE; Cdc->Epoch++;
      }
      E5UsbWrite8 (INDEX, 0);
    } else { Stall (Cdc); return; }
  } else { Stall (Cdc); return; }

  // Any unsupported IN request must stall; it must not fall into the
  // no-data status path after descriptor / GET_STATUS dispatch.
  if (Length != 0) { Stall (Cdc); return; }
  Cdc->Value = Value;
  Cdc->Phase = StatusIn;
  E5UsbWrite16 (CSR0, ACK_RX | DATA_END);
}

VOID
E5UsbCdcPoll (E5_USB_CDC *Cdc)
{
  UINT8 Bus;
  UINT16 Tx, Csr, Count;
  UINTN Index;
  UINT8 Notification[] = {0xA1, 0x20, 0, 0, 0, 0, 2, 0, 3, 0};

  Bus = E5UsbRead8 (INTRUSB); // clear on read
  Tx = E5UsbRead16 (INTRTX);
  (VOID)E5UsbRead16 (INTRRX);
  if (Bus & 0x24) { Cdc->Resets++; Reset (Cdc); return; }
  if (Bus & 1) { Cdc->Suspended = TRUE; }
  if (Bus & 2) { Cdc->Suspended = FALSE; }
  E5UsbWrite8 (INDEX, 0);
  Csr = E5UsbRead16 (CSR0);
  if (Csr & SENT_STALL) {
    E5UsbWrite16 (CSR0, 0); Cdc->Phase = Setup; Cdc->Action = NoAction;
  }
  if (Csr & SETUP_END) {
    E5UsbWrite16 (CSR0, ACK_SETUP);
    Cdc->Phase = Setup; Cdc->Action = NoAction;
    Csr = E5UsbRead16 (CSR0);
  }
  if ((Cdc->Phase == StatusIn) && (Tx & 1)) {
    // Address/configuration/control-line changes apply only AFTER the host
    // acknowledges status. A cancelled setup never commits them.
    switch (Cdc->Action) {
      case Address: E5UsbWrite8 (FADDR, (UINT8)Cdc->Value); break;
      case Configuration:
        Cdc->Configuration = (UINT8)Cdc->Value;
        Cdc->ControlLines = 0; Cdc->Notify = FALSE;
        Endpoints (Cdc); break;
      case ControlLines:
        Cdc->ControlLines = (UINT8)Cdc->Value; Cdc->Notify = TRUE; break;
      case LineCoding:
        for (Index = 0; Index < 7; Index++) { Cdc->LineCoding[Index] = Cdc->Reply[Index]; }
        break;
      default: break;
    }
    Cdc->Phase = Setup; Cdc->Action = NoAction;
  } else if ((Cdc->Phase == StatusOut) && (Tx & 1)) {
    Cdc->Phase = Setup;
  } else if ((Cdc->Phase == DataIn) && (Tx & 1) && !(Csr & TX_READY)) {
    NextReply (Cdc, 0);
  }
  if ((Cdc->Phase == LineOut) && (Csr & RX_READY)) {
    Count = E5UsbRead16 (RXCOUNT);
    if (Count != 7) { Stall (Cdc); }
    else {
      ReadFifo (0, Cdc->Reply, 7);
      if ((Cdc->Reply[4] > 2) || (Cdc->Reply[5] > 4) ||
          ((Cdc->Reply[6] != 5) && (Cdc->Reply[6] != 6) &&
           (Cdc->Reply[6] != 7) && (Cdc->Reply[6] != 8) && (Cdc->Reply[6] != 16))) {
        Stall (Cdc);
      } else {
        Cdc->Action = LineCoding; Cdc->Phase = StatusIn;
        E5UsbWrite16 (CSR0, ACK_RX | DATA_END);
      }
    }
  } else if ((Cdc->Phase == Setup) && (Csr & RX_READY)) { SetupPacket (Cdc); }

  if (!Cdc->Configuration || Cdc->Suspended) { return; }
  E5UsbWrite8 (INDEX, 2);
  Csr = E5UsbRead16 (TXCSR);
  if (Cdc->TxPending) {
    if (Csr & 0x20) { // SENTSTALL: never count as delivered
      E5UsbWrite16 (TXCSR, 0x2048); E5UsbWrite16 (TXCSR, 0x2048);
      Cdc->TxPending = FALSE; Cdc->Epoch++;
    } else {
      // PIO UNDERRUN means the host polled an empty FIFO and received NAK.
      // It can occur AFTER the queued packet was ACKed; retrying that packet
      // duplicates console bytes and prevents the final short/ZLP boundary.
      // Match musb_g_tx(): clear the NAK indication, then account completion.
      if (Csr & 4) { E5UsbWrite16 (TXCSR, (UINT16)(Csr & ~4U)); }
      if ((Csr & 3) == 0) {
        Cdc->TxPending = FALSE; Cdc->TxCompleted = TRUE;
      }
    }
  }
  // Logging is output-only. Drain host bulk OUT so writes cannot wedge the
  // ACM port; no reboot or other command is interpreted here.
  if (E5UsbRead16 (RXCSR) & 1) {
    Count = E5UsbRead16 (RXCOUNT);
    if (Count > E5_USB_PACKET) { Count = E5_USB_PACKET; }
    while (Count-- != 0) { (VOID)E5UsbRead8 (E5_USB_FIFO (2)); }
    E5UsbWrite16 (RXCSR, 0);
  }
  E5UsbWrite8 (INDEX, 1);
  if (Cdc->Notify && ((E5UsbRead16 (TXCSR) & 3) == 0)) {
    Notification[8] = (Cdc->ControlLines & 1) ? 3 : 0;
    WriteFifo (1, Notification, sizeof (Notification));
    E5UsbWrite16 (TXCSR, 0x2001);
    Cdc->Notify = FALSE;
  }
  E5UsbWrite8 (INDEX, 0);
}

BOOLEAN
E5UsbCdcSend (E5_USB_CDC *Cdc, CONST UINT8 *Data, UINTN Length)
{
  if (!Cdc->Configuration || !(Cdc->ControlLines & 1) || Cdc->Suspended ||
      Cdc->TxPending || (Length > E5_USB_PACKET)) { return FALSE; }
  E5UsbWrite8 (INDEX, 2);
  if (E5UsbRead16 (TXCSR) & 3) { E5UsbWrite8 (INDEX, 0); return FALSE; }
  WriteFifo (2, Data, Length);
  Cdc->TxPending = TRUE;
  Cdc->TxCompleted = FALSE;
  E5UsbWrite16 (TXCSR, 0x2001);
  E5UsbWrite8 (INDEX, 0);
  return TRUE;
}
