#!/usr/bin/env python3
"""I2C bridge test for the CH32V003 vendor device (ids from lib/v003_usb_ids.h).

The firmware bit bangs an I2C master on PC1 (SDA) / PC2 (SCL), both open drain
with external pull-ups, and exposes it through the control transfer data stages:

    V003_I2C_CONFIG      OUT, wValue = half period -> configure/release the bus
    V003_I2C_WRITE       OUT, payload [addr<<1][bytes...]
    V003_I2C_READ        OUT, payload [addr<<1|1][count]
    V003_I2C_GET_RX      IN  -> the bytes read by the last READ
    V003_I2C_GET_STATUS  IN  -> OK / ADDR_NACK / DATA_NACK / STRETCH / bytes
    V003_I2C_SCAN        OUT, wValue = first address -> probe 32 addresses
    V003_I2C_GET_SCAN    IN  -> ACK bitmap of that scan

Checks:
  1. bus idle levels (both lines high through the pull-ups), config round trip
  2. address scan finds the attached device (a BH1750 at 0x23 or 0x5C)
  3. a transfer to an unused address reports ADDR_NACK instead of hanging
  4. BH1750: power on, continuous H-resolution mode, then read lux over and over

Requires pyusb and, for the sensor checks, a BH1750 wired to PC1/PC2.

A bit banged master on jumper wires misreads an ACK slot roughly once in 300
transactions (0 on the sensor, ~0.3% when probing an address nobody answers),
so the single shot checks retry before they fail.
"""

import struct
import sys
import time

import usb.core
import usb.util

from v003_usb import VID, PID

GENERIC, GPIO, SPI, I2C = 0x00, 0x01, 0x02, 0x03
GET_FIFO_LEVEL = 0x33 | (GENERIC << 8)
GET_CTRL_OUT_SEQ = 0x37 | (GENERIC << 8)
GPIO_GET = 0x07 | (GPIO << 8)
GPIO_SET = 0x06 | (GPIO << 8)
GPIO_DIRECTION_INPUT = 0x0B | (GPIO << 8)
I2C_CONFIG = 0x50 | (I2C << 8)
I2C_WRITE = 0x51 | (I2C << 8)
I2C_READ = 0x52 | (I2C << 8)
I2C_GET_RX = 0x53 | (I2C << 8)
I2C_GET_STATUS = 0x54 | (I2C << 8)
I2C_SCAN = 0x55 | (I2C << 8)
I2C_GET_SCAN = 0x56 | (I2C << 8)
I2C_WRITE_READ = 0x57 | (I2C << 8)
I2C_GET_CFG = 0x59 | (I2C << 8)

ST_OK = 1 << 0
ST_ADDR_NACK = 1 << 1
ST_DATA_NACK = 1 << 2
ST_STRETCH = 1 << 3
ST_NOT_CONFIGURED = 1 << 4
ST_READ_NACK = 1 << 5

PC1 = 33  # SDA
PC2 = 34  # SCL
PC4 = 36  # free pin, used to prove the internal pull-up/pull-down works

BH1750_POWER_ON = 0x01
BH1750_CONT_H_RES = 0x10

failures = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


def describe(status):
    if not status:
        return "no status"
    bits = []
    for bit, name in ((ST_OK, "ok"), (ST_ADDR_NACK, "addr-nack"),
                      (ST_DATA_NACK, "data-nack"), (ST_STRETCH, "stretch"),
                      (ST_NOT_CONFIGURED, "not-configured")):
        if status & bit:
            bits.append(name)
    bits.append(f"{status >> 8 & 0xFF} bytes")
    if status & ST_DATA_NACK:
        bits.append(f"failed at byte {status >> 16 & 0xFF}")
    return ", ".join(bits)


