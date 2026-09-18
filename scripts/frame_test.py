#!/usr/bin/env python3
"""Framed endpoint protocol test for the CH32V003 vendor device.

The control path costs about 3 ms per operation (a low-speed control transfer
needs one frame for SETUP, one for the data stage and one for the status
stage).  The framed path moves the same commands over the interrupt endpoints
instead: requests go out on EP2 OUT, responses come back on EP3 IN, and a
response is only sent when the request carried a non-zero echo tag.

Frame layout (all fields little endian):

    request   [size u16][id u16][echo u16][handle u16][payload...]
    response  [size u16][id u16][echo u16][handle u16][result u16][payload...]

Requires pyusb:  python3 -m venv .venv && .venv/bin/pip install pyusb
"""

import argparse
import struct
import sys
import time

import usb.core
import usb.util

from v003_usb import VID, PID

EP1_OUT = 0x01
EP2_OUT = 0x02
EP3_IN = 0x83

USB_SET_CONFIGURATION = 0x09

FRAME_HDR = 8
EP_PACKET = 8  # low speed interrupt endpoint: 8 byte packets
FRAME_MAX = 72

OK = 0
EBADCMD = 1
ETOOBIG = 2

GENERIC = 0x00
GPIO = 0x01

SET_FRAME_MODE = 0x39
GET_FRAME_STATS = 0x3A
GET_DEVICE_VER = 0x30
GET_DEVICE_SN = 0x31
GET_EP_STATS = 0x32
GET_FIFO_LEVEL = 0x33
GET_FIFO_DROPS = 0x34

GPIO_SET = 0x06
GPIO_GET = 0x07
GPIO_GET_DIRECTION = 0x0A
GPIO_DIRECTION_INPUT = 0x0B
GPIO_DIRECTION_OUTPUT = 0x0C

PC0 = 32
PC4 = 36


def cmd(c, mid):
    return c | (mid << 8)


failures = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


