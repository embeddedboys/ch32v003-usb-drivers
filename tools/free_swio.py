#!/usr/bin/env python3
"""Give the debug pin back to the programmer.

`minichlink` failing with `nothing connected to linker` (or reading
`HARTINFO: ffffffff`) is nearly always the same thing on this bench: the
PD0<->PD1 jumper is in place, the UART port is enabled, and the firmware is
driving PD0 as an idle-high push-pull output - which through the jumper holds
PD1, the chip's SWIO line, and fights the WCH-LinkE that is trying to talk to it.

The fix does not need the jumper removed and does not need a power cycle: ask the
firmware, over USB, to disable the UART port.  That drops both pads back to
inputs (the firmware only configures them when the port is enabled), and the
programmer gets SWIO to itself again.

  tools/free_swio.py              # disable the UART port, show the pad directions
  tools/free_swio.py --reset 400  # ... and reset the chip through the watchdog
  tools/free_swio.py --check      # only report, change nothing

This tool measures instead of assuming: after the port is disabled it reads the
pad's mode back with `GPIO_GET_DIRECTION` (0x0a), because "the firmware says it
disabled the port" and "the pad is really an input again" are two different
statements - the first version of the ADC module taught this project that a pin
mode can be changed by something other than the module that documents it.

If the device does not answer USB at all, nothing here can help: the chip is
either held in reset by the programmer, or the firmware is wedged.  Reset it with
`scripts/wdg_test.py --reset 400` (needs USB), by power cycling, or by holding
the programmer's reset.  A fresh boot always has the UART disabled.
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from status import (Dev, GET_CAPABILITIES, CAP_STRUCT, GET_FIRMWARE_VER,  # noqa: E402
                    pin_name)

GPIO = 0x01 << 8
GPIO_GET_DIRECTION = 0x0A | GPIO

UART = 0x07 << 8
UART_CONFIG = 0x90 | UART
UART_GET_CFG = 0x91 | UART

WDG = 0x04 << 8
WDG_START = 0x60 | WDG

CFG_STRUCT = "<IBBBBI"  # baud, data_bits, parity, stop_bits, enable, actual
TX_PIN = 48  # PD0
RX_PIN = 49  # PD1 (SWIO)
CAP_UART = 1 << 5


def pad_report(d, label):
    for pin in (TX_PIN, RX_PIN):
        direction = d.u32(GPIO_GET_DIRECTION, (pin << 8) & 0xFFFF)
        print(f"     {label} {pin_name(pin)} ({pin}): "
              f"{'input (high impedance)' if direction else 'OUTPUT - still driving'}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reset", type=int, default=0, metavar="MS",
                    help="also arm the watchdog for MS and stop feeding it, so "
                         "the chip resets (destructive; the device re-enumerates)")
    ap.add_argument("--check", action="store_true",
                    help="report the state and change nothing")
    args = ap.parse_args()

    try:
        d = Dev()
    except SystemExit:
        print("FAIL the device does not answer USB at all, so there is nothing "
              "to ask.  Either the kernel modules own the interface "
              "(`tools/modules.sh unload`) or the chip is wedged:")
        print("     - reset it with `scripts/wdg_test.py --reset 400` if USB "
              "answers, otherwise power cycle it or use the programmer's reset")
        print("     - a fresh boot always has the UART port disabled, so the "
              "pads are inputs again")
        return 1

    print(f"info firmware {d.u32(GET_FIRMWARE_VER):#x}")
    caps, _ngpio, _nadc, _npwm, nuart, _rlo, _rhi = struct.unpack(
        CAP_STRUCT, d.in_data(GET_CAPABILITIES, 16))

    print("info before")
    pad_report(d, "")

    if not caps & CAP_UART or not nuart:
        print(f"info this build has no UART (caps {caps:#x}), so it cannot be "
              f"the thing driving PD0 - nothing to release")
    else:
        baud, _db, _par, _stop, enable, _actual = struct.unpack(
            CFG_STRUCT, d.in_data(UART_GET_CFG, 12))
        print(f"info the UART port is {'ENABLED' if enable else 'disabled'}"
              f" at {baud} baud")
        if not args.check and enable:
            d.out_data(UART_CONFIG, struct.pack(CFG_STRUCT, baud, 8, 0, 1, 0, 0))
            print("info asked the firmware to disable the port: both pads are "
                  "inputs again")
            print("info after")
            pad_report(d, "")
        elif args.check:
            print("info --check: not touching the port")

    if args.reset:
        if args.check:
            print("info --check: not resetting either")
        else:
            print(f"info arming the watchdog for {args.reset} ms and not "
                  f"feeding it: the chip resets and re-enumerates")
            d.out(args.reset, WDG_START)
            deadline = time.time() + args.reset / 1000.0 * 3 + 2.0
            while time.time() < deadline:
                time.sleep(0.2)
                try:
                    d = Dev()
                    print("info the device is back after the reset (a fresh "
                          "boot has the UART disabled)")
                    break
                except SystemExit:
                    continue
            else:
                print("FAIL the device did not come back after the watchdog "
                      "timeout")
                return 1

    print("info next: `make -C vendor` builds and flashes.  With the UART port "
          "disabled the jumper no longer blocks SWIO; if minichlink still cannot "
          "attach, unplug the PD0<->PD1 jumper - that is the guaranteed fix.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
