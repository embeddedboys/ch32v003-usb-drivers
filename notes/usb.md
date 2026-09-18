# What software USB on this chip costs

The device is a **low speed** (1.5 Mbit/s) USB device whose USB stack is bit
banged on a 48 MHz RISC-V core. Everything below is a consequence of that one
sentence, and most of the surprising behaviour of this project is a consequence
of these numbers.

## Why bit banging USB works at all on this part

Three things have to line up, and on a CH32V003 they do:

**1. A low speed bit is 32 CPU cycles.**  Low speed is 1.5 Mbit/s, so one bit
time is 667 ns; the core runs at 48 MHz, i.e. 32 cycles per bit.  That is a lot
of margin for code that has to sample a level and shift it into a byte (V-USB on
a 12 MHz AVR managed with 8 cycles per bit).  Full speed would be 12 Mbit/s =
83 ns = **4 cycles per bit**, which no byte-wise software stack can do, so the
device is low speed or nothing.

**2. The clock is accurate enough for low speed, but would not be for full
speed.**  USB allows a low speed device +-1.5% and a full speed one +-0.25%
(which in practice means a crystal).  This board runs from the internal 24 MHz
RC oscillator times the PLL by 2 (`HSI_VALUE`, `RCC_PLLSRC_HSI_Mul2`,
`RCC_CTLR |= RCC_HSION | RCC_PLLON`), factory trimmed, no crystal anywhere.  That
is the reason this can be a 1 US$ part with a USB connector.

**3. The hardware needed is two GPIOs and one edge interrupt.**  The electrical
side of low speed is plain 3.3 V push/pull: PD3 (D+) and PD4 (D-) are inputs,
PD5 (DPU) is driven high to put the board's 1.5 kOhm pull-up on D- and tell the
host a low speed device arrived.  Receiving is done by reading the pins,
transmitting by writing them.

The clever part is how the device knows a packet started, and the code says it
in three lines (`usb_setup()` in rv003usb.c):

```c
AFIO->EXTICR = ...USB_PIN_DM...;   /* route D- to EXTI            */
EXTI->FTENR  = 1 << USB_PIN_DM;    /* falling edge                */
NVIC_EnableIRQ( EXTI7_0_IRQn );
```

Low speed idles in the J state (D- high) and every packet opens with the SYNC
pattern, whose first edge pulls D- low - so **a falling edge on D- is "a packet
is starting"**.  No bus polling, no timer: the rest of the packet is decoded by
counting CPU cycles (`se0_cyccount`, `last_se0_cyccount`, `delta_se0_cyccount`
in rv003usb.S), and with 32 cycles per bit the sampling points are exact
integers.

Two more things the host does for us: every transaction is initiated by the host
(tokens), so the device is purely reactive and can sleep in the interrupt until
an edge arrives; and a low speed interrupt endpoint is polled once per 1 ms, so
after answering a token the core has almost a millisecond for the application -
which is what makes the I2C bit banging in this firmware possible at all.

## One 8 byte packet per endpoint per millisecond

Low speed has no microframes. The host polls an interrupt endpoint once per 1 ms
frame and a low speed packet carries at most 8 bytes, so:

- **8 bytes/ms/direction** is the ceiling for interrupt transfers when the host
  submits one URB at a time. Measured: 200 fire-and-forget frames of 10 bytes
  (2 packets each) took 784 ms, i.e. ~2 frames per 2-packet transfer;
- the interesting consequence for a protocol designer: *round trip latency is
  measured in frames, not bytes*. A 4 byte result and a 60 byte result cost
  nearly the same.

## A control transfer is exactly 3 frames

SETUP, data stage, status. Measured with an empty-body vendor IN: **3.00 ms**
(333/s through the kernel GPIO driver, 2.98 ms from pyusb). That is why "one
control transfer per command" is a perfectly reasonable design on this link,
and why it is hard to beat:

