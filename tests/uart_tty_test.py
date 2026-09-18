#!/usr/bin/env python3
"""UART test for the kernel side of the device: /dev/ttyV0 of v003-uart.ko.

Where scripts/uart_test.py talks the vendor protocol over pyusb, this one goes
through the Linux TTY layer, so it covers the driver (termios -> V003_UART_CONFIG,
write() flow control against the firmware's transmit ring, the polled receive
path) and not just the firmware.

  .venv/bin/python tests/uart_tty_test.py

It needs a **jumper between PD0 and PD1** (the same one scripts/uart_test.py
needs): PD1 is the chip's SWIO debug pin, so the only way to feed the receiver is
to tie it to the transmitter.

Access to /dev/ttyV0 is what usually stops this test: the tty core creates it as
`root:uucp 0660`, so either run as root or install a udev rule like the one the
I2C test needs:

  echo 'SUBSYSTEM=="tty", KERNEL=="ttyV*", GROUP="plugdev", MODE="0660"' \\
    | sudo tee /etc/udev/rules.d/61-tty-v003-plugdev.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=tty

Loading order, because the child driver needs the core's symbols:

  sudo insmod usb-mfd.ko
  sudo insmod v003-uart.ko

Only the standard library is used.
"""

import argparse
import os
import select
import sys
import termios
import time
import tty

DEVICE = "/dev/ttyV0"
STATS = "/sys/class/tty/ttyV0/stats"
STATS_RESET = "/sys/class/tty/ttyV0/stats_reset"
JUMPER_HINT = ("nothing came back - is the PD0<->PD1 jumper in place, and is the "
               "WCH-Link holding PD1?  (see notes/uart.md)")

UDEV_RULE = """SUBSYSTEM=="tty", KERNEL=="ttyV*", GROUP="plugdev", MODE="0660" """

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


def reset_stats():
    """Zero the firmware's counters, if we are allowed to.

    They are eight bit and cumulative, so a saturated one (the receiver's
    interrupt count reaches 255 after a few hundred bytes) reports a delta of 0
    and can only be measured after a reset.  The sysfs attribute is write-only
    and therefore root-owned, so a test run as a normal user gets EACCES here and
    has to skip the checks that depend on it instead of failing.
    """
    try:
        with open(STATS_RESET, "w") as fh:
            fh.write("1")
        return True
    except PermissionError:
        print("     (cannot reset the firmware counters: "
              f"{STATS_RESET} is root-only; the checks that need it are skipped)")
        return False


def skip(name, why):
    print(f"skip {name}: {why}")
    skipped.append(name)


def read_stats():
    """Parse the driver's stats attribute into a dict of ints."""
    out = {}
    with open(STATS) as fh:
        for line in fh:
            for field in line.split():
                if "=" in field:
                    key, _, value = field.partition("=")
                    try:
                        out[key] = int(value)
                    except ValueError:
                        out[key] = value
    return out


def configure(fd, baud):
    """Raw 8N1, no flow control, CLOCAL so open() does not wait for a carrier."""
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[3] = 0  # lflag
    attrs[2] = termios.CREAD | termios.CLOCAL | termios.CS8
    speed = getattr(termios, f"B{baud}")
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def read_for(fd, want, timeout):
    out = b""
    deadline = time.time() + timeout
    while len(out) < want and time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0.0, deadline - time.time()))
        if not ready:
            break
        chunk = os.read(fd, want - len(out))
        if not chunk:
            break
        out += chunk
    return out


