# What software USB on this chip costs

The device is a **low speed** (1.5 Mbit/s) USB device whose USB stack is bit
banged on a 48 MHz RISC-V core. Everything below is a consequence of that one
sentence, and most of the surprising behaviour of this project is a consequence
of these numbers.

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

## Host side habits worth keeping

- Read a result only after the firmware says it is done (`GET_CTRL_OUT_SEQ`, or
  the tagged `I2C_GET_RESULT`) - see [firmware.md](firmware.md).
- When testing, always verify the *cause* and not just the value: V003_GET_STATS
  byte counters, EP receive counters, `GET_FRAME_STATS`. A test that passes
  through a fallback path is worse than a failing one.
- Endpoint state survives a process exit (toggles, frame slots, EP3 IN FIFO
  contents). Re-synchronise at the start of a test instead of assuming a fresh
  device.
