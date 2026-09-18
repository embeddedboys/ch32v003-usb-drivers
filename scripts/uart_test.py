#!/usr/bin/env python3
"""UART test for the CH32V003 bridge: USART1 remapped to PD0 (TX) / PD1 (RX).

The receive path needs a **jumper between PD0 and PD1**, the same way the SPI
loopback test needs one between PC6 and PC7, and for a reason specific to this
part: PD1 is the chip's SWIO debug pin, so with the programmer attached it is
held high and cannot be used as an input on its own.  Tie it to PD0 and the
transmitter becomes the peer of the receiver.

What is checked, and why each one is the check that would catch the bug:

  - `actual_baud` read back from the device for four rates.  The BRR arithmetic
    is where a factor goes missing (the PWM module shipped a factor of 1000 for
    an afternoon), and 921600 is the rate the manual itself uses as its rounding
    example: 48000000 / 52 = 923076, +0.16 %;
  - a byte pattern that survives all the way out of the transmitter, through the
    jumper, into the receiver, through the interrupt, the ring and out over USB.
    Without the jumper this is the check that fails, and it says so;
  - the interrupt count grows by exactly one per received byte, which separates
    "the bytes arrived" from "the bytes arrived and every one interrupted";
  - the transmit ring's flow control: queueing faster than the line drains must
    be reported (tx_queued) and the overflow counted (tx_dropped), never
    silently dropped;
  - the receive ring's overflow, by flooding it without reading;
  - configuration validation: an out of range baud, word length, parity or stop
    bit count must be refused as a whole, leaving the previous configuration in
    place.

The test leaves the port disabled, which releases both pins - that is what makes
the board flashable again afterwards (a driven PD0 holds the SWIO line through
the jumper).
"""

import argparse
import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from v003_usb import find_device  # noqa: E402

UART_MODULE = 0x07
UART_CONFIG = 0x90 | (UART_MODULE << 8)
UART_GET_CFG = 0x91 | (UART_MODULE << 8)
UART_WRITE = 0x92 | (UART_MODULE << 8)
UART_READ = 0x93 | (UART_MODULE << 8)
UART_GET_STATE = 0x94 | (UART_MODULE << 8)
UART_GET_COUNTS = 0x95 | (UART_MODULE << 8)
UART_GET_ERRORS = 0x96 | (UART_MODULE << 8)
UART_FLUSH = 0x97 | (UART_MODULE << 8)
UART_GET_INFO = 0x99 | (UART_MODULE << 8)
UART_CLEAR_STATS = 0x9A | (UART_MODULE << 8)

GET_CAPABILITIES = 0x3D
CAP_UART = 1 << 5

TX_PIN = 48  # PD0
RX_PIN = 49  # PD1

CFG_STRUCT = "<IBBBBI"  # baud, data_bits, parity, stop_bits, enable, actual
BAUD_MIN = 733
BAUD_MAX = 3000000

failures = []
jumper_missing = False
# a single-producer/single-consumer ring of N bytes holds N-1: the head==tail
# test cannot tell "empty" from "full"
TX_RING_HOLDS = 32 - 1


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


