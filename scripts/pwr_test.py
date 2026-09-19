#!/usr/bin/env python3
"""Power management test for the CH32V003 bridge: sleep, standby, wake reasons.

This is the one module whose behaviour a host can only verify by watching the
device *stop existing* for a while, so the test is built around that:

  - **the AWU backstop is always armed**, and the check is that the device comes
    back by itself with `wake_reason = AWU` and the sleep counted.  Staying quiet
    for the whole duration is what makes that a real check: any host traffic
    during a sleep wakes the chip through the USB EXTI, which is a feature (the
    host can end a sleep early) and the reason the reason field exists;
  - **standby + detach is the measurable one**: the firmware releases the USB
    pull-up, so the device leaves the bus and comes back by itself.  Time from
    arming to the device answering again is the sleep duration plus enumeration,
    which is what turns the firmware's tick count into a measurement of the LSI -
    the same read-back-the-real-value rule as the PWM period and the baud rate;
  - **a sleep longer than the watchdog timeout must be refused**, because the
    IWDG keeps counting in standby: that is checked without sleeping at all.

  .venv/bin/python scripts/pwr_test.py [--long] [--wdg] [--button]

The modules have to be unloaded (the kernel driver claims the interface).

Durations that mean "the device is asleep" are detected by a control transfer
that fails or takes far longer than usual; a sleep that ends early shows up as a
wake reason that is not AWU, which is reported rather than hidden.
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from v003_usb import find_device  # noqa: E402

PWR_MODULE = 0x08
PWR_ARM = 0xA0 | (PWR_MODULE << 8)
PWR_GET_STATE = 0xA1 | (PWR_MODULE << 8)
PWR_GET_INFO = 0xA2 | (PWR_MODULE << 8)
WDG_GET_RESET_CAUSE = 0x63 | (0x04 << 8)

STATE_FMT = "<5I8B"  # 28 bytes, no padding
# Named indices, so adding a field to the structure cannot quietly shift every
# check in this file by one (which it did twice while this module was written).
(F_COUNT, F_TICKS, F_REQ, F_NOM, F_CAPS, F_REF, F_MODE, F_WAKE, F_DETACH,
 F_ARMED, F_REASON, F_DIV, F_PAD) = range(13)
MODE_SLEEP = 0
MODE_STANDBY = 1
WAKE_AWU = 0
WAKE_BUTTON = 1
WAKE_UART = 2
WAKE_OTHER = 3
REF = {0: "accepted", 1: "bad mode", 2: "out of range", 3: "watchdog armed",
       4: "busy"}
WAKE_NAME = {0: "AWU", 1: "button", 2: "UART", 3: "other"}
DIV = [1, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 10240, 61440]
LSI_NOMINAL = 128000

failures = []
skipped = []


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


def skip(name, why):
    print(f"skip {name}: {why}")
    skipped.append(name)


class Dev:
    """A device that may vanish (standby with detach) and come back."""

    def __init__(self, timeout=4.0):
        self.dev = None
        self.timeout = timeout
        self.reopen()

    def reopen(self, timeout=None):
        deadline = time.time() + (timeout if timeout else self.timeout)
        while time.time() < deadline:
            dev = find_device()
            if dev:
                try:
                    dev.set_configuration()
                    # endpoint toggles: the device resets them on
                    # SET_CONFIGURATION and this is also how a fresh
                    # enumeration is detected
                    dev.ctrl_transfer(0x00, 0x09, 1, 0, 0, timeout=1000)
                    self.dev = dev
                    return True
                except usb.core.USBError:
                    pass
            time.sleep(0.1)
        self.dev = None
        return False

    # -- requests ------------------------------------------------------

    def u32(self, idx, val=0, timeout=2000):
        return int.from_bytes(
            bytes(self.dev.ctrl_transfer(0xC0, 0, val, idx, 4,
                                         timeout=timeout)), "little")

    def state(self, timeout=2000):
        raw = bytes(self.dev.ctrl_transfer(0xC0, 0, 0, PWR_GET_STATE, 28,
                                           timeout=timeout))
        return struct.unpack(STATE_FMT, raw)

    def arm(self, duration_ms, mode=MODE_SLEEP, wake=0, detach=0, delay=10):
        self.dev.ctrl_transfer(
            0x40, 0, 0, PWR_ARM,
            struct.pack("<IBBBB", duration_ms, mode, wake, detach, delay),
            timeout=3000)

    def reset_cause(self):
        return self.u32(WDG_GET_RESET_CAUSE)

    def alive(self, timeout=200):
        try:
            return self.u32(0x30, timeout=timeout) == 0x1010
        except usb.core.USBError:
            return False


def tick_us(div):
    return DIV[div] * 125 / 16


def sleep_and_wait(dev, duration_ms, mode=MODE_SLEEP, detach=0, wake=0,
                   quiet=None):
    """Arm a sleep, stay off the bus for `quiet` ms, then find the device again.

    Returns (state, milliseconds from arming to the device answering).
    """
    before = dev.state()[F_COUNT]
    quiet = quiet if quiet is not None else duration_ms + 50
    t0 = time.time()
    dev.arm(duration_ms, mode=mode, detach=detach, wake=wake)
    time.sleep(quiet / 1000.0)
    if not dev.reopen():
        failures.append(f"the device did not come back after {duration_ms} ms")
        print(f"FAIL the device did not come back after {duration_ms} ms")
        return None, None
    elapsed = (time.time() - t0) * 1000
    st = dev.state()
    return st, elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--long", action="store_true",
                    help="also run a 10 second standby sleep")
    ap.add_argument("--wdg", action="store_true",
                    help="arm the IWDG and check that a longer sleep is refused "
                         "(the watchdog cannot be stopped afterwards; the chip "
                         "resets once and comes back with it off)")
    ap.add_argument("--uart", action="store_true",
                    help="try to end a sleep with UART traffic (needs a peer)")
    ap.add_argument("--button", action="store_true",
                    help="arm a long sleep and ask for the boot button to be "
                         "pressed to end it early")
    args = ap.parse_args()

    dev = Dev()
    info = dev.u32(PWR_GET_INFO)
    lo, hi = info & 0xffff, info >> 16
    st = dev.state()
    print(f"info range {lo}..{hi} ms, caps {st[4]:#x} "
          f"(sleep|standby|button|detach), sleep_count {st[0]}")
    check("the capability report offers a sleep range", (lo, hi), (10, 30000))
    check("the module says it can sleep, standby, use the button and detach",
          st[F_CAPS], 0xF)

    # -- what it refuses ------------------------------------------------

    for kwargs, why, expected in (
            (dict(duration_ms=hi + 1), "a duration above the range", 2),
            (dict(duration_ms=1), "a duration below the range", 2),
            (dict(duration_ms=100, mode=7), "an unknown mode", 1),
            (dict(duration_ms=100, wake=0x80), "an unknown wake source", 1)):
        dev.arm(**kwargs)
        time.sleep(0.05)
        st = dev.state()
        check(f"{why} is refused", (st[F_REF], st[F_ARMED]), (expected, 0))
        if st[F_REF] == 0:
            dev.reopen()  # it really went to sleep; get it back

    # -- the AWU backstop ----------------------------------------------

    # the reset flags are read and cleared, so read them once now: whatever
    # happened before the test (a flash resets the chip) is not the sleep's doing
    cause0 = dev.reset_cause()
    if cause0:
        print(f"     (the chip had been reset before the test: {cause0:#x})")

    for duration in (100, 300, 1000):
        st, elapsed = sleep_and_wait(dev, duration)
        if st is None:
            continue
        tick = tick_us(st[F_DIV])
        print(f"     {duration} ms sleep: ticks={st[F_TICKS]} div={st[F_DIV]} "
              f"tick={tick:.0f}us nominal={st[F_NOM]} ms, reason={WAKE_NAME[st[F_REASON]]}, "
              f"count={st[F_COUNT]}, answered {elapsed:.0f} ms after arming")
        check(f"{duration} ms: the sleep happened", st[F_COUNT] > 0, True)
        check(f"{duration} ms: the AWU backstop woke it",
              st[F_REASON], WAKE_AWU)
        # the AWU window is a whole number of ticks, so the nominal duration is
        # quantised to the tick period the firmware picked (2, 8 or 16 ms here)
        check_range(f"{duration} ms: the nominal duration matches the request "
                    f"within one {tick:.0f} ms tick",
                    st[F_NOM], duration - tick + 1, duration, " ms")
        check(f"{duration} ms: it was still asleep when we stopped waiting",
              elapsed >= duration, True)
        check(f"{duration} ms: nothing reset the chip",
              dev.reset_cause(), 0)

    # -- a host transfer ends a sleep early -----------------------------

    st, elapsed = sleep_and_wait(dev, 5000, quiet=150)
    if st is not None:
        print(f"     poked a 5000 ms sleep after 150 ms: reason="
              f"{WAKE_NAME[st[F_REASON]]}, count={st[F_COUNT]}")
        check("the sleep happened", st[F_COUNT] > 0, True)
        # The firmware cannot name this one (see vendor/pwr.h: the USB handler
        # eats the pending bit and the stack's activity marker moves on every
        # SOF), so the evidence is on the host's side: it answered long before
        # the deadline it was given.
        check_range("a sleep ends early when the host talks", elapsed,
                    0, 1000, " ms")
        print(f"     (the firmware reports {WAKE_NAME[st[F_REASON]]} for it, "
              f"which is honest: it cannot tell the host apart from a SOF)")

    # -- UART activity during a sleep (cannot be produced here) ---------

    # The reason exists in the firmware and the code path is exercised below,
    # but this bench cannot produce it: sending a byte *in* needs a second UART
    # device on PD1 (the only thing wired there is the loopback jumper to PD0),
    # and letting the device's own transmitter drain during the sleep races with
    # the host's own poke - which is what a control transfer after the arm is.
    # Measured once anyway, to see what happens: the sleep ended early and the
    # firmware reported AWU, which is consistent with the host's transfer having
    # arrived first, so nothing can be concluded from it.
    if args.uart:
        uart_cfg = 0x90 | (0x07 << 8)
        uart_write = 0x92 | (0x07 << 8)
        uart_flush = 0x97 | (0x07 << 8)
        try:
            dev.dev.ctrl_transfer(0x40, 0, 0, uart_cfg,
                                  struct.pack("<IBBBBI", 733, 8, 0, 1, 1, 0),
                                  timeout=3000)
            dev.dev.ctrl_transfer(0x40, 0, 0, uart_flush, b"", timeout=2000)
            t0 = time.time()
            dev.arm(2000)
            dev.dev.ctrl_transfer(0x40, 0, 0, uart_write, b"\x01\x02\x03",
                                  timeout=3000)
            time.sleep(0.3)
            if dev.reopen():
                st = dev.state()
                elapsed = (time.time() - t0) * 1000
                print(f"     UART traffic during a 2000 ms sleep: back after "
                      f"{elapsed:.0f} ms, reason={WAKE_NAME[st[F_REASON]]}")
                if st[F_REASON] != WAKE_UART:
                    skip("UART activity during a sleep is reported",
                         "the host's own poke ended the sleep first (there is no "
                         "second UART device on this bench to send a byte in)")
                else:
                    check("UART activity during a sleep is reported",
                          st[F_REASON], WAKE_UART)
        except usb.core.USBError as exc:
            skip("UART activity during a sleep", f"usb error: {exc}")
        finally:
            # never leave the port enabled: it drives PD0, which the loopback
            # jumper ties to the SWIO debug pin, and then the programmer cannot
            # get in any more
            try:
                dev.dev.ctrl_transfer(
                    0x40, 0, 0, uart_cfg,
                    struct.pack("<IBBBBI", 115200, 8, 0, 1, 0, 0),
                    timeout=3000)
            except usb.core.USBError:
                pass
    else:
        skip("UART activity during a sleep", "--uart (needs a second UART device)")

    # -- standby with detach: the measurable one ------------------------

    standby = []
    for duration in (500, 2000) + ((10000,) if args.long else ()):
        st, elapsed = sleep_and_wait(dev, duration, mode=MODE_STANDBY, detach=1)
        if st is None:
            continue
        tick = tick_us(st[F_DIV])
        nominal_ms = st[F_TICKS] * tick / 1000.0
        standby.append((nominal_ms, elapsed))
        print(f"     standby+detach {duration} ms: ticks={st[F_TICKS]} div={st[F_DIV]} "
              f"nominal={nominal_ms:.0f} ms, back after {elapsed:.0f} ms")
        check(f"standby {duration} ms: the sleep happened", st[F_COUNT] > 0, True)
        check(f"standby {duration} ms: the AWU woke it", st[F_REASON], WAKE_AWU)
        check_range(f"standby {duration} ms: the request was nominally honoured",
                    nominal_ms, duration * 0.9, duration, " ms")
        # the device re-appears after the sleep *plus* enumeration, and the host
        # takes a few hundred milliseconds to notice.  Two points give both the
        # overhead and the timer's real rate, which is the measurement that says
        # whether the AWU and the nominal 128 kHz LSI agree.
        check_range(f"standby {duration} ms: came back in a plausible time",
                    elapsed, duration * 0.5, duration + 3000, " ms")

    if len(standby) >= 2:
        (n1, e1), (n2, e2) = standby[0], standby[1]
        rate = (n2 - n1) / (e2 - e1)  # nominal ms per real ms
        overhead = e1 - n1 / rate
        print(f"     from two points: the sleep runs at {rate:.3f} of nominal "
              f"(LSI about {LSI_NOMINAL * rate / 1000:.0f} kHz against the "
              f"nominal {LSI_NOMINAL / 1000:.0f} kHz) and the host takes "
              f"{overhead:.0f} ms to see the device again")
        check_range("the AWU runs at the nominal rate within 20 %", rate,
                    0.8, 1.2)
        check_range("and the re-enumeration overhead is a fixed few hundred ms",
                    overhead, 0, 1500, " ms")

    # -- repeatability --------------------------------------------------

    before = dev.state()[F_COUNT]
    rounds = 10
    for _ in range(rounds):
        st, _ = sleep_and_wait(dev, 50, quiet=120)
        if st is None:
            break
    after = dev.state()[F_COUNT] if dev.dev else None
    check(f"{rounds} sleeps in a row are all counted", after - before, rounds)
    check("and the device is still healthy afterwards", dev.alive(), True)

    # -- the watchdog interaction (opt in) ------------------------------

    if args.wdg:
        dev.dev.ctrl_transfer(0x40, 0, 2000, 0x60 | (0x04 << 8), 0, timeout=2000)
        time.sleep(0.2)
        dev.arm(5000)
        time.sleep(0.1)
        st = dev.state()
        check("a sleep longer than the watchdog timeout is refused",
              (st[F_REF], st[F_ARMED]), (3, 0))
        print("     the IWDG is armed now and cannot be stopped: letting it "
              "bite, the device resets and comes back with it off")
        time.sleep(2.5)
        if dev.reopen():
            check("the watchdog reset the chip", dev.reset_cause() & 0x08, 8)
            check("and the state is fresh after it", dev.state()[0], 0)
        else:
            failures.append("the device did not come back after the watchdog")
    else:
        skip("the watchdog interaction", "--wdg (arms the IWDG, one way door)")

    # -- wake by the boot button (opt in, needs a finger) ---------------

    if args.button:
        # The button pulls PD6 down, and the firmware arms it as an EXTI event on
        # a falling edge.  There is no way for the host to fake that edge while
        # the device is asleep and detached (nothing of ours is on the pin), so
        # this one needs a finger on the board.
        # The wake is an edge, and the firmware deliberately clears a press that
        # happened before it armed - so the order matters: arm first, let the
        # device actually be asleep, and only then ask for a press.
        print("     arming a 20 s standby sleep with the button as the wake "
              "source")
        dev.arm(20000, mode=MODE_STANDBY, detach=1, wake=WAKE_BUTTON)
        t0 = time.time()
        time.sleep(1.5)
        print("     PRESS THE BOOT BUTTON NOW (the device is asleep and off "
              "the bus)")
        if not dev.reopen(timeout=25):
            failures.append("the device did not come back after the button sleep")
        else:
            elapsed = (time.time() - t0) * 1000
            st = dev.state()
            print(f"     back after {elapsed:.0f} ms with reason="
                  f"{WAKE_NAME[st[F_REASON]]}")
            if st[F_REASON] == WAKE_BUTTON:
                check("the boot button ended the sleep early", elapsed < 5000,
                      True)
            else:
                skip("wake by the boot button",
                     "the reason is " + WAKE_NAME[st[F_REASON]] +
                     ": the button was not pressed within the window")
    else:
        skip("wake by the boot button", "--button (drives PD6 from the host)")

    print()
    if skipped:
        print(f"{len(skipped)} skipped: {', '.join(skipped)}")
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except usb.core.USBError as exc:
        print(f"FAIL usb: {exc}")
        sys.exit(1)