class Device:
    def __init__(self, dev):
        self.dev = dev

    def ctrl(self, val, idx, length=0):
        bm = 0x40 if length == 0 else 0xC0
        return self.dev.ctrl_transfer(bm, 0x00, val, idx, length, timeout=3000)

    def u32(self, idx, val=0):
        return int.from_bytes(bytes(self.ctrl(val, idx, 4)), "little")

    def ctrl_out_data(self, idx, data):
        return self.dev.ctrl_transfer(0x40, 0x00, 0, idx, bytes(data),
                                      timeout=3000)

    def ctrl_in_data(self, idx, length):
        return bytes(self.ctrl(0, idx, length))

    # --- I2C -----------------------------------------------------------
    def seq(self):
        """Counter the firmware main loop advances per handled OUT request."""
        return self.u32(GET_CTRL_OUT_SEQ)

    def sync(self, timeout=1.0):
        """Wait until no request is pending, and return the settled counter.

        Taking the baseline right after issuing another request made an earlier
        revision read results one transaction late: the other request's pending
        increment satisfied the wait.  Always settle before taking a baseline.
        """
        last = self.seq()
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(0.002)
            now = self.seq()
            if now == last:
                return now
            last = now
        return last

    def wait_done(self, before, timeout=2.0):
        """Transfers run in the firmware main loop, so the result is only valid
        once the completion counter has moved past `before`."""
        end = time.time() + timeout
        while time.time() < end:
            if self.seq() != before:
                return True
        raise TimeoutError("firmware never finished the request")

    def config(self, half_100ns=50):
        """half_100ns is the half period in 100 ns units (50 ~= 100 kHz)."""
        self.ctrl(half_100ns, I2C_CONFIG)
        self.sync()

    def status(self):
        return self.u32(I2C_GET_STATUS)

    def write(self, addr7, data):
        before = self.sync()
        self.ctrl_out_data(I2C_WRITE, bytes([(addr7 << 1) | 0]) + bytes(data))
        self.wait_done(before)
        return self.status()

    def read(self, addr7, count):
        before = self.sync()
        self.ctrl_out_data(I2C_READ, bytes([(addr7 << 1) | 1, count]))
        self.wait_done(before)
        st = self.status()
        data = self.ctrl_in_data(I2C_GET_RX, count)
        return st, data

    def write_read(self, addr7, data, count):
        """Write `data`, repeated START, read `count` bytes back."""
        before = self.sync()
        payload = bytes([(addr7 << 1) | 0]) + bytes(data)
        self.dev.ctrl_transfer(0x40, 0x00, count, I2C_WRITE_READ, payload,
                               timeout=3000)
        self.wait_done(before)
        st = self.status()
        return st, self.ctrl_in_data(I2C_GET_RX, count)

    def scan(self, base):
        before = self.sync()
        self.ctrl(base, I2C_SCAN)
        self.wait_done(before)
        return self.u32(I2C_GET_SCAN)

    def scan_all(self):
        found = []
        for base in range(0, 128, 32):
            bitmap = self.scan(base)
            for i in range(32):
                if bitmap & (1 << i):
                    found.append(base + i)
        return found

    def gpio(self, pin):
        return self.u32(GPIO_GET, (pin << 8))

    def gpio_set(self, pin, value):
        self.ctrl((pin << 8) | value, GPIO_SET)

    def gpio_dir_input(self, pin):
        self.ctrl((pin << 8) | 0x00, GPIO_DIRECTION_INPUT)

    def cfg(self):
        return self.u32(I2C_GET_CFG)  # half period in microseconds


def read_bh1750(d, addr7):
    st, raw = d.read(addr7, 2)
    if st & ST_ADDR_NACK or len(raw) != 2:
        return st, None
    raw_value = (raw[0] << 8) | raw[1]
    return st, raw_value / 1.2


