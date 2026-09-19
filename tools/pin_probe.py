#!/usr/bin/env python3
"""What is actually on each pad, without trusting the wiring diagram.

Brought the modules up with the pin table as a map, then spent an afternoon
debugging a chip select that had quietly become an input.  Two questions this
answers, both of which the datasheets and the schematic cannot:

  * **is this pad bonded, and does it drive what we tell it to?**  `probe` drives
    each pin to both levels and reads it back.  A pad that answers `0,0` or `1,1`
    is held by something external or is not on the package at all (PA0 and
    PA3..PA15 on this part read 0 no matter what is written).
  * **which pin is this button on?**  `--watch` samples the pads as inputs and
    prints every change with a timestamp, so a button, a jumper or a floating pad
    identifies itself while you press it - no guessing from a photo of the board.

  tools/pin_probe.py                      # probe the pins the firmware left free
  tools/pin_probe.py --all                # every pad that is safe to touch
  tools/pin_probe.py --pins 50 54 36      # just these, by flat number
  tools/pin_probe.py --watch              # watch the free pads for 15 s
  tools/pin_probe.py --watch --seconds 60 --pins 54   # watching for a button
  tools/pin_probe.py --watch --pins 1 --keep-mode     # watch PWM on PA1 run
  tools/pin_probe.py --all --allow-risky  # include SWIO and the USB pins

Two things to know before running it:

  * **A probe changes the pin's mode, and the firmware's GPIO module does not
    remember what the pin *was* doing.**  Probing a pin another module owns
    (PC4 while the SPI module is driving it as a chip select, PD2 while the timer
    drives PWM) leaves that module's output off until the module is asked to
    reconfigure it.  Reserved pins are therefore skipped by default - the device
    reports its own reserved mask (0x3d) and this tool reads it, so "free" means
    free in *this* firmware build.
  * **The kernel drivers error out more gracefully here than the raw device
    does.**  Over pyusb the GPIO module has no ownership bookkeeping: REQUEST on
    a pin someone else is driving silently changes its mode.  The kernel GPIO
    driver (`tests/gpio_sysfs.py`, the character device) refuses a line another
    driver claimed, which is the layer to use if you want that protection.

There is no pull-up/down selector in the module: `GPIO_DIRECTION_INPUT` sets
input mode and leaves the output data register alone, and in input mode that bit
*is* the pull selector.  So the tool gets a definite pull-up by driving the pin
high once and then switching to input, and a definite pull-down by driving it low
first - which is also the trick to use by hand when a floating pad needs a level.
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from status import Dev, GET_CAPABILITIES, CAP_STRUCT, pin_name  # noqa: E402

GPIO = 0x01 << 8
GPIO_GET = 0x07 | GPIO
GPIO_REQUEST = 0x08 | GPIO
GPIO_FREE = 0x09 | GPIO
GPIO_GET_DIRECTION = 0x0A | GPIO
GPIO_DIRECTION_INPUT = 0x0B | GPIO
GPIO_DIRECTION_OUTPUT = 0x0C | GPIO

# pads that exist on this package: PA0..PA2, PC0..PC7, PD0..PD6
EXISTING = [0, 1, 2] + list(range(32, 40)) + list(range(48, 55))

# Never touched unless --allow-risky is given, with the reason.
RISKY = {
    49: "PD1 is SWIO: the WCH-LinkE holds it, and driving it can confuse the "
        "programmer (and it is the UART's RX pad when the jumper is on)",
    51: "PD3 is USB D+: touching it drops the device off the bus",
    52: "PD4 is USB D-: touching it drops the device off the bus",
    53: "PD5 is the USB pull-up: driving it low *disconnects this device from "
        "the host*, so the probe would lose the device it is talking to",
}

# Pins the boot button is on are safe to read and unsafe to drive: the button
# shorts the pad to ground when pressed, and two outputs fighting is a short.
READ_ONLY = {54: "PD6 carries the boot button: read only"}


def gpio_val(pin, val=0):
    """The module puts the pin in the high byte and the value in the low byte."""
    return ((pin & 0xFF) << 8) | (val & 1)


class Pins:
    def __init__(self):
        self.d = Dev()
        caps, _ngpio, _nadc, _npwm, _nuart, rlo, rhi = struct.unpack(
            CAP_STRUCT, self.d.in_data(GET_CAPABILITIES, 16))
        self.reserved = rlo | (rhi << 32)
        self.caps = caps

    def get(self, pin):
        return self.d.u32(GPIO_GET, gpio_val(pin))

    def direction(self, pin):
        """1 = the module says the pin is an input, 0 = an output."""
        return self.d.u32(GPIO_GET_DIRECTION, gpio_val(pin))

    def output(self, pin, val):
        self.d.out(GPIO_DIRECTION_OUTPUT, gpio_val(pin, val))

    def input(self, pin, pull):
        """Input, with the pull selected by first driving the level (see above)."""
        self.output(pin, pull)
        self.d.out(GPIO_DIRECTION_INPUT, gpio_val(pin))

    def free(self, pin):
        self.d.out(GPIO_FREE, gpio_val(pin))


def probe(p, pin):
    """Measure a pad and put it back the way it was."""
    was_output = p.direction(pin) == 0
    was_level = p.get(pin)

    p.d.out(GPIO_REQUEST, gpio_val(pin))
    p.input(pin, 1)
    pull_up = p.get(pin)
    p.input(pin, 0)
    pull_down = p.get(pin)
    p.output(pin, 0)
    drive_lo = p.get(pin)
    p.output(pin, 1)
    drive_hi = p.get(pin)

    if was_output:
        p.output(pin, was_level)
    else:
        p.input(pin, was_level)
    p.free(pin)

    if (drive_lo, drive_hi) == (0, 1):
        verdict = "the pad drives both levels"
    elif (drive_lo, drive_hi) == (0, 0):
        verdict = "stuck at 0 even driven high: not bonded, or held down hard"
    elif (drive_lo, drive_hi) == (1, 1):
        verdict = "stuck at 1 even driven low: held high by something external"
    else:
        verdict = "changed between the two samples: something else is driving it"

    # What the pad does *as an input* with a known weak pull is a separate
    # question: the push-pull output above wins against anything external, so a
    # pad that behaves there can still have a load on it.
    if pull_up == pull_down:
        verdict += (f"; an external load holds it {'high' if pull_up else 'low'}"
                    " (the internal pull does not move it)")
    else:
        verdict += ("; the internal pull decides its level, so only a high "
                    "impedance load is on it (an LED, another input)")

    return {"pull_up": pull_up, "pull_down": pull_down, "lo": drive_lo,
            "hi": drive_hi, "was": "out" if was_output else "in",
            "was_level": was_level, "verdict": verdict}


def cmd_probe(p, pins, risky_ok):
    print(f"{'pin':<6}{'name':<6}{'owner':<10}{'was':<5}"
          f"{'pull-up':<9}{'pull-dn':<9}{'drive 0':<9}{'drive 1':<9}verdict")
    for pin in pins:
        note = RISKY.get(pin) if not risky_ok else None
        if note:
            print(f"{pin:<6}{pin_name(pin):<6}{'skipped':<10}{'':<5}{'':<9}"
                  f"{'':<9}{'':<9}{'':<9}{note}")
            continue
        if pin in READ_ONLY:
            print(f"{pin:<6}{pin_name(pin):<6}{'read-only':<10}"
                  f"{'':<5}{'':<9}{'':<9}{'':<9}{'':<9}{READ_ONLY[pin]}")
            continue
        r = probe(p, pin)
        owner = "reserved" if p.reserved & (1 << pin) else "free"
        print(f"{pin:<6}{pin_name(pin):<6}{owner:<10}{r['was']:<5}"
              f"{r['pull_up']:<9}{r['pull_down']:<9}{r['lo']:<9}{r['hi']:<9}"
              f"{r['verdict']}")
    return 0


def cmd_watch(p, pins, seconds, pull, keep_mode, limit):
    """Sample the pins as inputs and report every change.

    A sweep is one control transfer per pin (about a millisecond each at low
    speed), so a press shorter than a sweep can slip between two samples: hold
    the button down for a second and the change is unmissable.  This is a level
    sampler, not a logic analyser - it cannot see a pulse per PWM period, only
    that a pin moved, and it stops printing after --limit edges (it keeps
    counting, so a fast signal shows up as a rate in the summary).
    """
    if not keep_mode:
        for pin in pins:
            p.d.out(GPIO_REQUEST, gpio_val(pin))
            p.input(pin, pull)

    state = {pin: p.get(pin) for pin in pins}
    print(f"watching {len(pins)} pins for {seconds} s"
          + (" without touching their modes" if keep_mode
             else f", pull-{'up' if pull else 'down'}") 
          + f", {len(pins)} ms per sweep")
    print(f"  starting levels: "
          + ", ".join(f"{pin_name(pi)}={state[pi]}" for pi in pins))
    print("  press the button / move the jumper now")

    start = time.monotonic()
    changes = {}
    printed = 0
    while time.monotonic() - start < seconds:
        for pin in pins:
            try:
                now = p.get(pin)
            except usb.core.USBError as err:
                print(f"  {time.monotonic() - start:6.2f}s  lost the device: "
                      f"{err}")
                return 1
            if now != state[pin]:
                if printed < limit:
                    print(f"  {time.monotonic() - start:6.2f}s  {pin_name(pin)} "
                          f"({pin}) {state[pin]} -> {now}")
                    printed += 1
                elif printed == limit:
                    print(f"  ... more edges; printing stopped at --limit "
                          f"{limit} (still counting)")
                    printed += 1
                changes[pin] = changes.get(pin, 0) + 1
                state[pin] = now
        time.sleep(0.001)

    moved = [pi for pi in pins if changes.get(pi)]
    if moved:
        print("pins that changed: "
              + ", ".join(f"{pin_name(pi)} ({pi}) x{changes[pi]}" for pi in moved))
    else:
        print("nothing changed: no button on these pins, or it was not pressed")

    if not keep_mode:
        for pin in pins:
            p.free(pin)
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pins", type=int, nargs="+", default=None,
                    help="flat pin numbers to work on (port*16 + pin)")
    ap.add_argument("--all", action="store_true",
                    help="include the pins the firmware reserved for its modules")
    ap.add_argument("--watch", action="store_true",
                    help="sample the pins instead of driving them")
    ap.add_argument("--seconds", type=float, default=15.0,
                    help="how long --watch runs (default 15)")
    ap.add_argument("--pull", choices=["up", "down"], default="up",
                    help="input pull to use while watching (default up)")
    ap.add_argument("--keep-mode", action="store_true",
                    help="with --watch: sample a pad another module is driving "
                         "(a PWM output, a chip select) without taking it away")
    ap.add_argument("--limit", type=int, default=40,
                    help="with --watch: stop printing after this many edges, "
                         "keep counting (default 40)")
    ap.add_argument("--allow-risky", action="store_true",
                    help="also touch SWIO and the USB pins - read the reasons first")
    args = ap.parse_args()

    p = Pins()

    if args.pins:
        pins = args.pins
    else:
        pins = [pi for pi in EXISTING
                if args.all or not (p.reserved & (1 << pi))]

    if args.watch:
        pins = [pi for pi in pins if pi not in RISKY or args.allow_risky]
        return cmd_watch(p, pins, args.seconds, 1 if args.pull == "up" else 0,
                         args.keep_mode, args.limit)

    return cmd_probe(p, pins, args.allow_risky)


if __name__ == "__main__":
    sys.exit(main())
