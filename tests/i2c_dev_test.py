#!/usr/bin/env python3
"""I2C test for the kernel side of the device: /dev/i2c-N of v003-i2c.ko.

Where scripts/i2c_test.py talks the vendor protocol over pyusb, this one goes
through the Linux I2C stack, so it covers the adapter (i2c_algorithm.master_xfer
-> control transfers -> firmware bit banging) and not just the firmware.

  .venv/bin/python tests/i2c_dev_test.py [--addr 0x50] [--scratch 0x1800]

The scratch page is written to, so point --scratch at an address you do not care
about (the test reads the original content first and puts it back).

Only ctypes and i2c-dev are needed; /dev/i2c-N has to be readable, which the
udev rule in the README arranges.
"""

import argparse
import ctypes
import ctypes.util
import glob
import os
import sys
import time

# i2c-dev uapi (linux/i2c-dev.h)
I2C_SLAVE = 0x0703
I2C_FUNCS = 0x0705
I2C_RDWR = 0x0707
I2C_FUNC_I2C = 0x00000001
I2C_FUNC_SMBUS_QUICK = 0x00010000
I2C_FUNC_SMBUS_READ_BYTE_DATA = 0x00080000
I2C_M_RD = 0x0001
I2C_M_TEN = 0x0010
I2C_M_NOSTART = 0x4000

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
libc.ioctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_void_p]
libc.ioctl.restype = ctypes.c_int


class I2CMsg(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("len", ctypes.c_uint16),
        ("buf", ctypes.POINTER(ctypes.c_uint8)),
    ]


class I2CRdwrData(ctypes.Structure):
    _fields_ = [
        ("msgs", ctypes.POINTER(I2CMsg)),
        ("nmsgs", ctypes.c_uint32),
    ]


def ioctl(fd, request, arg):
    if libc.ioctl(fd, request, arg) < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))


def ioctl_ret(fd, request, arg):
    ret = libc.ioctl(fd, request, arg)
    if ret < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    return ret


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


def find_bus():
    """the adapter v003-i2c.ko registered, by name rather than by number"""
    for path in sorted(glob.glob("/sys/class/i2c-dev/i2c-*")):
        name_file = os.path.join(path, "name")
        try:
            with open(name_file) as f:
                name = f.read().strip()
        except OSError:
            continue
        if "CH32V003" in name:
            return int(os.path.basename(path).split("-")[1]), name
    raise SystemExit("no CH32V003 i2c adapter found (is v003-i2c.ko loaded?)")


class Bus:
    def __init__(self, number):
        self.fd = os.open(f"/dev/i2c-{number}", os.O_RDWR)

    def funcs(self):
        value = ctypes.c_ulong(0)
        ioctl(self.fd, I2C_FUNCS, ctypes.byref(value))
        return value.value

    def transfer(self, msgs):
        """msgs = [(addr, flags, bytes), ...]; returns the read buffers"""
        keep = []
        array = (I2CMsg * len(msgs))()
        out = []

        for i, (addr, flags, data) in enumerate(msgs):
            buf = (ctypes.c_uint8 * max(len(data), 1))(*data)
            keep.append(buf)
            array[i].addr = addr
            array[i].flags = flags
            array[i].len = len(data)
            array[i].buf = ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint8))

        rdwr = I2CRdwrData(msgs=array, nmsgs=len(msgs))
        ioctl(self.fd, I2C_RDWR, ctypes.byref(rdwr))

        for i, (addr, flags, data) in enumerate(msgs):
            if flags & I2C_M_RD:
                out.append(bytes(keep[i][:len(data)]))
            else:
                out.append(None)

        return out

    def probe(self, addr7):
        """a zero length write: the firmware clocks only the address byte"""
        try:
            self.transfer([(addr7, 0, b"")])
            return 0
        except OSError as e:
            return e.errno


def eeprom_read(bus, addr7, word, count):
    msgs = [
        (addr7, 0, bytes([word >> 8, word & 0xFF])),
        (addr7, I2C_M_RD, b"\x00" * count),
    ]
    return bus.transfer(msgs)[1]