class Dev:
    def __init__(self, dev):
        self.dev = dev
        try:
            dev.set_configuration()
        except usb.core.USBError:
            pass
        # endpoint toggles: without this the first request can be dropped
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

    def config(self, baud, data_bits=8, parity=0, stop_bits=1, enable=1):
        cfg = struct.pack(CFG_STRUCT, baud, data_bits, parity, stop_bits,
                          enable, 0)
        self.out_data(UART_CONFIG, cfg)

    def get_cfg(self):
        return struct.unpack(CFG_STRUCT, self.in_data(UART_GET_CFG, 12))

    def write(self, data):
        self.out_data(UART_WRITE, data)

    def state(self):
        """(tx_queued, rx_available, tx_dropped, rx_dropped)"""
        val = self.u32(UART_GET_STATE)
        return (val >> 24, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF)

    def counts(self):
        """(tx_bytes, rx_bytes) - 16 bit, wrapping"""
        val = self.u32(UART_GET_COUNTS)
        return (val >> 16, val & 0xFFFF)

    def errors(self):
        """(isr_entries, overrun, framing, parity)"""
        val = self.u32(UART_GET_ERRORS)
        return (val >> 24, (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF)

    def read(self, most=64, timeout=0.5):
        """Drain whatever the receive ring holds right now.  The ring wraps, so
        a reply can stop at the wrap point; loop until it is empty."""
        out = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            avail = self.state()[1]
            if not avail:
                break
            out += self.in_data(UART_READ, 64, val=min(avail, most))
        return out

    def read_exact(self, want, timeout=1.0):
        """Read until `want` bytes have arrived.  A gap between bytes is not the
        end of a transfer: at 9600 baud a byte takes 1.04 ms while a control
        transfer takes about 3 ms, so the ring is empty between bytes and a
        drain-until-empty loop would stop early."""
        out = b""
        deadline = time.time() + timeout
        while len(out) < want and time.time() < deadline:
            avail = self.state()[1]
            if not avail:
                time.sleep(0.001)
                continue
            out += self.in_data(UART_READ, 64, val=min(avail, want - len(out)))
        return out

    def write_all(self, data, timeout=2.0):
        """Send everything, using tx_queued as flow control instead of assuming
        the ring is big enough.  This is how a host has to use the module: the
        transmit ring holds TX_RING_HOLDS bytes and reports its backlog."""
        deadline = time.time() + timeout
        sent = 0
        while sent < len(data):
            if time.time() > deadline:
                raise usb.core.USBTimeoutError("transmit ring never drained")
            room = TX_RING_HOLDS - self.state()[0]
            if room <= 0:
                time.sleep(0.001)
                continue
            chunk = data[sent:sent + min(room, 64)]
            self.write(chunk)
            sent += len(chunk)

    def flush(self):
        self.out(UART_FLUSH)

    def clear_stats(self):
        """Zero the drop and error counters, which are 8 bit and would otherwise
        carry a previous run's saturating value into this one."""
        self.out(UART_CLEAR_STATS)


def main():
    global jumper_missing

    ap = argparse.ArgumentParser()
    ap.add_argument("--bauds", default="9600,115200,921600,3000000")
    ap.add_argument("--size", type=int, default=32,
                    help="bytes per loopback transfer (default 32)")
    ap.add_argument("--no-jumper", action="store_true",
                    help="skip the loopback checks (no PD0<->PD1 jumper)")
    args = ap.parse_args()

    bauds = [int(b) for b in args.bauds.split(",")]
    dev = Dev(find_device())

    try:
        return run_checks(dev, bauds, args)
    finally:
        # Always let go of the pins.  PD1 is the SWIO debug pin and this bench
        # jumpers it to PD0, so a firmware that keeps the transmitter driving an
        # idle high line also keeps the programmer out (notes/uart.md); the test
        # is not allowed to leave the board in that state, not even when a check
        # failed or an exception came through.
        try:
            dev.config(115200, enable=0)
            print("     port disabled: both pins released (PD0 released, so the "
                  "programmer can talk to PD1/SWIO again)")
        except usb.core.USBError as exc:
            print(f"     could not disable the port: {exc}")


def run_checks(dev, bauds, args):
    global jumper_missing

    caps_raw = dev.in_data(GET_CAPABILITIES, 16)
    caps, _ngpio, _nadc, _npwm, nuart, rlo, rhi = struct.unpack(
        "<IBBBBII", caps_raw)
    reserved = rlo | (rhi << 32)

    if not caps & CAP_UART:
        print("FAIL the capability report does not offer a UART")
        return 1

    check("the capability report offers one UART", nuart, 1)
    check(f"pin {TX_PIN} (PD0, TX) is reserved",
          bool(reserved & (1 << TX_PIN)), True)
    check(f"pin {RX_PIN} (PD1, RX) is reserved",
          bool(reserved & (1 << RX_PIN)), True)

    info = dev.u32(UART_GET_INFO)
    print(f"     ring size {info >> 16} bytes, {info & 0xffff} port(s)")

    # -- configuration validation --------------------------------------

    dev.config(115200)
    before = dev.get_cfg()
    check("a valid configuration is accepted", (before[0], before[4]), (115200, 1))

    for bad, what in ((100, f"a baud below {BAUD_MIN}"),
                      (4000000, f"a baud above {BAUD_MAX}"),
                      (0, "baud 0")):
        dev.config(bad)
        check(f"{what} is refused", dev.get_cfg()[0], before[0])
    for kwargs, what in ((dict(data_bits=7), "7 data bits"),
                         (dict(data_bits=10), "10 data bits"),
                         (dict(parity=3), "parity 3"),
                         (dict(stop_bits=0), "0 stop bits"),
                         (dict(stop_bits=3), "3 stop bits")):
        dev.config(115200, **kwargs)
        cfg = dev.get_cfg()
        check(f"{what} is refused", (cfg[0], cfg[1], cfg[2], cfg[3]),
              (before[0], before[1], before[2], before[3]))

    # -- baud accuracy, as the hardware really divides ------------------

    for baud in bauds:
        if baud < BAUD_MIN or baud > BAUD_MAX:
            continue
        dev.config(baud)
        actual = dev.get_cfg()[5]
        # BRR is what divides HCLK, so the error is bounded by half a BRR step
        check_range(f"{baud} baud is generated within 0.5 %", actual,
                    int(baud * 0.995), int(baud * 1.005))

    # -- loopback: the transmitter, the jumper, the receiver ------------

    if args.no_jumper:
        print("skip loopback checks: --no-jumper")
    else:
        for baud in bauds:
            if baud < BAUD_MIN or baud > BAUD_MAX:
                continue
            dev.config(baud)
            dev.clear_stats()
            dev.flush()
            time.sleep(0.02)
            dev.read()  # anything left over
            dev.clear_stats()  # the flush above is a drop of its own

            isr0 = dev.errors()[0]
            tx0, _ = dev.counts()
            pattern = bytes((i * 7 + 1) & 0xFF for i in range(args.size))
            dev.write_all(pattern)
            got = dev.read_exact(args.size, timeout=1.0 + args.size / baud)
            isr1 = dev.errors()[0]
            tx1, rx1 = dev.counts()

            st = dev.state()
            print(f"     {baud} baud: state tx_queued={st[0]} rx_avail={st[1]} "
                  f"tx_dropped={st[2]} rx_dropped={st[3]}, "
                  f"tx_bytes+{tx1 - tx0} rx_bytes+{rx1 - tx1 * 0}, "
                  f"isr+{isr1 - isr0}")
            if not got:
                jumper_missing = True
                print(f"FAIL {baud} baud: nothing came back - is the PD0<->PD1 "
                      f"jumper in place?  (tx_bytes delta "
                      f"{tx1 - tx0}, isr delta {isr1 - isr0})")
                failures.append(f"{baud} baud loopback")
                continue

            check(f"{baud} baud: the pattern comes back whole", got, pattern)
            check(f"{baud} baud: every received byte interrupted once",
                  isr1 - isr0, len(got))
            check(f"{baud} baud: the transmitter counted the same bytes",
                  tx1 - tx0, len(pattern))

    # -- error counters stay clean on clean traffic ---------------------

    errs = dev.errors()
    print(f"     error counters: isr={errs[0]} overrun={errs[1]} "
          f"framing={errs[2]} parity={errs[3]}")
    check("no receive overrun during the run", errs[1], 0)
    if not jumper_missing:
        check("no framing error during the run", errs[2], 0)

    # -- transmit ring flow control -------------------------------------

    # the slowest rate the device accepts makes the ring fill up: 32 bytes at
    # 733 baud is 437 ms, far longer than the writes take
    dev.config(BAUD_MIN)
    dev.flush()
    dev.read()
    dev.clear_stats()
    st0 = list(dev.state())
    tx0, _ = dev.counts()
    sent = 0
    for _ in range(4):  # 4 x 32 bytes into a ring that holds 31
        dev.write(bytes([0xA5] * 32))
        sent += 32
    st1 = list(dev.state())
    tx1, _ = dev.counts()
    print(f"     after queueing {sent} bytes at {BAUD_MIN} baud: state {st1}")
    check("the ring reports a backlog", st1[0] > 0, True)
    check("the backlog is inside the ring", st1[0] <= TX_RING_HOLDS, True)
    check("what did not fit is counted as dropped",
          (tx1 - tx0) + st1[2] - st0[2], sent)
    st2 = list(dev.state())
    time.sleep(0.6)
    check("the ring drains into the line", dev.state()[0] < st2[0], True)

    # -- receive ring overflow -----------------------------------------

    # 115200 baud delivers a byte every 87 us; 160 bytes without reading
    # overflows the 64 byte ring, and the overflow has to be counted
    dev.config(115200)
    dev.flush()
    dev.read()
    dev.clear_stats()
    st0 = list(dev.state())
    for _ in range(5):
        dev.write(bytes([0x5A] * 32))
        time.sleep(0.02)
    time.sleep(0.05)
    st1 = list(dev.state())
    print(f"     after flooding the receiver: state {st1}")
    if jumper_missing:
        print("skip receive ring overflow: the jumper is missing")
    else:
        check("the receive ring filled up", st1[1], 63)
        check("the overflow was counted", st1[3] > st0[3], True)

    # -- flush drops what is held, and is counted -----------------------

    if jumper_missing:
        # nothing is arriving, so there is nothing to flush: make some
        dev.write(bytes([0x11] * 16))
        time.sleep(0.05)
    avail0 = dev.state()[1]
    dev.clear_stats()
    dev.flush()
    st = dev.state()
    check("flush empties the receive ring", st[1], 0)
    check("flush counts what it dropped", st[3], avail0)

    # -- disable, which is what releases the pins ----------------------

    dev.config(115200, enable=0)
    check("the port can be disabled", dev.get_cfg()[4], 0)

    print()
    if jumper_missing:
        print("NOTE the receive path was not verified: put a jumper between "
              "PD0 and PD1 and run again")
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