| Transport                          | frames | measured           |
| ---------------------------------- | ------ | ------------------ |
| control transfer, small command    | 3      | 2.0-3.0 ms         |
| framed EP request + response       | 4+     | 3.9-7.8 ms         |
| control transfer, 64 byte payload  | 11     | 6.0 ms round trip  |
| EP transfer, 64 byte payload       | 16     | 32.3 ms round trip |

The 64 byte EP numbers are the old raw echo path (8 packets out, 8 packets
back); the control path carries a 64 byte data stage inside one transfer, which
is why `kernel.md` keeps control as the default transport.

## rv003usb answers every IN token - there is no NAK

`usb_handle_user_in_request()` is *obligated* to call `usb_send_data()` or
`usb_send_empty()` (rv003usb.h says so in as many words). When the device has
nothing queued it sends a **zero length packet**, and a ZLP is a *short packet*:
it completes the host's transfer successfully with 0 bytes.

So a host that asks for a response before the firmware has produced one wastes a
whole frame per attempt, and each retry is a new URB. A framed round trip is
therefore request (2 frames) + at least one empty poll + response (2 frames) ~
4-5 frames, always behind a 3 frame control transfer. This is the single reason
the endpoint path loses for small commands, and a NAK would need changes in
`rv003usb.S` (the stack has no NAK support at all).

## Data toggles, and the bug that cost a morning

OUT packets carry a data toggle (DATA0/DATA1). rv003usb's OUT handler is blunt:

```c
if (e->toggle_out != which_data) goto just_ack;   /* acknowledge, then drop */
e->toggle_out = !e->toggle_out;
```

- a mismatched packet is **acknowledged and thrown away**, so the host sees a
  perfectly successful transfer while the firmware never sees the data;
- the toggle does **not** advance on a mismatch, so it stays mismatched forever:
  the endpoint is dead until the device is reset.

And rv003usb only resets toggles for the endpoint that received a `SETUP`, while
the USB spec requires *every* endpoint's toggle to go back to DATA0 on
`SET_CONFIGURATION`. A host that reopens the endpoints without a bus reset (a
userspace libusb program between runs, or the kernel driver after the device was
used by something else) starts at DATA0 while the device still expects the parity
left over from the previous session.

Symptom, verbatim: `ep2.write()` returns success, `GET_EP_STATS` shows the EP2
receive counter frozen at 0, frame statistics stay `handled 0, dropped 0`, and
nothing appears on EP3 IN. Control transfers keep working, which makes it look
like the *frame code* is broken.

The firmware now resets all toggles on `SET_CONFIGURATION`
(`usb_reset_endpoint_toggles()` in `vendor/vendor.c`), and every EP level test
sends `SET_CONFIGURATION` first. Details: [debugging.md](debugging.md).

## The SETUP ACK budget

The handler runs before rv003usb acknowledges the SETUP, and the ACK has to make
it inside the low speed window: ~4.5 us, measured (see
[firmware.md](firmware.md)). A long handler is not "slow", it is a failed
transfer.

## Interrupt driven vs main loop: two different worlds

| Do this in the interrupt                    | Do this in `main()`                    |
| ------------------------------------------- | -------------------------------------- |
| copy OUT payloads into queues               | anything that touches the I2C/SPI pins |
| drain the EP3 IN FIFO                       | control-OUT data stage work            |
| answer control IN requests (a few loads)    | framed command execution               |
| read counters and status words              | stack scan, bus recovery, scans        |

The interface between the two is deliberately dumb: byte queues and counters.
There is no locking anywhere because each queue has exactly one writer and one
reader, and a full queue **drops the new item** instead of moving the other
side's index (that rule is worth keeping: dropping is visible through the
`*_DROPS` counters, corruption is not).

## What the bridge is worth end to end

The bus clocks on the board say nothing about what a host gets, because every
transaction pays for USB framing first: a control transfer is three 1 ms frames,
and on the endpoint path an endpoint moves one 8 byte packet per frame.  Measured
on this bench, expressed as the bus clock a host would need to move the same
bytes in the same time:

