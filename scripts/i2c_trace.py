#!/usr/bin/env python3
"""Decode the firmware's I2C clock trace.

The firmware samples SDA at every rising SCL edge - exactly the instant a slave
latches it - together with the time since the previous edge, so the byte stream
the device actually sees can be reconstructed on the host.  That is what makes
a wrong clock count at a byte boundary visible: the decoded bytes stop matching
what was sent from that point on.

    scripts/i2c_trace.py [--addr 0x0100] [--count 1]

Requires pyusb.
"""

import sys
import time

import usb.core

from v003_usb import VID, PID

I2C, GENERIC = 0x03, 0x00
CONFIG = 0x50 | (I2C << 8)
WR = 0x57 | (I2C << 8)
TRACE = 0x5A | (I2C << 8)
GET_TRACE = 0x5B | (I2C << 8)
GET_TRACE_INFO = 0x5C | (I2C << 8)
SEQ = 0x37 | (GENERIC << 8)

DEV_ADDR = 0x50


def main():
    addr = 0x0100
    count = 1
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--addr" and i + 1 < len(args):
            addr = int(args[i + 1], 0)
        if a == "--count" and i + 1 < len(args):
            count = int(args[i + 1])

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("FAIL: device not found")
        return 1

    def ctrl(val, idx, ln=0):
        return dev.ctrl_transfer(0x40 if ln == 0 else 0xC0, 0x00, val, idx, ln,
                                 timeout=3000)

    def u32(idx, val=0):
        return int.from_bytes(bytes(ctrl(val, idx, 4)), "little")

    def wait(before, t=2.0):
        end = time.time() + t
        while time.time() < end and u32(SEQ) == before:
            pass

    def sync(t=1.0):
        """Wait until no request is pending.

        Reading the counter as a baseline right after sending *another*
        request is what made an earlier revision read results one transaction
        late: the pending increment satisfied the wait.  Settle first.
        """
        last = u32(SEQ)
        end = time.time() + t
        while time.time() < end:
            time.sleep(0.002)
            now = u32(SEQ)
            if now == last:
                return now
            last = now
        return last

    ctrl(50, CONFIG)
    sync()

    # Arm the trace and wait for the firmware to *confirm* it (arming clears the
    # entry count).  Settling on the completion counter alone is not enough: if
    # the arm request has not been picked up yet, the settle looks satisfied and
    # the previous transaction's trace is read back.
    payload = bytes([DEV_ADDR << 1, (addr >> 8) & 0xFF, addr & 0xFF])
    ctrl(1, TRACE)
    end = time.time() + 2.0
    while time.time() < end and (u32(GET_TRACE_INFO) & 0xFF) != 0:
        time.sleep(0.002)
    before = sync()
    dev.ctrl_transfer(0x40, 0x00, count, WR, payload, timeout=3000)
    wait(before)
    ctrl(0, TRACE)

    info = u32(GET_TRACE_INFO)
    n = info & 0xFF
    overflow = (info >> 8) & 0xFF
    unit = (info >> 16) & 0xFFFF
    print(f"trace: {n} rising SCL edges recorded"
          + (", BUFFER OVERFLOWED" if overflow else "")
          + f"  (time unit = {unit} ticks)")

    raw = b""
    off = 0
    while off < n * 2:
        chunk = bytes(ctrl(off, GET_TRACE, min(64, n * 2 - off)))
        if not chunk:
            break
        raw += chunk
        off += len(chunk)

    edges = [(raw[i * 2], raw[i * 2 + 1]) for i in range(min(n, len(raw) // 2))]

    print("\nSDA level at each rising SCL edge (dt = ticks since previous edge):")
    line = ""
    for i, (dt, sda) in enumerate(edges):
        line += f"{sda}"
        if (i + 1) % 9 == 0:
            line += " "
    print("  " + line)
    print("  dts: " + " ".join(str(dt) for dt, _ in edges))

    # decode: 9 bit groups (8 data + ACK) straight from the first edge; the
    # repeated START contributes one extra edge, so everything after it is
    # shifted by one group and needs a manual read
    bits = [sda for _, sda in edges]
    print("\n9-edge grouping (byte + ACK):")
    i = 0
    g = 0
    while i + 9 <= len(bits):
        grp = bits[i:i + 9]
        val = 0
        for b in grp[:8]:
            val = (val << 1) | b
        print(f"  group {g}: {''.join(map(str, grp[:8]))} ack={grp[8]} -> 0x{val:02x}")
        i += 9
        g += 1
    if i < len(bits):
        print(f"  trailing {len(bits) - i} edge(s): "
              f"{''.join(map(str, bits[i:]))}")

    print(f"\nsent address bytes: {(addr >> 8) & 0xFF:#04x} {(addr & 0xFF):#04x} "
          f"(dev 0x{DEV_ADDR:02x}, read count {count})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
