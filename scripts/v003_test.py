#!/usr/bin/env python3
"""End-to-end test for the CH32V003 vendor device (VID 1209, PID C303).

Covers:
  1. generic module: device version / serial number / EP stats
  2. endpoint OUT reception (EP1/EP2) via interrupt OUT writes
  3. endpoint IN transmission (EP3) - the firmware echoes EP1/EP2 OUT data
     back through the EP3 IN FIFO, which exercises the whole data path
  4. EP3 IN FIFO level and overflow accounting
  5. vendor control-OUT with a data stage

Requires pyusb, e.g.  python3 -m venv .venv && .venv/bin/pip install pyusb
"""

import sys
import time

import usb.core
import usb.util

VID = 0x1209
PID = 0xC303

V003_GENERIC_MODULE_ID = 0x00


def v003_cmd(cmd, mid):
    return cmd | (mid << 8)


def generic_cmd(cmd):
    return v003_cmd(cmd, V003_GENERIC_MODULE_ID)


GET_DEVICE_VER = generic_cmd(0x30)
GET_DEVICE_SN = generic_cmd(0x31)
GET_EP_STATS = generic_cmd(0x32)
GET_FIFO_LEVEL = generic_cmd(0x33)
GET_FIFO_DROPS = generic_cmd(0x34)
GET_REQ_DROPS = generic_cmd(0x35)
GET_CTRL_OUT_DATA = generic_cmd(0x36)

V003_SPI_MODULE_ID = 0x02
SPI_ENABLE = v003_cmd(0x40, V003_SPI_MODULE_ID)

V003_GPIO_MODULE_ID = 0x01
GPIO_SET = v003_cmd(0x06, V003_GPIO_MODULE_ID)
GPIO_GET = v003_cmd(0x07, V003_GPIO_MODULE_ID)
GPIO_REQUEST = v003_cmd(0x08, V003_GPIO_MODULE_ID)
GPIO_FREE = v003_cmd(0x09, V003_GPIO_MODULE_ID)
GPIO_GET_DIRECTION = v003_cmd(0x0A, V003_GPIO_MODULE_ID)
GPIO_DIRECTION_INPUT = v003_cmd(0x0B, V003_GPIO_MODULE_ID)
GPIO_DIRECTION_OUTPUT = v003_cmd(0x0C, V003_GPIO_MODULE_ID)

PC0 = 32

GPIO_LINE_DIRECTION_OUT = 0
GPIO_LINE_DIRECTION_IN = 1

EP1_OUT = 0x01
EP2_OUT = 0x02
EP3_IN = 0x83