def loopback(fd, baud, size):
    """Write `size` bytes and check the same bytes come back through the jumper.

    The firmware's counters are cumulative (they live in the device until it is
    reset or V003_UART_CLEAR_STATS is sent, which the TTY driver does not
    expose), so every assertion here is about a *delta*: a test that compares an
    absolute count is a test that fails because of what an earlier run did.
    """
    configure(fd, baud)
    time.sleep(0.05)  # let the driver configure the firmware port
    tty.tcflush(fd, termios.TCIOFLUSH)
    reset_stats()
    before = read_stats()

    pattern = bytes((i * 11 + 3) & 0xFF for i in range(size))
    os.write(fd, pattern)
    got = read_for(fd, size, timeout=1.0 + size * 10.0 / baud * 4)

    stats = read_stats()
    delta = {k: stats[k] - before[k] for k in stats
             if isinstance(stats[k], int)}
    print(f"     {baud} baud: wrote {len(pattern)}, read {len(got)}, "
          f"poll delta={delta['polls']}, firmware tx+{delta['tx_bytes']} "
          f"rx+{delta['rx_bytes']} isr+{delta['isr']}")

    if not got:
        print(f"FAIL {baud} baud: {JUMPER_HINT}")
        failures.append(f"{baud} baud loopback")
        return None

    check(f"{baud} baud: the pattern comes back whole", got, pattern)
    check(f"{baud} baud: the driver and the firmware agree on the byte count",
          (delta["tx_bytes"], delta["rx_bytes"]), (len(pattern), len(got)))
    if before["isr"] == 255 and delta["isr"] == 0:
        skip(f"{baud} baud: the receiver interrupted once per byte",
             "the firmware's interrupt counter is saturated at 255 - the driver "
             "zeroes the counters when the port is opened, so this needs a "
             "fresh open (scripts/uart_test.py checks it from the pyusb side)")
    else:
        check(f"{baud} baud: the receiver interrupted once per byte",
              delta["isr"], len(got))
    check(f"{baud} baud: nothing was dropped in this step",
          (delta["tx_dropped"], delta["rx_dropped"]), (0, 0))
    check(f"{baud} baud: no framing, overrun or parity error",
          (delta["framing"], delta["overrun"], delta["parity"]), (0, 0, 0))

    stats["actual"] = read_stats()["actual"]
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bauds", default="9600,115200",
                    help="rates to loop back through the jumper")
    ap.add_argument("--flow-size", type=int, default=256,
                    help="bytes to write in one call at 9600 (transmit ring is 31)")
    args = ap.parse_args()

    if not os.path.exists(DEVICE):
        print(f"FAIL {DEVICE} is missing - is v003-uart.ko loaded?")
        print("     (sudo insmod usb-mfd.ko && sudo insmod v003-uart.ko)")
        return 1

    if not os.path.exists(STATS):
        print(f"FAIL {STATS} is missing")
        return 1

    print(f"info {DEVICE}, driver: "
          + " ".join(f"{k}={v}" for k, v in read_stats().items()))

    try:
        fd = os.open(DEVICE, os.O_RDWR | os.O_NOCTTY)
    except PermissionError as exc:
        print(f"FAIL cannot open {DEVICE}: {exc}")
        print("     install the udev rule for the group you are in:")
        print(f"       echo '{UDEV_RULE}' | sudo tee "
              "/etc/udev/rules.d/61-tty-v003-plugdev.rules")
        print("       sudo udevadm control --reload-rules && "
              "sudo udevadm trigger --subsystem-match=tty")
        return 1

    try:
        stats0 = read_stats()
        check("the port reports itself open while the file is", stats0["open"], 1)

        # -- the link, at two rates, through the jumper ------------------

        results = {}
        for baud in (int(b) for b in args.bauds.split(",")):
            results[baud] = loopback(fd, baud, 32)

        # the driver puts the rate the hardware really divides to into termios,
        # which is what a caller reads back - check the number it used
        for baud, stats in results.items():
            if stats is None:
                continue
            check(f"{baud} baud: the reported actual rate is within 0.5 %",
                  abs(stats["actual"] - baud) <= baud * 0.005, True)
            print(f"     {baud} baud: requested {baud}, the device reports "
                  f"{stats['actual']}")

        # -- transmit ring flow control ---------------------------------

        # the firmware's transmit ring holds 31 bytes, so a single write of
        # --flow-size bytes only completes because the driver hands the rest
        # over as the ring drains (tty_wakeup from the poll work)
        configure(fd, 9600)
        time.sleep(0.05)
        tty.tcflush(fd, termios.TCIOFLUSH)
        reset_stats()
        pattern = bytes((i * 5 + 1) & 0xFF for i in range(args.flow_size))
        before = read_stats()
        start = time.time()
        written = os.write(fd, pattern)
        got = read_for(fd, args.flow_size, timeout=2.0 + args.flow_size * 2e-3)
        took = time.time() - start
        after = read_stats()
        delta = {k: after[k] - before[k] for k in after
                 if isinstance(after[k], int)}
        wire = args.flow_size * 10.0 / 9600
        print(f"     9600 baud flow control: write() took {written} bytes in "
              f"{took * 1000:.0f} ms (the wire needs {wire * 1000:.0f} ms), "
              f"{len(got)} came back, firmware tx+{delta['tx_bytes']} "
              f"rx+{delta['rx_bytes']} tx_dropped+{delta['tx_dropped']} "
              f"rx_dropped+{delta['rx_dropped']}")
        check("write() accepts the whole buffer", written, args.flow_size)
        check("every byte survives the ring", got, pattern)
        check("the firmware counted every byte", delta["rx_bytes"], len(got))
        check("the firmware dropped nothing", 
              (delta["tx_dropped"], delta["rx_dropped"]), (0, 0))
        # The transmit ring holds 31 bytes, so this only completes if the driver
        # hands the rest over as the ring drains.  The lower bound is loose on
        # purpose: write() returns once the bytes are queued, and the last two
        # are still in the transmitter's shift register at that point.
        check("the write took about as long as the wire does",
              wire * 0.5 <= took <= wire * 3.0, True)

        # -- the receive path is polled ---------------------------------

        check("the poll work is running", read_stats()["polls"] > stats0["polls"],
              True)

        # -- closing disables the port and gives the pins back ----------

        os.close(fd)
        time.sleep(0.2)
        stats = read_stats()
        print("     after close: "
              + " ".join(f"{k}={v}" for k, v in stats.items()
                         if k in ("open", "enabled", "baud", "actual")))
        check("the port is closed", stats["open"], 0)
        check("the firmware port is disabled (this is what releases PD0/PD1)",
              stats["enabled"], 0)

        # -- and it can be opened again ---------------------------------

        fd = os.open(DEVICE, os.O_RDWR | os.O_NOCTTY)
        configure(fd, 115200)
        time.sleep(0.05)
        check("reopening enables it again", read_stats()["enabled"], 1)
        os.close(fd)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass

    print()
    if skipped:
        print(f"{len(skipped)} skipped: {', '.join(skipped)}")
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
