#!/usr/bin/env python3
"""One command that asks the device what state it is in.

Every line here is a diagnostic this project reached for while bringing the
modules up, gathered in one place because the alternative was rewriting the same
twenty lines of pyusb each time.  It changes nothing on the device except where
noted, and it needs the kernel modules unloaded (they own the interface).

  tools/status.py                  # version, capabilities, budgets, module state
  tools/status.py --pins           # also print the reserved pin mask as ranges
  tools/status.py --reset-cause    # the cause of the last reset (answered once)

What each line is for:

  * **capabilities** - which modules this firmware was built with, their channel
    counts and the pins the device owns, reported by the device (0x3d) instead of
    assumed by the host;
  * **stack margin** - `GET_STACK_FREE` (0x3b) reports the deepest stack use the
    canary has seen, so a fresh boot is optimistic and a run that has exercised
    every module is the number to plan against.  The first read arms a refresh and
    answers with the *previous* value, so this reads it twice - the second read is
    the measurement.  350 bytes is the documented floor (`notes/firmware.md`);
  * **conversion time** - the ADC's own read back (42 us documented); a value four
    times smaller means `RCC->CFGR0` was rewritten behind the module's back, which
    is what a standby wake does (`notes/debugging.md`, case 14);
  * **sleeps** - the power module's count and the reason for the last wake, which
    is how a sleep is verified without a scope;
  * **watchdog** - whether the IWDG is running and the timeout it actually has
    (the LSI is only good to +-20 %, so the requested value would be a lie);
  * **reset cause** - the firmware latches the hardware reset flags at boot and
    **clears the latch when a host asks**, so this is answered once per boot and
    is the cause of the *last* reset.  "none" therefore has two meanings: the
    boot did not set a flag, or something already asked (a `wdg_test.py` run reads
    it as part of its checks, which is why reading it here afterwards says none);
  * **endpoint counters** - EP0/1/2 receive bytes, EP3 IN packets and bytes, and
    the FIFO level and drop counts, which is what says whether a data path was
    really taken or silently fell back.
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0] + "/../scripts")
from v003_usb import find_device  # noqa: E402

GENERIC = 0x00 << 8
GET_FIRMWARE_VER = 0x30 | GENERIC
GET_DEVICE_SN = 0x31 | GENERIC
GET_EP_STATS = 0x32 | GENERIC
GET_FIFO_LEVEL = 0x33 | GENERIC
GET_FIFO_DROPS = 0x34 | GENERIC
GET_REQ_DROPS = 0x35 | GENERIC
GET_STACK_FREE = 0x3B | GENERIC
GET_CAPABILITIES = 0x3D | GENERIC

ADC = 0x06 << 8
ADC_GET_SEQ = 0x83 | ADC
ADC_GET_STATUS = 0x84 | ADC

WDG = 0x04 << 8
WDG_GET_STATE = 0x62 | WDG
WDG_GET_RESET_CAUSE = 0x63 | WDG

PWR = 0x08 << 8
PWR_GET_STATE = 0xA1 | PWR

CAP_STRUCT = "<IBBBBII"  # caps, ngpio, nadc, npwm, nuart, reserved_lo, reserved_hi
PWR_STRUCT = "<5I8B"  # 28 bytes, no padding (vendor/pwr.h)
# Named indices, so adding a field to a structure cannot quietly shift the ones
# below it - doing that by hand cost an afternoon while the ADC was written.
(P_COUNT, P_TICKS, P_REQ, P_NOM, P_CAPS, P_REF, P_MODE, P_WAKE, P_DETACH,
 P_ARMED, P_REASON, P_DIV) = range(12)

CAP_NAMES = ["gpio", "spi", "i2c", "adc", "pwm", "uart", "frame", "wdg", "pwr"]
CAP_MASK_ALL = 0x1FF
PWR_CAPS = ["sleep", "standby", "button", "detach"]
PWR_REFUSED = {0: "accepted", 1: "bad mode", 2: "out of range",
               3: "watchdog armed", 4: "busy"}
PWR_WOKE = {0: "AWU", 1: "button", 2: "UART", 3: "other"}
RST_BITS = [(1 << 0, "pin"), (1 << 1, "power-on"), (1 << 2, "software"),
            (1 << 3, "watchdog"), (1 << 4, "window watchdog"),
            (1 << 5, "low power")]
PORTS = {0: "PA", 2: "PC", 3: "PD"}

STACK_FLOOR = 350  # bytes, notes/firmware.md
ADC_CONVERSION_US = 42  # 241 sample clocks + 11 at 6 MHz, vendor/adc.h


def pin_name(pin):
    port = PORTS.get(pin >> 4)
    return f"{port}{pin & 0xF}" if port else f"pin{pin}"


def ranges(mask):
    """A 64 bit pin mask as a compact list of names and ranges."""
    spans = []
    start = None
    for pin in range(64):
        if mask & (1 << pin):
            if start is None:
                start = pin
        elif start is not None:
            spans.append((start, pin - 1))
            start = None
    if start is not None:
        spans.append((start, 63))

    text = []
    for lo, hi in spans:
        if lo == hi:
            text.append(pin_name(lo))
        elif (lo, hi) == (16, 31):
            text.append("16..31 (no port on this part)")
        else:
            text.append(f"{pin_name(lo)}..{pin_name(hi)}")
    return ", ".join(text)


class Dev:
    """The same control transfer helper every script in scripts/ uses."""

    def __init__(self):
        self.dev = None
        for _ in range(20):
            dev = find_device()
            if dev is not None:
                try:
                    dev.set_configuration()
                    dev.ctrl_transfer(0x00, 0x09, 1, 0, 0, timeout=1000)
                    self.dev = dev
                    break
                except usb.core.USBError:
                    time.sleep(0.2)
        if self.dev is None:
            raise SystemExit("FAIL the device is not there "
                             "(are the kernel modules unloaded?)")

    def u32(self, idx, val=0):
        return int.from_bytes(self.in_data(idx, 4, val), "little")

    def in_data(self, idx, length, val=0):
        return bytes(self.dev.ctrl_transfer(0xC0, 0x00, val, idx, length,
                                            timeout=2000))

    def out(self, idx, val=0):
        """Vendor control OUT: the command travels in wIndex, its argument in
        wValue, the convention every script in scripts/ uses."""
        return self.dev.ctrl_transfer(0x40, 0x00, val, idx, 0, timeout=2000)

    def out_data(self, idx, data, val=0):
        return self.dev.ctrl_transfer(0x40, 0x00, val, idx, bytes(data),
                                      timeout=3000)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pins", action="store_true",
                    help="name the reserved pin mask instead of one hex number")
    ap.add_argument("--reset-cause", action="store_true",
                    help="also read the cause of the last reset; the firmware "
                         "answers it once per boot, so this clears the latch")
    args = ap.parse_args()

    d = Dev()
    print(f"firmware          {d.u32(GET_FIRMWARE_VER):#x}, "
          f"serial word {d.u32(GET_DEVICE_SN):#010x}")

    caps, ngpio, nadc, npwm, nuart, rlo, rhi = struct.unpack(
        CAP_STRUCT, d.in_data(GET_CAPABILITIES, 16))
    modules = [n for i, n in enumerate(CAP_NAMES) if caps & (1 << i)]
    print(f"capabilities      {caps:#x}: {' '.join(modules)}"
          + ("" if caps == CAP_MASK_ALL else f"   (full set is {CAP_MASK_ALL:#x})"))
    print(f"channel counts    {ngpio} gpio lines, {nadc} adc, {npwm} pwm, "
          f"{nuart} uart")

    mask = rlo | (rhi << 32)
    print(f"reserved pins     {mask:#018x}")
    if args.pins:
        print(f"                  {ranges(mask)}")

    d.u32(GET_STACK_FREE)  # arms the refresh, answers with the stale value
    stack = d.u32(GET_STACK_FREE)
    print(f"stack margin      {stack} bytes free of 2048, deepest use the canary "
          f"has seen"
          + ("" if stack >= STACK_FLOOR else f"   (BELOW the {STACK_FLOOR} B floor!)"))

    status = d.u32(ADC_GET_STATUS)
    us = (status >> 16) & 0xFF
    print(f"adc               {status & 0xFF} conversions, "
          f"{(status >> 8) & 0xFF} requests, {status >> 24} errors, "
          f"last conversion {us} us, seq {d.u32(ADC_GET_SEQ)}"
          + ("" if us in (0, ADC_CONVERSION_US) else
             f"   (expected {ADC_CONVERSION_US})"))

    st = struct.unpack(PWR_STRUCT, d.in_data(PWR_GET_STATE, 28))
    print(f"pwr               {st[P_COUNT]} sleeps, last "
          f"{'standby' if st[P_MODE] else 'sleep'} for {st[P_NOM]} ms nominal "
          f"of {st[P_REQ]} asked, woke by {PWR_WOKE.get(st[P_WAKE], '?')}, "
          f"detach {st[P_DETACH]}, refused: {PWR_REFUSED.get(st[P_REF], '?')}")
    print(f"                  caps: "
          + " ".join(c for i, c in enumerate(PWR_CAPS) if st[P_CAPS] & (1 << i))
          + (f", {st[P_ARMED]} armed" if st[P_ARMED] else ""))

    wdg = d.u32(WDG_GET_STATE)
    print(f"watchdog          {'running' if wdg & 1 else 'off'}"
          + (f", actual timeout {wdg >> 8} ms (the LSI is +-20 %)"
             if wdg & 1 else ""))

    print(f"endpoints         ep0 {d.u32(GET_EP_STATS, 0)} B rx, "
          f"ep1 {d.u32(GET_EP_STATS, 1)} B, ep2 {d.u32(GET_EP_STATS, 2)} B, "
          f"ep3 {d.u32(GET_EP_STATS, 3)} packets / "
          f"{d.u32(GET_EP_STATS, 4)} B out")
    print(f"fifo              {d.u32(GET_FIFO_LEVEL)} B queued, "
          f"{d.u32(GET_FIFO_DROPS)} dropped; "
          f"{d.u32(GET_REQ_DROPS)} vendor requests dropped")

    if args.reset_cause:
        cause = d.u32(WDG_GET_RESET_CAUSE)
        names = [n for bit, n in RST_BITS if cause & bit]
        print(f"reset cause       {cause:#x}: {', '.join(names) if names else 'none'}"
              f"   (the latch is cleared by this read; 'none' can also mean "
              f"something asked first)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