def main():
    watch = 0
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--watch" and i + 1 < len(args):
            watch = int(args[i + 1])

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("FAIL: device not found")
        return 1
    d = Device(dev)

    # --- 1. bus idle levels ---------------------------------------------
    d.config()
    check("SDA idles high (pull-up)", d.gpio(PC1), 1)
    check("SCL idles high (pull-up)", d.gpio(PC2), 1)
    check("status after config", d.status(), 0)

    # --- 2. scan ---------------------------------------------------------
    found = d.scan_all()
    print(f"info I2C scan found: {[hex(a) for a in found] or 'nothing'}")
    if not found:
        print("warn no I2C device answered - is it wired to PC1(SDA)/PC2(SCL)?")
    else:
        print(f"ok   scan found {len(found)} device(s)")

    # --- 3. NACK path -----------------------------------------------------
    # A bit banged master on jumper wires can misread an ACK slot once in a few
    # hundred transactions (measured ~0.3% when probing an address nobody
    # answers), so retry before declaring the NACK handling broken.
    unused = next((a for a in range(0x08, 0x78) if a not in found), None)
    if unused is not None:
        nacked = False
        for _ in range(5):
            st = d.write(unused, b"\x00")
            if st & ST_ADDR_NACK:
                nacked = True
                break
        check(f"write to unused 0x{unused:02x} reports ADDR_NACK", nacked, True)

        nacked = False
        for _ in range(5):
            st, _ = d.read(unused, 1)
            if st & ST_ADDR_NACK:
                nacked = True
                break
        check(f"read from unused 0x{unused:02x} reports ADDR_NACK", nacked, True)

    # also make sure a real transfer is not falsely flagged
    if found:
        st = d.write(found[0], b"")
        check("address-only write is ACKed", bool(st & ST_OK), True)

    # --- 4. BH1750 --------------------------------------------------------
    sensor = next((a for a in found if a in (0x23, 0x5C)), None)
    if sensor is None:
        print("warn no BH1750 (0x23/0x5C) on the bus - skipping the lux read")
    else:
        print(f"info BH1750 found at 0x{sensor:02x}")
        st = d.write(sensor, [BH1750_POWER_ON])
        check("BH1750 power on", bool(st & ST_OK), True)
        st = d.write(sensor, [BH1750_CONT_H_RES])
        check("BH1750 continuous H-resolution", bool(st & ST_OK), True)

        # The register still holds 0xffff until the first conversion (typ. 120
        # ms) landed, so wait it out and throw that sample away.
        time.sleep(0.3)
        read_bh1750(d, sensor)

        rounds = watch if watch else 5
        samples = []
        for i in range(rounds):
            if watch:
                time.sleep(1.0)
            else:
                time.sleep(0.2)
            st, lux = read_bh1750(d, sensor)
            if lux is None:
                print(f"FAIL BH1750 read {i}: {describe(st)}")
                failures.append(f"BH1750 read {i}")
                break
            samples.append(lux)

        if samples:
            check("BH1750 reads all ACKed", len(samples), rounds)
            print(f"ok   illuminance: {min(samples):.1f} - {max(samples):.1f} lx "
                  f"(median {sorted(samples)[len(samples) // 2]:.1f}, "
                  f"{len(samples)} samples)")
            if watch:
                for s in samples:
                    print(f"     {s:8.1f} lx")

    # --- 5. write + repeated START read -----------------------------------
    if sensor is not None:
        # An address only write followed by a repeated START read: the idiom
        # register based devices need, and a legal way to read a BH1750 too.
        st, rx = d.write_read(sensor, b"", 2)
        check("repeated START read ACKed", bool(st & ST_OK), True)
        check("repeated START read length", len(rx), 2)

        # it must agree with the two transaction sequence
        st2, rx2 = d.read(sensor, 2)
        v1 = int.from_bytes(rx, "big") / 1.2
        v2 = int.from_bytes(rx2, "big") / 1.2
        close = abs(v1 - v2) <= max(1.0, 0.1 * max(v1, v2))
        check("repeated START agrees with write+read", close, True)
        print(f"info repeated START {v1:.1f} lx vs two transactions {v2:.1f} lx")

        # write phase carrying data (the register address idiom), then read.
        # Retried: a bit banged master on jumper wires misreads an ACK slot
        # about once in 300 transactions, and this is a single shot check.
        st = None
        moved = 0
        for _ in range(5):
            st, rx = d.write_read(sensor, [BH1750_CONT_H_RES], 2)
            moved = (st >> 8) & 0xFF
            if (st & ST_OK) and moved == 3:
                break
        check("repeated START with a written byte", bool(st & ST_OK), True)
        check("repeated START moved 1 write + 2 read bytes", moved, 3)
        # NOTE: the BH1750 NACKs a *second* consecutive data byte in one
        # transaction (verified: DATA_NACK at byte 1), so it only accepts one
        # command byte per transaction - that is the device, not the bridge.

        # a write phase to a dead address must NACK, not read
        for _ in range(5):
            st, _ = d.write_read(0x08, b"\x00", 2)
            if st & ST_ADDR_NACK:
                break
        check("repeated START to a dead address NACKs", bool(st & ST_ADDR_NACK),
              True)

    if failures:
        print(f"\nFAILED: {len(failures)} check(s): {', '.join(failures)}")
        return 1

    print("\nALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
