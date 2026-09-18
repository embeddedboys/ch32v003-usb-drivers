#!/usr/bin/env python3
"""AT24C256 (and compatible 24Cxx) test for the CH32V003 USB I2C bridge.

Covers the parts of the AT24C256 datasheet the bridge is expected to honour:

  1. address scan finds the device (0x50-0x57 with A2A1A0 wired)
  2. page aligned markers at spread out addresses verify absolute addressing
  3. ACK polling: after a write the device NACKs until its internal write cycle
     (tWR, 5 ms max) is done, so polling the device address measures the cycle
  4. the timeout path reports ST_TIMEOUT for an address that never answers
  5. soak: page safe random writes and reads, using ACK polling instead of a
     fixed delay, verified byte for byte

Page writes are 64 bytes and roll over *inside the same page*, so every write
here stays inside one page - a write that crosses a page boundary would
silently overwrite the start of that page.

Requires pyusb.  The device address defaults to 0x50, override with --addr.
"""

import random
import sys
import time

import usb.core

VID = 0x1209
PID = 0xC303

I2C, GENERIC = 0x03, 0x00
CONFIG = 0x50 | (I2C << 8)
WRITE = 0x51 | (I2C << 8)
WR = 0x57 | (I2C << 8)
GET_RX = 0x53 | (I2C << 8)
STATUS = 0x54 | (I2C << 8)
SCAN = 0x55 | (I2C << 8)
GET_SCAN = 0x56 | (I2C << 8)
WAIT_READY = 0x5D | (I2C << 8)
MEM_WRITE_READ = 0x5F | (I2C << 8)
GET_WAIT_US = 0x5E | (I2C << 8)
SEQ = 0x37 | (GENERIC << 8)
DROPS = 0x38 | (GENERIC << 8)

ST_OK = 1 << 0
ST_ADDR_NACK = 1 << 1
ST_TIMEOUT = 1 << 6

PAGE = 64
SIZE = 0x8000  # 32 KB

failures = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


def info(name, value):
    print(f"info {name}: {value}")


