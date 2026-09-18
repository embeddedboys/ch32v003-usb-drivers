#!/usr/bin/env python3
"""Watchdog test for the CH32V003 bridge: feed it, and (optionally) let it bite.

The non-destructive half checks the reset cause and the state, and proves that
feeding a watchdog that was never started does not arm it.  The interesting half
starts a short watchdog, stops feeding it, and waits for the chip to reset
itself - which is the only way to know that the timeout, the LSI based counter
and the reset flag really work together.  That part resets the board, so it is
opt-in:

    ./scripts/wdg_test.py                 # commands and state only
    ./scripts/wdg_test.py --reset 2000    # arm 2 s, stop feeding, expect a reset

The device comes back with `iwdg` as its reset cause and the watchdog off again
(the IWDG does not survive a reset), which the test checks.
"""

import argparse
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from v003_usb import find_device  # noqa: E402

WDG_MODULE = 0x04


def wdg(cmd):
    return cmd | (WDG_MODULE << 8)


WDG_START = wdg(0x60)
WDG_FEED = wdg(0x61)
WDG_FEED_KEY = 0xAAAA
WDG_GET_STATE = wdg(0x62)
WDG_GET_RESET_CAUSE = wdg(0x63)

RST_NAMES = {1: "pin", 2: "power-on", 4: "software", 8: "watchdog",
             16: "window watchdog", 32: "low power"}

failures = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


class Dev:
    def __init__(self, dev):
        self.dev = dev
        try:
            dev.set_configuration()
        except usb.core.USBError:
            pass
        # endpoint toggles live across sessions, so put both sides in sync
        dev.ctrl_transfer(0x00, 0x09, 1, 0, 0, timeout=2000)

    def out(self, val, idx):
        return self.dev.ctrl_transfer(0x40, 0, val, idx, 0, timeout=2000)

    def u32(self, idx, val=0):
        return int.from_bytes(
            bytes(self.dev.ctrl_transfer(0xC0, 0, val, idx, 4, timeout=2000)),
            "little")

    def state(self):
        v = self.u32(WDG_GET_STATE)
        return v >> 8, bool(v & 1)

    def cause(self):
        c = self.u32(WDG_GET_RESET_CAUSE)
        return c, [n for b, n in RST_NAMES.items() if c & b]

    def alive(self):
        try:
            self.u32(WDG_GET_STATE)
            return True
        except usb.core.USBError:
            return False


def open_dev():
    for _ in range(20):
        dev = find_device()
        if dev is not None:
            return Dev(dev)
        time.sleep(0.5)
    raise SystemExit("device not found")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reset", type=int, default=0, metavar="MS",
                    help="arm the watchdog for MS, stop feeding it and expect "
                         "the device to reset (destructive)")
    args = ap.parse_args()

    dev = open_dev()

    cause, names = dev.cause()
    print(f"info reset cause: {', '.join(names) or 'none'}")
    check("reset cause is a known set", cause & ~0x3F, 0)

    check("watchdog is off at boot", dev.state()[1], False)
    dev.out(WDG_FEED_KEY, WDG_FEED)
    check("feeding an unstarted watchdog does not arm it", dev.state()[1], False)
    dev.out(0x1234, WDG_FEED)
    check("a wrong feed key does nothing", dev.state()[1], False)

    if not args.reset:
        print("\n(armed/expiry test skipped: pass --reset <ms> to run it)")
        return 1 if failures else 0

    dev.out(args.reset, WDG_START)
    timeout_ms, running = dev.state()
    check("watchdog reports running after start", running, True)
    check("reported timeout is close to what was asked",
          abs(timeout_ms - args.reset) < 200, True)
    print(f"info armed for {timeout_ms} ms")

    # keep it alive for about the same time again: it must not reset
    for _ in range(3):
        time.sleep(args.reset / 4000.0)
        dev.out(WDG_FEED_KEY, WDG_FEED)
    check("device is alive while being fed", dev.alive(), True)

    print("info stop feeding and wait...")
    deadline = time.time() + (args.reset / 1000.0) * 3
    while time.time() < deadline and dev.alive():
        time.sleep(0.2)
    check("device is gone (it reset)", dev.alive(), False)

    time.sleep(2.5)
    dev = open_dev()
    cause, names = dev.cause()
    print(f"info reset cause after the bite: {', '.join(names) or 'none'}")
    check("the device reports a watchdog reset", bool(cause & 8), True)
    check("the watchdog is off after the reset", dev.state()[1], False)

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