| What                                        | measured | equivalent clock      |
| ------------------------------------------- | -------- | --------------------- |
| 64 B control data stage out + 64 B in       | 6.00 ms  | 21.3 kB/s, SCK ~85 kHz |
| SPI 64 B full duplex over the control path  | 6.00 ms  | SCK ~85 kHz           |
| 64 B round trip over EP2/EP3 (one 64 B read)| 26.0 ms  | SCK ~20 kHz           |
| 64 B round trip over EP2/EP3 (8 B reads)    | 32.2 ms  | SCK ~16 kHz           |
| i2c adapter: 1 byte register read           | 9.00 ms  | SCL ~0.9 kHz          |
| i2c adapter: 64 byte block read             | 15.00 ms | SCL ~34 kHz           |
| i2c adapter: 64 byte page write             | 12.00 ms | SCL ~43 kHz           |
| single 4 byte control IN                    | 3.00 ms  | 333 bytes/s           |

So the on-wire rates - ~280 kHz of bit banged I2C, up to 24 MHz of SPI1 - are
reachable only in bulk.  64 bytes of SPI take 21 us on the wire and 6 ms through
the bridge (300x); 64 bytes of I2C at 280 kHz take ~2.9 ms on the wire against
6 ms through the bridge (2x), which is why I2C loses much less than SPI here.

The lesson is batching, not clocking: one 64 byte transfer gets ~85 kHz
equivalent while one byte at a time gets ~0.9 kHz - a factor of 95.  Anything
that reads a register per byte should be redesigned to move blocks (which is
what `I2C_MEM_WRITE_READ` did for the EEPROM: 18.0 ms against 42.0 ms).

## Driving a write-only display (measured)

A 128x160 RGB565 panel is 40960 bytes per frame, which makes it a convenient
unit for "how fast can this bridge write".  Two paths were measured, both with
the SPI module enabled and no MISO readback (a write-only panel needs none):

| Path                                      | per submission | rate      | full frame |
| ----------------------------------------- | -------------- | --------- | ---------- |
| control OUT data stage, 64 bytes          | 3.00 ms        | 21.3 kB/s | 1.92 s (0.52 fps) |
| EP2 OUT straight to MOSI, 8 byte packets  | 2.94 ms        | 2.7 kB/s  | 15.0 s (0.07 fps) |

Both are dominated by the same ~3 ms per submission - SETUP/data/STATUS for the
control transfer, one URB at a time for the endpoint - so the control path wins
by carrying eight times more per submission.  `SPI_GET_STATS` was checked in both
cases to prove the bytes really reached the SPI peripheral.

Note that a synchronous host call costs about 3 ms per 8 byte endpoint write,
not the 1 ms a frame-by-frame model predicts: the host has to submit and reap one
URB per call, and the endpoint is only serviced once per frame.  Keeping several
URBs in flight (the dln2 style pool) would recover part of that, but 8 bytes per
frame still caps that path below the control one.

The ceiling is the link, not the firmware: low speed is 1.5 Mbit/s, so a 40 kB
frame cannot take less than 0.22 s even with perfect framing, and low speed
transaction overhead (SYNC, PID, EOP, turnaround around an 8 byte payload) puts
a realistic best case nearer 1-2 fps.  Video over this bridge is therefore out of
reach; block updates are not - a 128x32 status bar is 8 kB (0.38 s), a 128 pixel
line is 256 bytes (12 ms), while drawing pixel by pixel is hopeless (2 bytes per
3 ms transfer).

## Host side habits worth keeping

- Read a result only after the firmware says it is done (`GET_CTRL_OUT_SEQ`, or
  the tagged `I2C_GET_RESULT`) - see [firmware.md](firmware.md).
- When testing, always verify the *cause* and not just the value: V003_GET_STATS
  byte counters, EP receive counters, `GET_FRAME_STATS`. A test that passes
  through a fallback path is worse than a failing one.
- Endpoint state survives a process exit (toggles, frame slots, EP3 IN FIFO
  contents). Re-synchronise at the start of a test instead of assuming a fresh
  device.