EP3_FIFO_SIZE = 64

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
        self.ep1 = usb.util.find_descriptor(intf, bEndpointAddress=EP1_OUT)
        self.ep2 = usb.util.find_descriptor(intf, bEndpointAddress=EP2_OUT)
        self.ep3 = usb.util.find_descriptor(intf, bEndpointAddress=EP3_IN)
        if not (self.ep1 and self.ep2 and self.ep3):
            raise RuntimeError(
                f"endpoints not found: {self.ep1} {self.ep2} {self.ep3}")

    def ctrl(self, val, idx, length=0):
        """Vendor control transfer: wValue carries `val`, wIndex carries `idx`."""
        bm = 0x40 if length == 0 else 0xC0
        return self.dev.ctrl_transfer(bm, 0x00, val, idx, length, timeout=2000)

    def ctrl_out_data(self, idx, data, val=0):
        """Vendor control-OUT with a data stage."""
        return self.dev.ctrl_transfer(0x40, 0x00, val, idx, bytes(data),
                                      timeout=2000)

    def ctrl_in_data(self, idx, length, val=0):
        return bytes(self.ctrl(val, idx, length))

    def u32(self, idx, val=0):
        return int.from_bytes(bytes(self.ctrl(val, idx, 4)), "little")

    def ep_stats(self, which):
        return self.u32(GET_EP_STATS, which)

    def reset_stats(self):
        self.ctrl(0xFF, GET_EP_STATS, 0)

    def fifo_level(self):
        return self.u32(GET_FIFO_LEVEL)

    def fifo_drops(self):
        return self.u32(GET_FIFO_DROPS)

    def drain(self, count, timeout=2.0):
        """Read exactly `count` bytes from EP3 IN, one packet at a time."""
        out = b""
        deadline = time.time() + timeout
        while len(out) < count and time.time() < deadline:
            chunk = bytes(self.ep3.read(8, timeout=1000))
            if not chunk:
                continue
            out += chunk
        return out

    def gpio(self, pin):
        return self.u32(GPIO_GET, (pin << 8) | 0x00)

    def gpio_set(self, pin, value):
        self.ctrl((pin << 8) | value, GPIO_SET)

    def gpio_dir_output(self, pin, value):
        self.ctrl((pin << 8) | value, GPIO_DIRECTION_OUTPUT)

    def gpio_dir_input(self, pin):
        self.ctrl((pin << 8) | 0x00, GPIO_DIRECTION_INPUT)

    def gpio_direction(self, pin):
        return self.u32(GPIO_GET_DIRECTION, (pin << 8) | 0x00)

    def drain_fifo(self):
        """Drop whatever is left in the EP3 IN FIFO."""
        while self.fifo_level() > 0:
            if not self.ep3.read(8, timeout=1000):
                break


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("FAIL: device not found")
        return 1

    d = Device(dev)
    if d.dev.is_kernel_driver_active(0):
        d.dev.detach_kernel_driver(0)
        usb.util.claim_interface(d.dev, 0)

    # --- 1. generic module --------------------------------------------
    check("device version", d.u32(GET_DEVICE_VER), 0x1010)
    check("device serial", d.u32(GET_DEVICE_SN), 0x12345678)
    check("dropped vendor requests", d.u32(GET_REQ_DROPS), 0)

    # --- 2. EP1/EP2 OUT reception -------------------------------------
    # start from a known state: SPI off, FIFO drained, stats cleared
    d.ctrl(0, SPI_ENABLE)
    d.drain_fifo()
    d.reset_stats()
    check("fifo level after reset", d.fifo_level(), 0)

    ep1_payload = b"ep1data!"
    ep2_payload = b"Hello, World!"
    check("EP1 OUT write", d.ep1.write(ep1_payload), len(ep1_payload))
    check("EP2 OUT write", d.ep2.write(ep2_payload), len(ep2_payload))
    check("EP1 rx bytes", d.ep_stats(1), len(ep1_payload))
    check("EP2 rx bytes", d.ep_stats(2), len(ep2_payload))

    # --- 3. EP3 IN echo data path --------------------------------------
    expected = ep1_payload + ep2_payload
    check("fifo level after writes", d.fifo_level(), len(expected))
    check("EP3 IN echo", d.drain(len(expected)), expected)
    check("fifo level after drain", d.fifo_level(), 0)
    check("EP3 tx packets", d.ep_stats(3), 3)
    check("EP3 tx bytes", d.ep_stats(4), len(expected))

    # --- 4. FIFO overflow accounting -----------------------------------
    d.reset_stats()
    burst = bytes(range(0x40)) * 2  # 128 bytes -> 64 fit, 64 dropped
    d.ep2.write(burst)
    check("fifo level after burst", d.fifo_level(), EP3_FIFO_SIZE)
    check("fifo dropped bytes", d.fifo_drops(), len(burst) - EP3_FIFO_SIZE)
    check("fifo full drain", d.drain(EP3_FIFO_SIZE), burst[:EP3_FIFO_SIZE])
    check("fifo level after full drain", d.fifo_level(), 0)

    # --- 4b. FIFO wrap-around -------------------------------------------
    # The EP3 IN FIFO is a 64 byte ring: force head and tail to wrap past the
    # end and verify the byte stream is still in order.
    d.drain_fifo()
    d.reset_stats()
    first = bytes((0x10 + i) & 0xFF for i in range(40))
    second = bytes((0x80 + i) & 0xFF for i in range(40))
    d.ep2.write(first)
    check("wrap: level after first block", d.fifo_level(), len(first))
    check("wrap: first 24 bytes", d.drain(24), first[:24])
    d.ep2.write(second)
    expected_wrap = first[24:] + second
    check("wrap: level after second block", d.fifo_level(), len(expected_wrap))
    check("wrap: stream is in order", d.drain(len(expected_wrap)), expected_wrap)
    check("wrap: fifo empty", d.fifo_level(), 0)

    # --- 4c. streaming ---------------------------------------------------
    # Move a bigger payload through the EP path in FIFO sized chunks.
    rounds, chunk = 8, EP3_FIFO_SIZE
    stream = bytes((i * 13 + 5) & 0xFF for i in range(rounds * chunk))
    started = time.time()
    stream_ok = True
    for r in range(rounds):
        piece = stream[r * chunk:(r + 1) * chunk]
        d.ep1.write(piece)
        back = d.drain(chunk)
        if back != piece:
            stream_ok = False
            failures.append(f"streaming round {r}")
            print(f"FAIL streaming round {r}: {len(back)} bytes back")
            break
    elapsed = time.time() - started
    if stream_ok:
        print(f"ok   streaming {rounds * chunk} bytes through EP1 OUT -> EP3 IN "
              f"({len(stream) / elapsed / 1024:.1f} KiB/s host side)")
    d.drain_fifo()

    # --- 5. control-OUT data stage, read back with GET_CTRL_OUT_DATA -----
    # Every packet boundary is covered: short stages, exact 8 byte multiples
    # and a full 64 byte stage (8 EP0 packets).
    sweep = list(range(1, 17)) + [24, 31, 32, 33, 47, 63, 64]
    stage_failed = False
    for n in sweep:
        payload = bytes((i * 7 + n) & 0xFF for i in range(n))
        d.ctrl_out_data(GET_CTRL_OUT_DATA, payload, val=0x1234)
        back = d.ctrl_in_data(GET_CTRL_OUT_DATA, n)
        if back != payload:
            stage_failed = True
            failures.append(f"control-OUT stage {n}")
            print(f"FAIL control-OUT stage len {n}: got {back.hex()} "
                  f"expected {payload.hex()}")
            break

    if not stage_failed:
        print(f"ok   control-OUT data stage round trip "
              f"({len(sweep)} sizes, 1..64 bytes)")

    # a longer read must be clamped to what was actually received
    short = b"\xa5\x5a\x01\xfe\x7f"
    d.ctrl_out_data(GET_CTRL_OUT_DATA, short)
    check("control-OUT read clamped to received length",
          d.ctrl_in_data(GET_CTRL_OUT_DATA, 32), short)

    # --- 6. GPIO module -------------------------------------------------
    # V003_GPIO_REQUEST claims the line and puts it into a safe input mode
    # (input with pull-up/down), so a direction check right after it reports
    # "input" - exactly like the gpiolib request -> direction_output flow.
    d.ctrl((PC0 << 8) | 0x00, GPIO_REQUEST)
    check("PC0 direction after request", d.gpio_direction(PC0),
          GPIO_LINE_DIRECTION_IN)

    d.gpio_dir_output(PC0, 1)
    check("PC0 direction after direction_output", d.gpio_direction(PC0),
          GPIO_LINE_DIRECTION_OUT)
    check("PC0 reads back high", d.gpio(PC0), 1)
    d.gpio_dir_output(PC0, 0)
    check("PC0 reads back low", d.gpio(PC0), 0)

    d.gpio_dir_input(PC0)
    check("PC0 direction is input", d.gpio_direction(PC0),
          GPIO_LINE_DIRECTION_IN)
    info_value = d.gpio(PC0)
    print(f"info PC0 level as input: {info_value}")

    d.ctrl((PC0 << 8) | 0x00, GPIO_FREE)

    d.reset_stats()
    # --- 7. transport throughput (informational) -------------------------
    # EP3 IN is an interrupt endpoint with bInterval = 1, so it can move at
    # most one 8 byte packet per 1 ms frame (~8 KiB/s).  The control-OUT data
    # stage moves 64 bytes per transfer instead - measure both.
    rounds = 32
    payload = bytes(64)
    started = time.time()
    for _ in range(rounds):
        d.ctrl_out_data(GET_CTRL_OUT_DATA, payload)
        d.ctrl_in_data(GET_CTRL_OUT_DATA, len(payload))
    dt = time.time() - started
    print(f"info control-OUT/IN round trip: {2 * rounds * len(payload) / dt / 1024:.1f} KiB/s "
          f"({dt / rounds * 1000:.2f} ms per 64 byte round trip)")

    rounds = 32
    started = time.time()
    for _ in range(rounds):
        d.ep1.write(payload)
        d.drain(len(payload))
    dt = time.time() - started
    print(f"info EP OUT/IN round trip:      {2 * rounds * len(payload) / dt / 1024:.1f} KiB/s "
          f"({dt / rounds * 1000:.2f} ms per 64 byte round trip)")
    d.drain_fifo()

    if failures:
        print(f"\nFAILED: {len(failures)} check(s): {', '.join(failures)}")
        return 1

    print("\nALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