class Eeprom:
    def __init__(self, dev, addr):
        self.dev = dev
        self.addr = addr

    def ctrl(self, val, idx, length=0):
        return self.dev.ctrl_transfer(0x40 if length == 0 else 0xC0, 0x00, val,
                                      idx, length, timeout=3000)

    def out_data(self, idx, data, val=0):
        return self.dev.ctrl_transfer(0x40, 0x00, val, idx, bytes(data),
                                      timeout=3000)

    def u32(self, idx, val=0):
        return int.from_bytes(bytes(self.ctrl(val, idx, 4)), "little")

    # --- synchronisation -------------------------------------------------
    def sync(self, timeout=1.0):
        """The transfers run in the firmware main loop: settle the completion
        counter before taking a baseline, otherwise a still pending increment
        satisfies the next wait and the previous result is read."""
        last = self.u32(SEQ)
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(0.002)
            now = self.u32(SEQ)
            if now == last:
                return now
            last = now
        return last

    def wait_done(self, before, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.u32(SEQ) != before:
                return True
        raise TimeoutError("firmware never finished the request")

    # --- I2C -------------------------------------------------------------
    def write(self, addr, data):
        before = self.sync()
        self.out_data(WRITE, bytes([self.addr << 1, addr >> 8, addr & 0xFF]) +
                      bytes(data))
        self.wait_done(before)
        return self.u32(STATUS)

    def read(self, addr, count):
        before = self.sync()
        self.out_data(WR, bytes([self.addr << 1, addr >> 8, addr & 0xFF]),
                      val=count)
        self.wait_done(before)
        st = self.u32(STATUS)
        return st, bytes(self.ctrl(0, GET_RX, count))

    def wait_ready(self, timeout_ms=100):
        """ACK poll the device address; returns (status, microseconds)."""
        before = self.sync()
        self.out_data(WAIT_READY, bytes([self.addr << 1]), val=timeout_ms)
        self.wait_done(before)
        return self.u32(STATUS), self.u32(GET_WAIT_US)

    def mem_write_read(self, addr, data, read_count, poll=True, addr_len=2):
        """Whole memory cycle in one request: page write, ACK poll, read back."""
        before = self.sync()
        payload = (bytes([self.addr << 1]) +
                   (bytes([addr >> 8, addr & 0xFF]) if addr_len == 2
                    else bytes([addr & 0xFF])) + bytes(data))
        opts = (read_count & 0xFF) | ((1 if addr_len == 1 else 0) << 8) | \
               ((1 if poll else 0) << 9)
        self.out_data(MEM_WRITE_READ, payload, val=opts)
        self.wait_done(before)
        st = self.u32(STATUS)
        rx = bytes(self.ctrl(0, GET_RX, read_count)) if read_count else b""
        return st, rx, self.u32(GET_WAIT_US)

    def scan(self):
        found = []
        for base in range(0, 128, 32):
            before = self.sync()
            self.ctrl(base, SCAN)
            self.wait_done(before)
            bitmap = self.u32(GET_SCAN)
            found += [base + i for i in range(32) if bitmap & (1 << i)]
        return found


def main():
    addr7 = 0x50
    soak = 100
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--addr" and i + 1 < len(args):
            addr7 = int(args[i + 1], 0)
        if a == "--soak" and i + 1 < len(args):
            soak = int(args[i + 1])

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("FAIL: device not found")
        return 1

    e = Eeprom(dev, addr7)
    e.ctrl(50, CONFIG)
    e.sync()

    # --- 1. device present -------------------------------------------------
    found = e.scan()
    info("scan", [hex(a) for a in found] or "nothing")
    check(f"device at 0x{addr7:02x} answers", addr7 in found, True)
    if addr7 not in found:
        return 1

    # --- 2. absolute addressing -------------------------------------------
    addrs = [PAGE * i for i in (0, 1, 2, 4, 16, 32, 64, SIZE // PAGE - 1)]
    pats = [bytes([0xE0 + i]) * 8 for i in range(len(addrs))]
    for a, p in zip(addrs, pats):
        e.write(a, p)
        e.wait_ready()
    ok = 0
    for a, p in zip(addrs, pats):
        st, d = e.read(a, len(p))
        ok += (d == p)
        if d != p:
            print(f"FAIL 0x{a:04x} -> {d.hex()} want {p.hex()}")
    check("page aligned markers", ok, len(addrs))

    # --- 3. ACK polling measures the write cycle --------------------------
    cycles = []
    for i in range(30):
        e.write(0x1000 + (i * 4) % PAGE, bytes([i] * 4))
        st, us = e.wait_ready(200)
        if not (st & ST_OK):
            failures.append("wait_ready")
            print(f"FAIL wait_ready returned {st:#x}")
            break
        cycles.append(us)
    if cycles:
        cycles.sort()
        info("write cycle", f"min {cycles[0]} us, median "
                            f"{cycles[len(cycles) // 2]} us, max {cycles[-1]} us "
                            f"(datasheet says tWR <= 5000 us)")

    # --- 4. timeout path ---------------------------------------------------
    dead = next((a for a in range(0x08, 0x78) if a not in found), 0x08)
    e.addr = dead
    st, us = e.wait_ready(20)
    check(f"WAIT_READY times out on the dead address 0x{dead:02x}",
          bool(st & ST_TIMEOUT), True)
    info("timeout took", f"{us} us (asked for 20 ms)")
    e.addr = addr7

    # --- 5. soak, ACK polled instead of sleeping --------------------------
    rng = random.Random(7)
    bad = 0
    started = time.time()
    for i in range(soak):
        n = rng.choice((1, 2, 4, 8, 16, 32))
        page = rng.randrange(0, SIZE // PAGE)
        off = rng.randrange(0, PAGE)
        if off + n > PAGE:
            n = PAGE - off
        a = page * PAGE + off
        p = bytes(rng.randrange(256) for _ in range(n))
        stw = e.write(a, p)
        st, _ = e.wait_ready()
        str_, d = e.read(a, n)
        if d != p or not (stw & ST_OK) or not (str_ & ST_OK) or not (st & ST_OK):
            bad += 1
            if bad < 4:
                print(f"FAIL soak 0x{a:04x} n={n} wrote {p.hex()} read {d.hex()}")
    dt = time.time() - started
    check(f"soak {soak} page safe writes + reads (ACK polled)", bad, 0)
    info("soak rate", f"{dt / soak * 1000:.1f} ms per write+read round")
    check("no dropped control-OUT requests", e.u32(DROPS), 0)

    # --- 6. the whole memory cycle in one request -------------------------
    # AT24C256: 64 byte pages, so a full page fits in the request payload
    full = bytes(range(0x40))
    st, rx, us = e.mem_write_read(0x2000, full, 64)
    check("combined request wrote and read a full page", rx, full)
    check("combined request status ok", bool(st & ST_OK), True)
    info("combined request poll", f"{us} us")

    # a write followed by an immediate read must NOT be expected to work: the
    # device is still inside its write cycle and NACKs, which is exactly why the
    # poll exists.  Without the poll the request is a plain random read.
    st, rx, _ = e.mem_write_read(0x2020, b"", 4, poll=False)
    check("combined request without polling = random read", rx, full[0x20:0x24])

    st, _, _ = e.mem_write_read(0x2044, b"\x55\x66", 0)
    check("combined request without read back is accepted", bool(st & ST_OK), True)
    e.wait_ready()
    st, rx = e.read(0x2044, 2)
    check("write without read back landed", rx, b"\x55\x66")

    # --- 7. how much do the saved control transfers buy? ------------------
    rounds = 20
    started = time.time()
    for i in range(rounds):
        a = 0x3000 + (i % 16) * 4
        p = bytes([i & 0xFF] * 4)
        e.mem_write_read(a, p, 4)
    one = (time.time() - started) / rounds

    started = time.time()
    for i in range(rounds):
        a = 0x3100 + (i % 16) * 4
        p = bytes([i & 0xFF] * 4)
        e.write(a, p)
        e.wait_ready()
        e.read(a, 4)
    three = (time.time() - started) / rounds

    info("round cost (harness polling)", f"combined {one * 1000:.1f} ms vs "
          f"write+poll+read {three * 1000:.1f} ms ({three / one:.1f}x)")

    # same comparison without the harness' 2 ms sleep granularity, which
    # otherwise dominates both numbers
    def tight(op):
        before = e.u32(SEQ)
        op()
        while e.u32(SEQ) == before:
            pass
        return e.u32(STATUS)

    started = time.time()
    for i in range(rounds):
        out = []

        def op(a=0x3200 + (i % 16) * 4, p=bytes([i & 0xFF] * 4)):
            e.out_data(MEM_WRITE_READ,
                       bytes([e.addr << 1, a >> 8, a & 0xFF]) + p, val=4 | (1 << 9))
        tight(op)
    one_tight = (time.time() - started) / rounds

    started = time.time()
    for i in range(rounds):
        a = 0x3300 + (i % 16) * 4
        p = bytes([i & 0xFF] * 4)

        def w1(a=a, p=p):
            e.out_data(WRITE, bytes([e.addr << 1, a >> 8, a & 0xFF]) + p)

        def w2():
            e.out_data(WAIT_READY, bytes([e.addr << 1]), val=100)

        def w3(a=a):
            e.out_data(WR, bytes([e.addr << 1, a >> 8, a & 0xFF]), val=4)

        tight(w1)
        tight(w2)
        tight(w3)
    three_tight = (time.time() - started) / rounds
    info("round cost (tight polling)", f"combined {one_tight * 1000:.1f} ms vs "
          f"write+poll+read {three_tight * 1000:.1f} ms "
          f"({three_tight / one_tight:.1f}x)")

    if failures:
        print(f"\nFAILED: {len(failures)} check(s): {', '.join(failures)}")
        return 1

    print("\nALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