def eeprom_write(bus, addr7, word, data):
    bus.transfer([(addr7, 0, bytes([word >> 8, word & 0xFF]) + bytes(data))])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", type=lambda v: int(v, 0), default=0x50)
    ap.add_argument("--scratch", type=lambda v: int(v, 0), default=0x1800)
    args = ap.parse_args()

    number, name = find_bus()
    info("adapter", f"i2c-{number} ({name})")
    bus = Bus(number)

    funcs = bus.funcs()
    info("functionality", f"{funcs:#010x}")
    check("adapter advertises I2C_FUNC_I2C", bool(funcs & I2C_FUNC_I2C), True)
    check("adapter advertises SMBus quick (i2cdetect needs it)",
          bool(funcs & I2C_FUNC_SMBUS_QUICK), True)
    check("adapter advertises SMBus byte data",
          bool(funcs & I2C_FUNC_SMBUS_READ_BYTE_DATA), True)
    check("adapter does not claim 10 bit addresses",
          bool(funcs & 0x00000002), False)
    check("adapter does not claim SMBus block transfers",
          bool(funcs & 0x0F000000), False)

    # --- addressing --------------------------------------------------------
    check(f"device at 0x{args.addr:02x} acknowledges",
          bus.probe(args.addr), 0)
    err = bus.probe(0x51)
    info("probe of an empty address", f"errno {err} ({os.strerror(err)})")
    check("empty address is reported as ENXIO", err, 6)

    # --- what a real device answers ---------------------------------------
    page = eeprom_read(bus, args.addr, args.scratch & ~0x3F, 16)
    info(f"page 0x{args.scratch & ~0x3F:04x}", page.hex(" "))

    # --- write path -------------------------------------------------------
    original = eeprom_read(bus, args.addr, args.scratch, 16)
    info(f"scratch 0x{args.scratch:04x} before", original.hex(" "))

    # a marker this test owns, so it never depends on what an earlier session
    # happened to leave behind
    marker_word = args.scratch + 0x40
    marker_backup = eeprom_read(bus, args.addr, marker_word, 8)
    marker = bytes((0xC0 + i) for i in range(8))
    eeprom_write(bus, args.addr, marker_word, marker)
    time.sleep(0.01)
    check("marker written and read back",
          eeprom_read(bus, args.addr, marker_word, 8), marker)
    eeprom_write(bus, args.addr, marker_word, marker_backup)
    time.sleep(0.01)

    pattern = bytes((0x5A + i * 3) & 0xFF for i in range(16))
    eeprom_write(bus, args.addr, args.scratch, pattern)
    time.sleep(0.01)  # AT24C256 tWR is 5 ms at worst (measured ~330 us)
    back = eeprom_read(bus, args.addr, args.scratch, 16)
    check("16 byte write reads back", back, pattern)

    # a second write at an unaligned offset, the adapter must not care
    eeprom_write(bus, args.addr, args.scratch + 5, b"\xa5\x5a\x00\xff")
    time.sleep(0.01)
    check("unaligned 4 byte write reads back",
          eeprom_read(bus, args.addr, args.scratch + 5, 4),
          b"\xa5\x5a\x00\xff")

    # --- error paths ------------------------------------------------------
    try:
        eeprom_read(bus, 0x51, 0, 4)
        check("read from an empty address fails", "no error", "ENXIO")
    except OSError as e:
        info("read from an empty address", f"errno {e.errno} ({e.strerror})")
        check("read from an empty address fails", e.errno, 6)

    try:
        bus.transfer([(args.addr, I2C_M_TEN, b"\x00")])
        check("10 bit addressing is refused", "no error", "EOPNOTSUPP")
    except OSError as e:
        info("10 bit message", f"errno {e.errno} ({e.strerror})")
        check("10 bit addressing is refused", e.errno, 95)

    # --- restore ----------------------------------------------------------
    eeprom_write(bus, args.addr, args.scratch, original)
    time.sleep(0.01)
    check("scratch page restored",
          eeprom_read(bus, args.addr, args.scratch, 16), original)

    os.close(bus.fd)

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
