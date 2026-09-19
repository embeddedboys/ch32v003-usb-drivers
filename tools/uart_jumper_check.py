#!/usr/bin/env python3
"""Is it the PD0<->PD1 jumper, or is it the firmware?

The UART's receive path needs a jumper between PD0 (TX) and PD1 (RX), because
PD1 is also the chip's SWIO debug pin and the WCH-LinkE holds it.  That jumper
makes every "nothing came back" failure ambiguous, and the two causes need
opposite fixes:

  * no jumper installed      -> the firmware never saw the bytes at all;
  * jumper installed         -> the bytes *were* transmitted and the receive
    path (interrupt, ring, counters) is what failed.

This tool separates them in two steps and prints what each one measured:

  1. **look at the wire without the UART.**  The port is disabled, which hands
     PD0 and PD1 back as plain GPIO, and each pin is read as an input with the
     internal pull-up and then with the internal pull-down.  A pad that follows
     the pull has nothing attached to it; a pad that stays high is being held by
     something external.  PD0 has nothing else on it on this board, so
     "PD0 stuck at 1" means it is tied to PD1 - which the programmer holds high
     - and the jumper is there.  This step never drives a pad, deliberately:
     PD1 is SWIO and driving into the programmer is a short.
  2. **then do one slow loopback**, at the module's slowest documented baud
     (733, where a byte takes 13.6 ms), and report the counters that say who
     moved what: `tx_bytes` (the transmitter), `rx_bytes` and the receive
     interrupt count (the receiver), plus the drop counters.

  tools/uart_jumper_check.py                 # 733 baud, 8 byte pattern
  tools/uart_jumper_check.py --baud 9600 --size 32
  tools/uart_jumper_check.py --pins-only     # just the wire check, no traffic

One caveat, stated because it is a real limit of step 1: with **no programmer
attached**, a floating PD1 leaves the jumpered PD0 floating too, so step 1 would
report "no jumper".  It is reliable in the normal case this repo works in (a
WCH-LinkE on SWIO) and it is only a hint, not proof - step 2 is what proves the
data path.

The port is disabled again before this tool exits, jumper or not: a UART that
keeps PD0 driving an idle high line through the jumper also keeps `minichlink`
out (`nothing connected to linker`, see README).
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from status import Dev, GET_CAPABILITIES, CAP_STRUCT, pin_name  # noqa: E402

GPIO = 0x01 << 8
GPIO_GET = 0x07 | GPIO
GPIO_REQUEST = 0x08 | GPIO
GPIO_FREE = 0x09 | GPIO
GPIO_DIRECTION_INPUT = 0x0B | GPIO
GPIO_DIRECTION_OUTPUT = 0x0C | GPIO

UART = 0x07 << 8
UART_CONFIG = 0x90 | UART
UART_GET_CFG = 0x91 | UART
UART_WRITE = 0x92 | UART
UART_READ = 0x93 | UART
UART_GET_STATE = 0x94 | UART
UART_GET_COUNTS = 0x95 | UART
UART_GET_ERRORS = 0x96 | UART
UART_FLUSH = 0x97 | UART
UART_CLEAR_STATS = 0x9A | UART

CFG_STRUCT = "<IBBBBI"  # baud, data_bits, parity, stop_bits, enable, actual
TX_PIN = 48  # PD0
RX_PIN = 49  # PD1
BAUD_MIN = 733  # V003_UART_BAUD_MIN: below this the divisor does not fit 16 bits


def gpio_val(pin, val=0):
    return ((pin & 0xFF) << 8) | (val & 1)


def read_with_pull(d, pin, pull):
    """Input level with a known weak pull; the module has no pull selector, so
    the level is chosen by driving it once and then switching to input."""
    d.out(GPIO_DIRECTION_OUTPUT, gpio_val(pin, pull))
    d.out(GPIO_DIRECTION_INPUT, gpio_val(pin))
    return d.u32(GPIO_GET, gpio_val(pin))


def wire_check(d, tx_pin, rx_pin):
    """Look at the two pads as plain GPIO.  Returns True if a jumper is likely."""
    levels = {}
    for pin in (tx_pin, rx_pin):
        d.out(GPIO_REQUEST, gpio_val(pin))
        levels[pin] = (read_with_pull(d, pin, 1), read_with_pull(d, pin, 0))
        print(f"     {pin_name(pin)} ({pin}): pull-up reads {levels[pin][0]}, "
              f"pull-down reads {levels[pin][1]}")

    for pin in (tx_pin, rx_pin):
        d.out(GPIO_FREE, gpio_val(pin))

    tx_up, tx_down = levels[tx_pin]
    if (tx_up, tx_down) == (1, 1):
        print(f"     {pin_name(tx_pin)} is held high by something external: on "
              f"this board that is the jumper to {pin_name(rx_pin)}, which the "
              f"programmer holds high")
        return True
    if tx_up != tx_down:
        print(f"     {pin_name(tx_pin)} follows the internal pull, so nothing is "
              f"tied to it: no jumper (or no programmer on SWIO, which leaves "
              f"the other end floating too)")
        return False

    print(f"     {pin_name(tx_pin)} reads {tx_up} with both pulls, which is "
          f"neither a floating pad nor a held-high one - unexpected")
    return None


def loopback(d, baud, size):
    """One slow round trip, with the counters that say who moved the bytes."""
    d.out_data(UART_CONFIG, struct.pack(CFG_STRUCT, baud, 8, 0, 1, 1, 0))
    actual = struct.unpack(CFG_STRUCT, d.in_data(UART_GET_CFG, 12))[5]
    d.out(UART_FLUSH)
    d.out(UART_CLEAR_STATS)
    time.sleep(0.02)
    drain(d)
    d.out(UART_CLEAR_STATS)  # the flush above is a dropped byte of its own

    errs0 = d.u32(UART_GET_ERRORS)
    c0 = d.u32(UART_GET_COUNTS)
    pattern = bytes((i * 7 + 1) & 0xFF for i in range(size))
    d.out_data(UART_WRITE, pattern)

    # a byte at this baud takes ~13.6 ms, and a control transfer ~1 ms, so wait
    # for the whole pattern instead of draining until empty
    want = size
    got = b""
    deadline = time.time() + 1.0 + size * 14.0 / baud
    while len(got) < want and time.time() < deadline:
        avail = (d.u32(UART_GET_STATE) >> 16) & 0xFF
        if avail:
            got += d.in_data(UART_READ, 64, val=min(avail, want - len(got)))
        else:
            time.sleep(0.001)

    c1 = d.u32(UART_GET_COUNTS)
    errs1 = d.u32(UART_GET_ERRORS)
    st = d.u32(UART_GET_STATE)

    # error word: isr entries, overrun, framing, parity, one byte each
    isr = (errs1 >> 24) - (errs0 >> 24)
    overrun = ((errs1 >> 16) & 0xFF) - ((errs0 >> 16) & 0xFF)
    framing = ((errs1 >> 8) & 0xFF) - ((errs0 >> 8) & 0xFF)
    parity = (errs1 & 0xFF) - (errs0 & 0xFF)
    tx_bytes = (c1 >> 16) - (c0 >> 16)
    rx_bytes = (c1 & 0xFFFF) - (c0 & 0xFFFF)

    print(f"     configured {baud} baud (the divisor works out to {actual}), "
          f"sent {size} bytes")
    print(f"     tx_bytes +{tx_bytes}, rx_bytes +{rx_bytes}, "
          f"receive interrupts +{isr}, overrun +{overrun}, "
          f"framing +{framing}, parity +{parity}")
    print(f"     after the transfer: tx_queued={st >> 24}, rx_avail="
          f"{(st >> 16) & 0xFF}, tx_dropped={(st >> 8) & 0xFF}, "
          f"rx_dropped={st & 0xFF}")

    return got, pattern, tx_bytes, rx_bytes


def drain(d):
    while True:
        avail = (d.u32(UART_GET_STATE) >> 16) & 0xFF
        if not avail:
            return
        d.in_data(UART_READ, 64, val=avail)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baud", type=int, default=BAUD_MIN,
                    help=f"loopback baud (default {BAUD_MIN}, the module minimum)")
    ap.add_argument("--size", type=int, default=8,
                    help="bytes in the pattern (default 8)")
    ap.add_argument("--pins-only", action="store_true",
                    help="only look at the wire, send no traffic")
    ap.add_argument("--tx-pin", type=int, default=TX_PIN,
                    help=f"pad to check as the transmitter (default {TX_PIN}, "
                         f"PD0); point it at a floating pad to see what the "
                         f"'no jumper' answer looks like")
    ap.add_argument("--rx-pin", type=int, default=RX_PIN,
                    help=f"pad to check as the receiver (default {RX_PIN}, PD1)")
    args = ap.parse_args()

    d = Dev()
    caps, _ngpio, _nadc, _npwm, nuart, _rlo, _rhi = struct.unpack(
        CAP_STRUCT, d.in_data(GET_CAPABILITIES, 16))
    if not caps & (1 << 5):
        print("FAIL this firmware has no UART module (caps "
              f"{caps:#x}): build with MODULES including uart")
        return 1
    if not nuart:
        print(f"FAIL the capability report says the device has {nuart} UARTs")
        return 1
    if args.baud < BAUD_MIN:
        print(f"FAIL {args.baud} baud is below the module minimum {BAUD_MIN} "
              f"(the divisor would not fit 16 bits)")

    try:
        # step 1: the wire, with the UART out of the way so PD0 is a plain pad
        print("info wire check (port disabled, no pad is driven)")
        d.out_data(UART_CONFIG, struct.pack(CFG_STRUCT, 115200, 8, 0, 1, 0, 0))
        jumped = wire_check(d, args.tx_pin, args.rx_pin)
        if args.pins_only:
            return 0

        # step 2: one slow round trip
        print("info loopback")
        got, pattern, tx, rx = loopback(d, args.baud, args.size)

        print("info verdict")
        if got == pattern:
            print(f"ok   the jumper is there and the whole receive path works: "
                  f"{len(got)} bytes came back identical")
            return 0

        if jumped is False:
            print("FAIL nothing came back and PD0 is not tied to anything: "
                  "**the jumper is missing**.  Put it between PD0 and PD1 - the "
                  "firmware was never given the bytes.")
            return 1

        if rx == 0 and tx == args.size:
            print(f"FAIL the transmitter counted {tx} bytes and the receiver "
                  f"counted none, and PD0 is held high (the jumper is there): "
                  f"the receive path is broken, not the wiring.  Check the "
                  f"receive interrupts and the ring - see notes/uart.md.")
            return 1

        print(f"FAIL the round trip is not clean: {len(got)} of {args.size} "
              f"bytes came back, tx_bytes +{tx}, rx_bytes +{rx}.  Neither "
              f"'jumper missing' nor 'receiver dead' explains this - read the "
              f"byte counts above.")
        return 1
    finally:
        try:
            d.out_data(UART_CONFIG,
                       struct.pack(CFG_STRUCT, 115200, 8, 0, 1, 0, 0))
            print("info port disabled: PD0 released, SWIO is free for the "
                  "programmer again")
        except usb.core.USBError as exc:
            print(f"info could not disable the port: {exc}")


if __name__ == "__main__":
    sys.exit(main())
