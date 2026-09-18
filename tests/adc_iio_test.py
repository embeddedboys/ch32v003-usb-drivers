#!/usr/bin/env python3
"""ADC test for the kernel side of the device: the IIO device of v003-adc.ko.

Where scripts/adc_test.py talks the vendor protocol over pyusb, this one goes
through the Linux IIO interface, so it covers the driver (read_raw ->
V003_ADC_START + V003_ADC_GET with the completion tag) and the scale the driver
reports, not just the firmware.

  .venv/bin/python tests/adc_iio_test.py

Loading order, because the child driver needs symbols from both:

  sudo insmod usb-mfd.ko
  sudo insmod v003-adc.ko      # after the IIO core is available

The IIO core (`industrialio`) has to be loaded first, otherwise insmod fails with
`Unknown symbol devm_iio_device_alloc`.  It is usually a module of its own; on
this machine it ships compressed (`industrialio.ko.zst`), which `insmod` cannot
read, so either `modprobe industrialio` or

  zstdcat /lib/modules/$(uname -r)/kernel/drivers/iio/industrialio.ko.zst > /tmp/industrialio.ko
  sudo insmod /tmp/industrialio.ko

Everything here is plain sysfs: no dependency beyond the standard library.
"""

import argparse
import glob
import os
import sys

IIO_DEVICES = "/sys/bus/iio/devices"
DEVICE_NAME = "v003-adc"

CHANNELS = 10
VREF_CHANNEL = 8
VCAL_CHANNEL = 9
VREF_MV = 1200
AVDD_MV = 3300

failures = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


def check_range(name, got, lo, hi, unit=""):
    ok = lo <= got <= hi
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got}{unit}"
          + ("" if ok else f"  (expected {lo}..{hi}{unit})"))
    if not ok:
        failures.append(name)
    return ok


def read(path):
    with open(path) as fh:
        return fh.read().strip()


def find_device():
    """The IIO device the driver registered, found by name: the index is not
    something a test may assume (another IIO device may already be there)."""
    for path in sorted(glob.glob(f"{IIO_DEVICES}/iio:device*")):
        name = f"{path}/name"
        if os.path.exists(name) and read(name) == DEVICE_NAME:
            return path
    return None


def raw(dev, channel):
    return int(read(f"{dev}/in_voltage{channel}_raw"))


def scale_mv(dev, channel):
    return float(read(f"{dev}/in_voltage{channel}_scale"))


def label(dev, channel):
    return read(f"{dev}/in_voltage{channel}_label")


def mv(dev, channel):
    return raw(dev, channel) * scale_mv(dev, channel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int, default=3,
                    help="reads per channel for the stability check")
    args = ap.parse_args()

    dev = find_device()
    if not dev:
        print(f"FAIL no IIO device named {DEVICE_NAME} - is v003-adc.ko loaded?")
        print("     (load order: usb-mfd.ko, then v003-adc.ko, after industrialio)")
        return 1

    print(f"info {dev}")

    # -- the interface the driver is supposed to expose ----------------

    for channel in range(CHANNELS):
        for what in ("raw", "scale", "label"):
            path = f"{dev}/in_voltage{channel}_{what}"
            if not os.path.exists(path):
                failures.append(f"channel {channel} has no {what}")
    check(f"all {CHANNELS} channels expose raw, scale and label",
          [f for f in failures if "has no"][:1], [])

    check("scale is AVDD / 1024", scale_mv(dev, 0), AVDD_MV / 1024)

    # -- labels say which channel is which ----------------------------

    labels = [label(dev, c) for c in range(CHANNELS)]
    print(f"     labels: {labels}")
    check("the internal reference is labelled",
          "Vref" in labels[VREF_CHANNEL], True)
    check("the calibration voltage is labelled",
          "Vcal" in labels[VCAL_CHANNEL], True)
    check("channel 1 is labelled with its pin (measured as PA1)",
          "PA1" in labels[1], True)
    check("every channel has a label", all(labels), True)

    # -- readings are 10 bit and repeatable ---------------------------

    values = {c: [raw(dev, c) for _ in range(args.repeats)]
              for c in range(CHANNELS)}
    for channel, raws in values.items():
        check_range(f"channel {channel} stays inside 10 bits", max(raws), 0, 1023)

    spreads = {c: max(v) - min(v) for c, v in values.items()}
    print("     spread over "
          + f"{args.repeats} reads per channel: "
          + ", ".join(f"ch{c}={spreads[c]}" for c in range(CHANNELS)))
    # Only the two internal channels are asserted on: a floating input moves,
    # that is what a floating input does, and pinning a tolerance on it would be
    # a test that fails for the wrong reason.
    for channel in (VREF_CHANNEL, VCAL_CHANNEL):
        check_range(f"channel {channel} repeats to within a few counts",
                    spreads[channel], 0, 8, " counts")

    # -- the internal reference: the one absolute truth ----------------

    vref = [raw(dev, VREF_CHANNEL) for _ in range(args.repeats)]
    vref_mv = vref[0] * scale_mv(dev, VREF_CHANNEL)
    print(f"     channel {VREF_CHANNEL} (Vref): {vref} = {vref_mv:.0f} mV "
          f"(nominal {VREF_MV} mV)")
    for value in vref:
        check_range(f"Vref reading {value} is a 1.2 V reference",
                    round(value * scale_mv(dev, VREF_CHANNEL)), 1080, 1320,
                    " mV")

    # -- the calibration voltage, and the values pyusb reads -----------

    vcal_mv = mv(dev, VCAL_CHANNEL)
    print(f"     channel {VCAL_CHANNEL} (Vcal): {vcal_mv:.0f} mV "
          f"(2/4 AVDD nominal {AVDD_MV / 2:.0f} mV)")
    check_range("Vcal reads half the supply", round(vcal_mv), 1500, 1800, " mV")

    print("     all channels: "
          + ", ".join(f"ch{c}={mv(dev, c):.0f}mV" for c in range(CHANNELS)))

    # -- an attribute that must not be writable -----------------------

    try:
        with open(f"{dev}/in_voltage0_raw", "w") as fh:
            fh.write("1")
        check("writing a raw value is refused", "accepted", "refused")
    except OSError:
        check("writing a raw value is refused", "refused", "refused")

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