class Device:
    def __init__(self, dev):
        self.dev = dev
        cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]
        self.ep2 = usb.util.find_descriptor(intf, bEndpointAddress=EP2_OUT)
        self.ep3 = usb.util.find_descriptor(intf, bEndpointAddress=EP3_IN)
        if not (self.ep2 and self.ep3):
            raise RuntimeError(f"endpoints not found: {self.ep2} {self.ep3}")

    # ---- control path -------------------------------------------------
    def ctrl(self, val, idx, length=0):
        bm = 0x40 if length == 0 else 0xC0
        return self.dev.ctrl_transfer(bm, 0x00, val, idx, length, timeout=2000)

    def u32(self, idx, val=0):
        return int.from_bytes(bytes(self.ctrl(val, idx, 4)), "little")

    def frame_mode(self, on):
        self.ctrl(1 if on else 0, SET_FRAME_MODE)

    def frame_stats(self):
        val = self.u32(GET_FRAME_STATS)
        return val >> 16, val & 0xFFFF

    # ---- framed path --------------------------------------------------
    @staticmethod
    def build(iid, handle, arg=None, echo=1, payload=b"", size=None):
        if arg is not None:
            payload = struct.pack("<H", arg) + payload
        body = struct.pack("<HHH", iid, echo, handle) + payload
        total = FRAME_HDR + len(payload) if size is None else size
        return struct.pack("<H", total) + body

    def send(self, frame):
        assert self.ep2.write(bytes(frame), timeout=2000) == len(frame)

    def collect(self, timeout_ms=2000):
        """Read one response frame, one packet at a time."""
        buf = b""
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            try:
                chunk = bytes(self.ep3.read(EP_PACKET, timeout=timeout_ms))
            except usb.core.USBTimeoutError:
                break
            if not chunk:
                continue
            buf += chunk
            if len(buf) >= FRAME_HDR:
                size = struct.unpack_from("<H", buf)[0]
                if size < FRAME_HDR + 2 or size > FRAME_MAX:
                    return None
                if len(buf) >= size:
                    return self.parse(buf[:size])
        return None

    @staticmethod
    def parse(buf):
        size, iid, echo, handle, result = struct.unpack_from("<HHHHH", buf)
        return {
            "size": size,
            "id": iid,
            "echo": echo,
            "handle": handle,
            "result": result,
            "payload": buf[FRAME_HDR + 2:],
            "value": int.from_bytes(buf[FRAME_HDR + 2:], "little"),
        }

    def request(self, iid, handle, arg=None, echo=1, payload=b"", timeout_ms=2000):
        self.send(self.build(iid, handle, arg, echo, payload))
        if not echo:
            return None
        rsp = self.collect(timeout_ms)
        if rsp is None:
            handled, dropped = self.frame_stats()
            raise RuntimeError(
                f"no response for id 0x{iid:02x} "
                f"(frames handled {handled}, dropped {dropped}, "
                f"fifo level {self.u32(GET_FIFO_LEVEL)})")
        if rsp["echo"] != echo:
            raise RuntimeError(
                f"echo mismatch: got 0x{rsp['echo']:04x} want 0x{echo:04x}")
        return rsp

    def quiet(self, ms):
        """True when nothing (not even an empty packet) shows up on EP3 IN."""
        time.sleep(ms / 1000.0)
        try:
            return len(bytes(self.ep3.read(EP_PACKET, timeout=200))) == 0
        except usb.core.USBTimeoutError:
            return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", type=int, default=0,
                    help="sequential framed operations to time")
    ap.add_argument("--pipe", type=int, default=0,
                    help="fire-and-forget frames to time back to back")
    args = ap.parse_args()

    dev = Device(usb.core.find(idVendor=VID, idProduct=PID))
    if dev.dev is None:
        print("device not found")
        return 1

    try:
        dev.dev.set_configuration()
    except usb.core.USBError:
        pass

    # The device keeps each endpoint's data toggle across sessions while the
    # host restarts them at DATA0, and a mismatched OUT packet is silently
    # dropped by the device.  SET_CONFIGURATION puts both sides back in sync
    # (the firmware resets the toggles for it, as the USB spec requires).
    dev.dev.ctrl_transfer(0x00, USB_SET_CONFIGURATION, 1, 0, 0, timeout=2000)

    dev.frame_mode(0)
    # make sure the control path and the raw echo path are quiet first
    dev.ctrl(0xFF, GET_EP_STATS, 0)
    dev.frame_mode(1)

    print("== framed path: generic queries ==")
    rsp = dev.request(GET_DEVICE_VER, GENERIC)
    check("GET_DEVICE_VER result", rsp["result"], OK)
    check("GET_DEVICE_VER value", rsp["value"], 0x1010)
    check("GET_DEVICE_VER handle", rsp["handle"], GENERIC)
    check("GET_DEVICE_VER size", rsp["size"], FRAME_HDR + 2 + 4)
    check("GET_DEVICE_SN", dev.request(GET_DEVICE_SN, GENERIC)["value"],
          0x12345678)

    print("\n== framed path: gpio ==")
    dev.request(GPIO_DIRECTION_OUTPUT, GPIO, (PC0 << 8) | 1)
    check("PC0 high", dev.request(GPIO_GET, GPIO, PC0 << 8)["value"], 1)
    dev.request(GPIO_SET, GPIO, (PC0 << 8) | 0)
    check("PC0 low", dev.request(GPIO_GET, GPIO, PC0 << 8)["value"], 0)
    dev.request(GPIO_DIRECTION_INPUT, GPIO, PC4 << 8)
    rsp = dev.request(GPIO_GET_DIRECTION, GPIO, PC4 << 8)
    check("PC4 direction is input result", rsp["result"], OK)
    check("PC4 direction is input", rsp["value"], 1)
    # the control-path encoding (cmd | module << 8) has to work as well
    rsp = dev.request(cmd(GPIO_GET_DIRECTION, GPIO), GPIO, PC4 << 8)
    check("control-path encoded id still works", rsp["result"], OK)
    check("   ... and answers", rsp["value"], 1)

    print("\n== echo tag handling ==")
    check("echo is copied back",
          dev.request(GPIO_GET, GPIO, PC0 << 8, echo=0xBEEF)["echo"], 0xBEEF)
    # echo 0 means "no answer wanted": the device must stay silent, so the
    # next response has to be the tagged one and not a stray reply
    dev.send(dev.build(GPIO_SET, GPIO, (PC0 << 8) | 1, echo=0))
    check("no response for echo 0", dev.quiet(30), True)
    rsp = dev.request(GPIO_GET, GPIO, PC0 << 8, echo=0x1234)
    check("tagged response after untagged request", rsp["echo"], 0x1234)
    check("untagged SET was applied", rsp["value"], 1)
    dev.request(GPIO_SET, GPIO, (PC0 << 8) | 0)

    print("\n== error reporting ==")
    rsp = dev.request(0x30, 0x7F)
    check("unknown module -> EBADCMD", rsp["result"], EBADCMD)
    check("unknown module -> no payload", rsp["payload"], b"")
    check("unknown gpio command",
          dev.request(0x30, GPIO)["result"], EBADCMD)

    print("\n== resynchronisation ==")
    before = dev.frame_stats()[1]
    # a bogus length must be dropped, and must not corrupt the next frame
    bad = struct.pack("<HHHH", 0x0003, 0x1111, 0x0001, 0x0000)
    dev.send(bad + b"\xaa" * 8)
    check("bogus length is dropped", dev.frame_stats()[1] > before, True)
    check("next frame still works",
          dev.request(GPIO_GET, GPIO, PC0 << 8)["value"], 0)
    check("no stray response from the bogus frame", dev.quiet(20), True)

    print("\n== multi packet frames ==")
    # 72 bytes = header + arg + 62 payload = 9 OUT packets, the largest frame
    # the firmware accepts
    pad = bytes(range(0x40, 0x40 + 62))
    dev.send(dev.build(GPIO_SET, GPIO, (PC0 << 8) | 1, echo=0, payload=pad))
    check("72 byte frame applied", dev.request(GPIO_GET, GPIO, PC0 << 8)["value"],
          1)
    rsp = dev.request(GPIO_GET, GPIO, PC0 << 8)
    check("device still in sync after a full frame", rsp["result"], OK)
    dev.request(GPIO_SET, GPIO, (PC0 << 8) | 0)

    if args.bench:
        print(f"\n== sequential latency ({args.bench} operations) ==")
        n = args.bench
        t0 = time.time()
        for _ in range(n):
            dev.request(GPIO_GET, GPIO, PC0 << 8)
        dt = time.time() - t0
        framed_us = dt / n * 1e6

        t0 = time.time()
        for _ in range(n):
            dev.ctrl(GPIO_GET, PC0 << 8, 4)
        ctrl_us = (time.time() - t0) / n * 1e6

        print(f"framed  EP2 OUT + EP3 IN : {framed_us / 1000:.2f} ms/op "
              f"({n / dt:.0f}/s)")
        print(f"control EP0 transfer     : {ctrl_us / 1000:.2f} ms/op "
              f"({n / (ctrl_us * n / 1e6):.0f}/s)")

    if args.pipe:
        print(f"\n== pipelined fire-and-forget ({args.pipe} frames) ==")
        n = args.pipe
        frame = dev.build(GPIO_SET, GPIO, (PC0 << 8) | 1, echo=0)
        packets = n * ((len(frame) + EP_PACKET - 1) // EP_PACKET)
        handled0, dropped0 = dev.frame_stats()
        t0 = time.time()
        for _ in range(n):
            dev.send(frame)
        dt = time.time() - t0
        time.sleep(0.2)
        handled1, dropped1 = dev.frame_stats()
        done = handled1 - handled0
        print(f"{len(frame)} byte frames, {packets} EP2 OUT packets in "
              f"{dt * 1000:.0f} ms -> {packets / (dt * 1000):.2f} packets/ms")
        print(f"handled {done}/{n}, dropped {dropped1 - dropped0}")
        check("pipelined frames all handled", done, n)
        check("PC0 high after pipelined SETs",
              dev.request(GPIO_GET, GPIO, PC0 << 8)["value"], 1)
        dev.request(GPIO_SET, GPIO, (PC0 << 8) | 0)

    print("\n== mode switch ==")
    dev.frame_mode(0)
    flushed = b""
    deadline = time.time() + 0.3
    while time.time() < deadline:
        try:
            flushed += bytes(dev.ep3.read(EP_PACKET, timeout=200))
        except usb.core.USBTimeoutError:
            break

    # after leaving frame mode the raw EP1 -> EP3 echo has to work again
    ep1 = usb.util.find_descriptor(dev.dev.get_active_configuration()[(0, 0)],
                                   bEndpointAddress=EP1_OUT)
    ep1.write(b"\x11\x22\x33", timeout=1000)
    echo = b""
    deadline = time.time() + 1.0
    while len(echo) < 3 and time.time() < deadline:
        try:
            echo += bytes(dev.ep3.read(EP_PACKET, timeout=500))
        except usb.core.USBTimeoutError:
            break
    check("EP1 -> EP3 echo works again", echo, b"\x11\x22\x33")

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
