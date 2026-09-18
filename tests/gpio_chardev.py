#!/usr/bin/env python3
"""GPIO character device test for the v003-usb-mfd gpiochip.

Uses the GPIO v2 uAPI directly through ioctl (no libgpiod needed, and none of
the deprecated sysfs interface that tests/gpio_sysfs.py used).  The ioctl
numbers are derived from the struct sizes below, so a layout mistake fails
cleanly in the kernel instead of corrupting memory.

    tests/gpio_chardev.py [--chip-label v003-gpio] [--out 32] [--in 36]

Requirements: the usb-mfd core and the v003-gpio child module loaded, and read
access to /dev/gpiochipN (the device nodes are group plugdev on this machine).
"""

import argparse
import ctypes
import fcntl
import glob
import os
import struct
import sys

GPIO_MAX_NAME_SIZE = 32
GPIO_V2_LINES_MAX = 64
GPIO_V2_LINE_NUM_ATTRS_MAX = 10

GPIO_V2_LINE_FLAG_USED = 1 << 0
GPIO_V2_LINE_FLAG_ACTIVE_LOW = 1 << 1
GPIO_V2_LINE_FLAG_INPUT = 1 << 2
GPIO_V2_LINE_FLAG_OUTPUT = 1 << 3

GPIO_V2_LINE_ATTR_ID_FLAGS = 1
GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES = 2

_IOC_NRBITS, _IOC_TYPEBITS, _IOC_SIZEBITS, _IOC_DIRBITS = 8, 8, 14, 2
_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT = _IOC_SIZESHIFT + _IOC_SIZEBITS
_IOC_NONE, _IOC_WRITE, _IOC_READ = 0, 1, 2


class gpiochip_info(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * GPIO_MAX_NAME_SIZE),
                ("label", ctypes.c_char * GPIO_MAX_NAME_SIZE),
                ("lines", ctypes.c_uint32)]


class gpio_v2_line_values(ctypes.Structure):
    _fields_ = [("bits", ctypes.c_uint64),
                ("mask", ctypes.c_uint64)]


class gpio_v2_line_attribute(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint32),
                ("padding", ctypes.c_uint32),
                ("value", ctypes.c_uint64)]


class gpio_v2_line_config_attribute(ctypes.Structure):
    _fields_ = [("attr", gpio_v2_line_attribute),
                ("mask", ctypes.c_uint64)]


class gpio_v2_line_config(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint64),
                ("num_attrs", ctypes.c_uint32),
                ("padding", ctypes.c_uint32 * 5),
                ("attrs", gpio_v2_line_config_attribute *
                 GPIO_V2_LINE_NUM_ATTRS_MAX)]


class gpio_v2_line_request(ctypes.Structure):
    _fields_ = [("offsets", ctypes.c_uint32 * GPIO_V2_LINES_MAX),
                ("consumer", ctypes.c_char * GPIO_MAX_NAME_SIZE),
                ("config", gpio_v2_line_config),
                ("num_lines", ctypes.c_uint32),
                ("event_buffer_size", ctypes.c_uint32),
                ("padding", ctypes.c_uint32 * 5),
                ("fd", ctypes.c_int32)]


def _IOC(direction, type_, nr, size):
    return ((direction << _IOC_DIRSHIFT) | (type_ << _IOC_TYPESHIFT) |
            (nr << _IOC_NRSHIFT) | (size << _IOC_SIZESHIFT))


GPIO_TYPE = 0xB4  # the GPIO ioctl type; not the ASCII 'B' (0x42)


def _IO(type_, nr):
    return _IOC(_IOC_NONE, type_, nr, 0)


def _IOR(type_, nr, st):
    return _IOC(_IOC_READ, type_, nr, ctypes.sizeof(st))


def _IOWR(type_, nr, st):
    return _IOC(_IOC_READ | _IOC_WRITE, type_, nr, ctypes.sizeof(st))


GPIO_V2_GET_CHIPINFO_IOCTL = _IOR(GPIO_TYPE, 0x01, gpiochip_info)
GPIO_V2_GET_LINE_IOCTL = _IOWR(GPIO_TYPE, 0x07, gpio_v2_line_request)
GPIO_V2_LINE_GET_VALUES_IOCTL = _IOWR(GPIO_TYPE, 0x0e, gpio_v2_line_values)
GPIO_V2_LINE_SET_VALUES_IOCTL = _IOWR(GPIO_TYPE, 0x0f, gpio_v2_line_values)

