#!/usr/bin/env python3
"""Stress the userspace data path until the device wedges.

Every now and then the firmware stops answering and drops off the bus; a chip
reboot (`minichlink -b`) brings it back.  This tool narrows down which traffic
triggers it by hammering one transport at a time and reporting the iteration
that failed.

Usage:
    scripts/stress_test.py --mode ctrl --iterations 500
    scripts/stress_test.py --mode ep   --iterations 200

Modes:
    ctrl  control-OUT data stage (64 B) + control-IN read back, per iteration
    ep    EP1 OUT (64 B) + EP3 IN drain, per iteration
    mixed both, alternating

Requires pyusb.
"""

import argparse
import sys
import time

import usb.core
import usb.util

from v003_usb import VID, PID

GENERIC_MODULE_ID = 0x00
GET_CTRL_OUT_DATA = 0x36 | (GENERIC_MODULE_ID << 8)
GET_FIFO_LEVEL = 0x33 | (GENERIC_MODULE_ID << 8)

EP1_OUT = 0x01
EP3_IN = 0x83


def open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        return None
    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]
    return dev, usb.util.find_descriptor(intf, bEndpointAddress=EP1_OUT), \
        usb.util.find_descriptor(intf, bEndpointAddress=EP3_IN)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("ctrl", "ep", "mixed"), default="ctrl")
    ap.add_argument("--iterations", type=int, default=200)
    args = ap.parse_args()

    opened = open_device()
    if not opened:
        print("FAIL: device not found (reboot it with `minichlink -b`)")
        return 1
    dev, ep1, ep3 = opened

    payload = bytes((i * 3 + 1) & 0xFF for i in range(64))
    started = time.time()

    for i in range(args.iterations):
        mode = args.mode
        if mode == "mixed":
            mode = "ctrl" if i % 2 else "ep"

        try:
            if mode == "ctrl":
                dev.ctrl_transfer(0x40, 0x00, 0x1234, GET_CTRL_OUT_DATA,
                                  payload, timeout=2000)
                back = bytes(dev.ctrl_transfer(0xC0, 0x00, 0x1234,
                                               GET_CTRL_OUT_DATA, 64,
                                               timeout=2000))
            else:
                ep1.write(payload)
                back, want = b"", len(payload)
                deadline = time.time() + 2.0
                while len(back) < want and time.time() < deadline:
                    chunk = bytes(ep3.read(8, timeout=1000))
                    if chunk:
                        back += chunk
                back = back[:want]
        except Exception as e:
            dt = time.time() - started
            print(f"WEDGED after {i} iterations ({dt:.1f} s), mode={mode}: {e}")
            return 2

        if back != payload:
            print(f"MISMATCH at iteration {i} (mode={mode}): "
                  f"{len(back)} bytes back: {back[:16].hex()}")
            return 3

    dt = time.time() - started
    print(f"OK: {args.iterations} iterations, mode={args.mode}, "
          f"{dt:.1f} s ({args.iterations / dt:.0f} iter/s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
