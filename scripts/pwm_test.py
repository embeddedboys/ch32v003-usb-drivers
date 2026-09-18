#!/usr/bin/env python3
"""PWM test for the CH32V003 bridge: TIM1 channels 1 and 2 (PD2 and PA1).

There is no scope here, so the checks are the two things that can be measured
without one:

  - what the timer really does, read back from the firmware
    (`V003_PWM_GET` reports the period the prescaler and reload work out to),
    which is what caught a factor of 1000 in the cycle arithmetic;
  - the pin level at the duty extremes.  0 % must read low and 100 % high, and
    since the pin is driven by the timer while the level is read through the
    GPIO module, that also proves nothing else on the board is driving it.

A duty in between cannot be checked this way: the host would have to sample a
signal that is switching at 20 kHz to 1 kHz, which is exactly what a scope is
for.  `scripts/spi_test.py --loopback` has the same shape for the same reason.
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from v003_usb import find_device  # noqa: E402

PWM_MODULE = 0x05
PWM_SET = 0x70 | (PWM_MODULE << 8)
PWM_GET = 0x71 | (PWM_MODULE << 8)
PWM_GET_INFO = 0x72 | (PWM_MODULE << 8)
GET_CAPABILITIES = 0x3D
GPIO_GET = 0x07 | (0x01 << 8)

# channel -> pin, from the TIM1 default remap (vendor/pwm.h)
PWM_PINS = {0: 50, 1: 1}

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
        dev.ctrl_transfer(0x00, 0x09, 1, 0, 0, timeout=2000)

    def out_data(self, idx, data, val=0):
        return self.dev.ctrl_transfer(0x40, 0, val, idx, bytes(data),
                                      timeout=2000)

    def in_data(self, idx, length, val=0):
        return bytes(self.dev.ctrl_transfer(0xC0, 0, val, idx, length,
                                            timeout=2000))

    def u32(self, idx, val=0):
        return int.from_bytes(self.in_data(idx, 4, val), "little")

    def set_pwm(self, channel, period_ns, permille, enable=1):
        self.out_data(PWM_SET,
                      struct.pack("<BBHII", channel, enable, permille,
                                  period_ns, 0))

    def get_pwm(self, channel):
        channel, enable, duty, want, actual = struct.unpack(
            "<BBHII", self.in_data(PWM_GET, 12, val=channel))
        return channel, enable, duty, want, actual

    def pin(self, pin):
        return self.u32(GPIO_GET, pin << 8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--periods", default="50000,1000000,20000000,2000000000",
                    help="comma separated periods in ns to check")
    args = ap.parse_args()

    dev = Dev(find_device())

    # The state of a channel is whatever the last write left it in, which is not
    # something a test can assume (an earlier run configured it); what can be
    # checked is that the structure comes back whole, and that an out of range
    # channel is refused rather than read out of bounds.
    check("the channel state is 12 bytes",
          len(dev.in_data(PWM_GET, 12, val=0)), 12)
    check("an out of range channel returns nothing",
          len(dev.in_data(PWM_GET, 12, val=7)), 0)

    caps_raw = dev.in_data(GET_CAPABILITIES, 16)
    caps, ngpio, nadc, npwm, nuart, rlo, rhi = struct.unpack(
        "<IBBBBII", caps_raw)
    reserved = rlo | (rhi << 32)
    check("the capability report offers two PWM channels", npwm, 2)
    check("channel count agrees with the command",
          dev.u32(PWM_GET_INFO), npwm)
    for ch, pin in PWM_PINS.items():
        check(f"pin {pin} (channel {ch + 1}) is reserved", 
              bool(reserved & (1 << pin)), True)

    for period_ns in (int(p) for p in args.periods.split(",")):
        for ch, pin in PWM_PINS.items():
            dev.set_pwm(ch, period_ns, 1000)
            time.sleep(0.02)
            high = dev.pin(pin)
            dev.set_pwm(ch, period_ns, 0)
            time.sleep(0.02)
            low = dev.pin(pin)
            _, enable, _, want, actual = dev.get_pwm(ch)

            check(f"ch{ch} {period_ns} ns: duty 100% drives pin {pin} high",
                  high, 1)
            check(f"ch{ch} {period_ns} ns: duty 0% drives pin {pin} low",
                  low, 0)
            check(f"ch{ch} {period_ns} ns: reported period",
                  abs(actual - period_ns) <= max(1, period_ns // 10000), True)
            check(f"ch{ch} {period_ns} ns: echoed back in the state",
                  (enable, want), (1, period_ns))

    # leave the pins quiet
    for ch in PWM_PINS:
        dev.set_pwm(ch, 1000000, 0)
    for ch, pin in PWM_PINS.items():
        check(f"ch{ch} parked low", dev.pin(pin), 0)

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