failures = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


def chip_info(fd):
    info = gpiochip_info()
    fcntl.ioctl(fd, GPIO_V2_GET_CHIPINFO_IOCTL, info)
    return info.name.decode(), info.label.decode(), info.lines


def find_chip(label_wanted):
    for path in sorted(glob.glob("/dev/gpiochip*"),
                       key=lambda p: int(p.replace("/dev/gpiochip", ""))):
        try:
            fd = os.open(path, os.O_RDONLY)
        except OSError:
            continue
        try:
            name, label, lines = chip_info(fd)
        finally:
            os.close(fd)
        print(f"info {path}: name={name!r} label={label!r} lines={lines}")
        if label_wanted in (label, name):
            return path, name, label, lines
    return None, None, None, None


def request_lines(fd, offsets, flags, consumer=b"v003-test"):
    req = gpio_v2_line_request()
    for i, off in enumerate(offsets):
        req.offsets[i] = off
    req.consumer = consumer
    req.num_lines = len(offsets)
    req.config.flags = flags
    fcntl.ioctl(fd, GPIO_V2_GET_LINE_IOCTL, req)
    return req.fd


def get_values(line_fd, num=1):
    vals = gpio_v2_line_values()
    vals.mask = (1 << num) - 1
    fcntl.ioctl(line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, vals)
    return vals.bits & vals.mask


def set_values(line_fd, values, mask):
    vals = gpio_v2_line_values()
    vals.bits = values
    vals.mask = mask
    fcntl.ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chip-label", default="v003-gpio")
    ap.add_argument("--out", type=int, default=32, help="output line offset")
    ap.add_argument("--in", dest="inp", type=int, default=35,
                    help="input line offset")
    ap.add_argument("--bench", type=int, default=0,
                    help="measure N set/get operations per line")
    ap.add_argument("--blink", type=int, default=0,
                    help="blink the output line N times (0.1 s on/off)")
    args = ap.parse_args()

    path, name, label, lines = find_chip(args.chip_label)
    if not path:
        print(f"FAIL: no gpiochip with label {args.chip_label!r} "
              f"(are usb-mfd and v003-gpio loaded?)")
        return 1

    check("gpiochip line count", lines, 56)

    fd = os.open(path, os.O_RDONLY)

    # output: drive a line and read the state back
    out_fd = request_lines(fd, [args.out], GPIO_V2_LINE_FLAG_OUTPUT)
    set_values(out_fd, 1, 1)
    check(f"line {args.out} reads back high", get_values(out_fd), 1)
    set_values(out_fd, 0, 1)
    check(f"line {args.out} reads back low", get_values(out_fd), 0)

    # input: just needs to be requestable and readable
    in_fd = request_lines(fd, [args.inp], GPIO_V2_LINE_FLAG_INPUT)
    value = get_values(in_fd)
    print(f"info input line {args.inp} level: {value}")

    if args.blink:
        import time as _t

        for _ in range(args.blink):
            set_values(out_fd, 1, 1)
            _t.sleep(0.1)
            set_values(out_fd, 0, 1)
            _t.sleep(0.1)
        print(f"info blinked line {args.out} {args.blink} times")

    if args.bench:
        import time as _t

        started = _t.time()
        for i in range(args.bench):
            set_values(out_fd, i & 1, 1)
        set_ms = (_t.time() - started) / args.bench * 1000

        started = _t.time()
        for _ in range(args.bench):
            get_values(out_fd)
        get_ms = (_t.time() - started) / args.bench * 1000

        print(f"info {args.bench} gpiolib ops: set {set_ms:.2f} ms "
              f"({1000 / set_ms:.0f}/s), get {get_ms:.2f} ms "
              f"({1000 / get_ms:.0f}/s)")

    # the pins the device itself uses must not be requestable
    for offset in (51, 52, 53, 54):
        try:
            bad_fd = request_lines(fd, [offset], GPIO_V2_LINE_FLAG_OUTPUT)
            os.close(bad_fd)
            check(f"reserved line {offset} is refused", "granted", "refused")
        except OSError as e:
            check(f"reserved line {offset} is refused",
                  e.errno in (22, 16, 19), True)  # EINVAL/EBUSY/ENODEV

    os.close(out_fd)
    os.close(in_fd)
    os.close(fd)

    if failures:
        print(f"\nFAILED: {len(failures)} check(s): {', '.join(failures)}")
        return 1

    print("\nALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
