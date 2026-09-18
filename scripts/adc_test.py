#!/usr/bin/env python3
"""ADC test for the CH32V003 bridge: ADC1, 10 bit, one conversion per request.

The chip gives two internal channels that make this module verifiable without
wiring anything up, and the package gives one external channel this board can
drive by itself:

  - channel 8 is the internal reference, nominally 1.2 V against a 3.3 V full
    scale, so about 365 counts.  It is the check for "is the whole path
    plausible at all";
  - channel 9 is the internal calibration voltage, selectable between 2/4 and
    3/4 of AVDD (RM 9.3).  A change the host commands and the reading follows is
    what distinguishes a live conversion from a cached value - the same idea as
    the completion tag, measured instead of trusted;
  - channel 1 is PA1, which is also TIM1 channel 2 on this board, so driving
    that pin from the PWM module and reading it back through the ADC turns a
    GPIO into a ground truth of 0 V and AVDD.

Readings are converted with reading * AVDD / 1024, AVDD = 3.3 V, which is the
10 bit scaling of RM 9.2.2 ("规则组通道的数据寄存器 ADC_RDATAR 保存的是实际转换
的 10 位数字值").  Scaling as 12 bit reported 294 mV for the 1.2 V reference.
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from v003_usb import find_device  # noqa: E402

ADC_MODULE = 0x06
ADC_START = 0x80 | (ADC_MODULE << 8)
ADC_GET = 0x81 | (ADC_MODULE << 8)
ADC_GET_INFO = 0x82 | (ADC_MODULE << 8)
ADC_GET_SEQ = 0x83 | (ADC_MODULE << 8)
ADC_GET_STATUS = 0x84 | (ADC_MODULE << 8)
ADC_SET_CALVOL = 0x85 | (ADC_MODULE << 8)

GET_CAPABILITIES = 0x3D
CAP_ADC = 1 << 3
CAP_PWM = 1 << 4

PWM_SET = 0x70 | (0x05 << 8)
PWM_CH2_PIN = 1  # PA1, the pin ADC channel 1 samples

ADC_CHANNELS = 10
ADC_BITS = 10
ADC_MAX = (1 << ADC_BITS) - 1
ADC_VREF_CHANNEL = 8
ADC_VCAL_CHANNEL = 9
ADC_AVDD_MV = 3300

CALVOL_HALF = 0
CALVOL_3Q = 1

failures = []
skips = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


def check_range(name, got, lo, hi):
    ok = lo <= got <= hi
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {lo}..{hi})"))
    if not ok:
        failures.append(name)
    return ok


def skip(name, why):
    print(f"skip {name}: {why}")
    skips.append(name)


class Dev:
    def __init__(self, dev):
        self.dev = dev
        try:
            dev.set_configuration()
        except usb.core.USBError:
            pass
        # resync the endpoint toggles: without this the first request can be
        # dropped and every check after it reads a stale answer
        dev.ctrl_transfer(0x00, 0x09, 1, 0, 0, timeout=2000)

    def out(self, idx, val=0):
        return self.dev.ctrl_transfer(0x40, 0, val, idx, 0, timeout=2000)

    def out_data(self, idx, data, val=0):
        return self.dev.ctrl_transfer(0x40, 0, val, idx, bytes(data),
                                      timeout=3000)

    def in_data(self, idx, length, val=0):
        return bytes(self.dev.ctrl_transfer(0xC0, 0, val, idx, length,
                                            timeout=2000))

    def u32(self, idx, val=0):
        return int.from_bytes(self.in_data(idx, 4, val), "little")

    # -- the module ----------------------------------------------------

    def start(self, channel):
        self.out(ADC_START, channel)

    def get(self):
        """One read: (raw, tag, valid)."""
        val = self.u32(ADC_GET)
        return val & ADC_MAX, (val >> 16) & 0xff, (val >> 15) & 1

    def convert(self, channel):
        self.start(channel)
        raw, _, _ = self.get()
        return raw

    def status(self):
        val = self.u32(ADC_GET_STATUS)
        return {"err": val >> 24, "us": (val >> 16) & 0xff,
                "reqs": (val >> 8) & 0xff, "conv": val & 0xff}

    def seq(self):
        return self.u32(ADC_GET_SEQ)

    def set_calvol(self, level):
        self.out(ADC_SET_CALVOL, level)

    def mv(self, raw):
        return raw * ADC_AVDD_MV / (ADC_MAX + 1)

    # -- other modules -------------------------------------------------

    def set_pwm(self, channel, period_ns, permille, enable=1):
        self.out_data(PWM_SET,
                      struct.pack("<BBHII", channel, enable, permille,
                                  period_ns, 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int, default=3,
                    help="conversions per fixed reading (default 3)")
    ap.add_argument("--no-pwm", action="store_true",
                    help="skip the PWM driven external channel check")
    args = ap.parse_args()

    dev = Dev(find_device())

    caps_raw = dev.in_data(GET_CAPABILITIES, 16)
    caps, _ngpio, nadc, _npwm, _nuart, _rlo, _rhi = struct.unpack(
        "<IBBBBII", caps_raw)

    if not caps & CAP_ADC:
        print("FAIL the capability report does not offer an ADC")
        return 1

    check("the capability report offers ten ADC channels", nadc, ADC_CHANNELS)

    info = dev.u32(ADC_GET_INFO)
    check("resolution and channel count", (info >> 16, info & 0xffff),
          (ADC_BITS, ADC_CHANNELS))

    # -- the internal reference: is the path plausible at all ----------

    before = dev.status()
    print(f"     status before: {before}")

    vref = [dev.convert(ADC_VREF_CHANNEL) for _ in range(args.repeats)]
    print(f"     channel {ADC_VREF_CHANNEL} (Vref) raw: {vref}"
          f" = {dev.mv(vref[0]):.0f} mV")
    for raw in vref:
        # the part is specified for 1.2 V nominal; allow +-10 % for the
        # reference tolerance before calling the conversion wrong
        check_range(f"Vref reading {raw} is a 1.2 V reference",
                    round(dev.mv(raw)), 1080, 1320)

    # -- the calibration voltage: a change the host commands -----------

    lvls = []
    for lvl in (CALVOL_HALF, CALVOL_3Q, CALVOL_HALF):
        dev.set_calvol(lvl)
        raw = dev.convert(ADC_VCAL_CHANNEL)
        lvls.append((lvl, raw))
        print(f"     channel {ADC_VCAL_CHANNEL} (Vcal) calvol={lvl} raw: "
              f"{raw} = {dev.mv(raw):.0f} mV")

    check_range("2/4 AVDD reads 1650 mV", round(dev.mv(lvls[0][1])),
                1500, 1800)
    check_range("3/4 AVDD reads 2475 mV", round(dev.mv(lvls[1][1])),
                2300, 2650)
    check_range("selecting 3/4 AVDD moved the reading by ~825 mV",
                round(dev.mv(lvls[1][1]) - dev.mv(lvls[0][1])), 700, 950)

    # -- the external channel, driven by the PWM module ----------------

    if not caps & CAP_PWM:
        skip("channel 1 follows PA1", "this build has no PWM module")
    elif args.no_pwm:
        skip("channel 1 follows PA1", "--no-pwm")
    else:
        try:
            dev.set_pwm(1, 1000000, 1000)
            time.sleep(0.02)
            high = dev.convert(1)
            dev.set_pwm(1, 1000000, 0)
            time.sleep(0.02)
            low = dev.convert(1)
            print(f"     channel 1 (PA1) raw: PWM 100% -> {high}"
                  f" ({dev.mv(high):.0f} mV), PWM 0% -> {low}"
                  f" ({dev.mv(low):.0f} mV)")
            check_range("PA1 at 100 % reads AVDD", high, ADC_MAX - 10, ADC_MAX)
            check_range("PA1 at 0 % reads ground", low, 0, 10)
        finally:
            dev.set_pwm(1, 1000000, 0)

    # -- the completion tag: a fresh answer, not a cached one ----------

    raw_ref, last_tag, valid = dev.get()
    check("a conversion has happened", valid, 1)
    # reading again without asking for a conversion must not move the tag
    tags = [dev.get()[1] for _ in range(3)]
    check("the tag only moves when a conversion runs", tags,
          [last_tag, last_tag, last_tag])

    dev.start(ADC_VREF_CHANNEL)
    raw, tag, valid = dev.get()
    check("one read after START carries the new tag",
          tag, (last_tag + 1) & 0xff)
    check_range("and it is a Vref conversion, not the previous result", raw,
                vref[0] - 6, vref[0] + 6)
    check("the result is marked valid", valid, 1)

    # -- a rejected request must not be counted as a conversion --------

    seq0 = dev.seq()
    raw_before, tag_before, valid_before = dev.get()
    dev.start(ADC_CHANNELS)  # one past the last channel
    time.sleep(0.05)
    check("an out of range channel is not converted", dev.seq(), seq0)
    raw, tag, valid = dev.get()
    check("and the last good result is untouched",
          (raw, tag, valid), (raw_before, tag_before, valid_before))

    # -- diagnostics have to agree with the counters -------------------

    after = dev.status()
    print(f"     status after: {after}")
    check("no conversion timed out", after["err"], 0)
    # 241 sample clocks + 11 conversion clocks at 6 MHz
    check_range("a conversion takes about 42 us", after["us"], 30, 60)
    check("the request that asked for an out of range channel was seen",
          (after["reqs"] - before["reqs"]) - (after["conv"] - before["conv"]),
          1)

    # -- leave the module the way it was found (2/4 AVDD) --------------

    dev.set_calvol(CALVOL_HALF)
    print(f"     channel {ADC_VCAL_CHANNEL} parked back at 2/4 AVDD: "
          f"{dev.convert(ADC_VCAL_CHANNEL)}")

    print()
    if skips:
        print(f"{len(skips)} skipped: {', '.join(skips)}")
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except usb.core.USBError as exc:
        print(f"FAIL usb: {exc}")
        sys.exit(1)
