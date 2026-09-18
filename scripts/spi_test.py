#!/usr/bin/env python3
"""SPI bridge test for the CH32V003 vendor device (VID 1209, PID C303).

The firmware runs SPI1 (SCK=PC5, MOSI=PC6, MISO=PC7) as a USB <-> SPI bridge:
while SPI is enabled, every byte the host writes to EP2 OUT is clocked out on
MOSI and the byte sampled on MISO is queued to the EP3 IN FIFO.

Checks:
  1. SPI state/config round trip
  2. byte accounting (GET_STATS) and the EP3 IN response stream
  3. MISO sampling: PC7 is driven as a plain GPIO output (the GPIO module owns
     the pin, exactly like a real slave would) and the byte the SPI samples is
     verified - no external wiring needed
  4. SCK/MOSI pin levels (information only: the SPI releases MOSI/SCK as soon
     as it goes idle, so their static level is not asserted)
  5. disabling SPI restores the EP2 OUT echo behaviour
  6. soak: random payloads through both transfer paths (--soak N)

With a jumper between PC6 (MOSI) and PC7 (MISO) the test additionally verifies
true full duplex loopback; pass --loopback to make that a hard requirement and
--soak N to push N random payloads through both paths.

Note: a plain MOSI-MISO wire cannot verify the bit order, because every bit
returns on the same clock edge it left on.  Bit order (MSB first, `LSBFIRST` is
never set) and CPOL/CPHA follow the SPI_CTLR1 configuration - proving them needs
a scope or a real slave.

Requires pyusb, e.g.  python3 -m venv .venv && .venv/bin/pip install pyusb
"""

import random
import sys
import time

import usb.core
import usb.util

VID = 0x1209
PID = 0xC303

GENERIC_MODULE_ID = 0x00
GPIO_MODULE_ID = 0x01
SPI_MODULE_ID = 0x02

GET_FIFO_LEVEL = 0x33 | (GENERIC_MODULE_ID << 8)
GPIO_SET = 0x06 | (GPIO_MODULE_ID << 8)
GPIO_GET = 0x07 | (GPIO_MODULE_ID << 8)
GPIO_DIRECTION_INPUT = 0x0B | (GPIO_MODULE_ID << 8)
GPIO_DIRECTION_OUTPUT = 0x0C | (GPIO_MODULE_ID << 8)
SPI_ENABLE = 0x40 | (SPI_MODULE_ID << 8)
SPI_CONFIG = 0x41 | (SPI_MODULE_ID << 8)
SPI_GET_STATE = 0x42 | (SPI_MODULE_ID << 8)
SPI_GET_STATS = 0x43 | (SPI_MODULE_ID << 8)
SPI_SET_CS = 0x44 | (SPI_MODULE_ID << 8)
SPI_TRANSFER = 0x45 | (SPI_MODULE_ID << 8)
SPI_GET_RX = 0x46 | (SPI_MODULE_ID << 8)
SPI_GET_CS = 0x47 | (SPI_MODULE_ID << 8)

EP2_OUT = 0x02
EP3_IN = 0x83

PC4 = 36  # chip select for the control path transfers
PC5 = 37  # SCK
PC6 = 38  # MOSI
PC7 = 39  # MISO

PRESCALER = 0x03  # /16 -> 3 MHz at 48 MHz APB2
MODE = 0x00  # CPOL = 0, CPHA = 0

failures = []
warnings = []


def check(name, got, expected):
    ok = got == expected
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    if not ok:
        failures.append(name)
    return ok


def info(name, value):
    print(f"info {name}: {value!r}")


class Device:
    def __init__(self, dev):
        self.dev = dev
        cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]
        self.ep2 = usb.util.find_descriptor(intf, bEndpointAddress=EP2_OUT)
        self.ep3 = usb.util.find_descriptor(intf, bEndpointAddress=EP3_IN)
        if not (self.ep2 and self.ep3):
            raise RuntimeError(f"endpoints not found: {self.ep2} {self.ep3}")

    def ctrl(self, val, idx, length=0):
        bm = 0x40 if length == 0 else 0xC0
        return self.dev.ctrl_transfer(bm, 0x00, val, idx, length, timeout=2000)

    def u32(self, idx, val=0):
        return int.from_bytes(bytes(self.ctrl(val, idx, 4)), "little")

    # --- endpoint data path -------------------------------------------
    def spi_out(self, data):
        return self.ep2.write(bytes(data))

    def read_fifo(self, count, timeout=1.0):
        """Read `count` bytes from EP3 IN.

        The device always answers with whole 8 byte packets (an empty packet
        when nothing is queued), so reads must use an 8 byte buffer - a
        smaller one makes libusb report an overflow.
        """
        out = b""
        deadline = time.time() + timeout
        while len(out) < count and time.time() < deadline:
            chunk = bytes(self.ep3.read(8, timeout=500))
            if not chunk:
                continue
            out += chunk
        return out[:count]

    def fifo_level(self):
        return self.u32(GET_FIFO_LEVEL)

    def drain(self):
        while self.fifo_level() > 0:
            if not self.ep3.read(8, timeout=500):
                break

    # --- control requests ---------------------------------------------
    def spi_state(self):
        state = self.u32(SPI_GET_STATE)
        return state & 0xFF, (state >> 8) & 0xFF, (state >> 16) & 0xFF

    def spi_bytes(self):
        return self.u32(SPI_GET_STATS)

    def spi_config(self):
        self.ctrl((PRESCALER << 8) | MODE, SPI_CONFIG)

    def spi_set_cs(self, pin):
        self.ctrl(pin, SPI_SET_CS)

    def spi_cs(self):
        cs = self.u32(SPI_GET_CS)
        return None if cs == 0xFFFF else cs

    def spi_transfer(self, data):
        """Atomic transfer through the EP0 data stage."""
        self.ctrl_out_data(SPI_TRANSFER, data)

    def spi_rx(self, length):
        return self.ctrl_in_data(SPI_GET_RX, length)

    def ctrl_out_data(self, idx, data, val=0):
        return self.dev.ctrl_transfer(0x40, 0x00, val, idx, bytes(data),
                                      timeout=2000)

    def ctrl_in_data(self, idx, length, val=0):
        return bytes(self.ctrl(val, idx, length))

    def gpio(self, pin):
        return self.u32(GPIO_GET, (pin << 8) | 0x00)

    def gpio_dir_output(self, pin, value):
        self.ctrl((pin << 8) | value, GPIO_DIRECTION_OUTPUT)

    def gpio_dir_input(self, pin):
        self.ctrl((pin << 8) | 0x00, GPIO_DIRECTION_INPUT)


def miso_test(d, value):
    """Drive PC7 as a GPIO output and check what the SPI samples."""
    d.drain()
    before = d.spi_bytes()

    d.gpio_dir_output(PC7, value)
    d.spi_out(b"\x5a")
    sampled = d.read_fifo(1)

    check(f"MISO driven {'high' if value else 'low'} is sampled as "
          f"0x{'ff' if value else '00'}", sampled, bytes([0xFF if value else 0x00]))
    check(f"byte clocked with MISO {'high' if value else 'low'}",
          d.spi_bytes() - before, 1)

    # hand the pin back to the SPI peripheral
    d.spi_config()


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--soak")]
    require_loopback = "--loopback" in argv
    soak = 0
    for i, a in enumerate(sys.argv):
        if a == "--soak" and i + 1 < len(sys.argv):
            soak = int(sys.argv[i + 1])

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("FAIL: device not found")
        return 1

    d = Device(dev)
    if d.dev.is_kernel_driver_active(0):
        d.dev.detach_kernel_driver(0)
        usb.util.claim_interface(d.dev, 0)

    # --- 1. state / config --------------------------------------------
    enabled, mode, prescaler = d.spi_state()
    info("spi state at boot", f"enabled={enabled} mode={mode} prescaler={prescaler}")

    d.ctrl(0, SPI_ENABLE)
    d.drain()
    # mode/prescaler are remembered across enable/disable, only "enabled" resets
    check("spi disabled after reset", d.spi_state()[0], 0)
    check("fifo empty after reset", d.fifo_level(), 0)

    d.spi_config()
    d.ctrl(1, SPI_ENABLE)
    check("spi state after enable", d.spi_state(), (1, MODE, PRESCALER))

    # --- 2. byte accounting and the response stream ---------------------
    d.drain()
    before = d.spi_bytes()
    payload = bytes([0x01, 0x00, 0x80, 0xFF, 0xAA, 0x55, 0x01, 0x00])
    check("EP2 OUT write", d.spi_out(payload), len(payload))
    check("MOSI bytes clocked", d.spi_bytes() - before, len(payload))
    check("EP3 IN fifo level", d.fifo_level(), len(payload))

    received = d.read_fifo(len(payload))
    # A real loopback must return the payload *and* the all-zero pattern: a
    # floating MISO reads 0xff, so the zero pattern also catches a reading that
    # only looks like a loopback (e.g. stale FIFO content).
    d.spi_out(b"\x00" * len(payload))
    zero_pattern = d.read_fifo(len(payload))
    loopback = received == payload and zero_pattern == b"\x00" * len(payload)

    if loopback:
        print(f"ok   SPI full duplex loopback (PC6<->PC7 jumper): {received.hex()}")
    else:
        msg = (f"no working PC6(MOSI)<->PC7(MISO) loopback: sampled "
               f"{received.hex()} / {zero_pattern.hex()} instead of "
               f"{payload.hex()} / 00..")
        if require_loopback:
            check("SPI full duplex loopback", received, payload)
        else:
            print(f"warn {msg} (pass --loopback to require it)")
            warnings.append(msg)

    # --- 3. MISO sampling (no wiring required) --------------------------
    if require_loopback and not loopback:
        print("skip MISO sampling: loopback is required but not wired")
    else:
        miso_test(d, 0)
        miso_test(d, 1)

    # --- 3b. atomic transfer over the control path ----------------------
    # V003_SPI_TRANSFER takes the EP0 data stage as the MOSI payload, drives
    # chip select around it and keeps the sampled MISO bytes for
    # V003_SPI_GET_RX.  Much cheaper than the endpoint path for small
    # transfers (one control round trip instead of 1 ms frames per packet).
    check("no chip select by default", d.spi_cs(), None)
    d.spi_set_cs(PC4)
    check("chip select configured", d.spi_cs(), PC4)
    check("chip select idles high", d.gpio(PC4), 1)

    payload = bytes([0x01, 0x80, 0xFF, 0x00, 0xAA, 0x55, 0x12, 0x34])
    d.gpio_dir_output(PC7, 0)
    before = d.spi_bytes()
    d.spi_transfer(payload)
    check("control transfer clocked payload", d.spi_bytes() - before,
          len(payload))
    check("control transfer sampled MISO low",
          d.spi_rx(len(payload)), b"\x00" * len(payload))
    check("chip select deasserted", d.gpio(PC4), 1)

    d.gpio_dir_output(PC7, 1)
    d.spi_transfer(payload)
    check("control transfer sampled MISO high",
          d.spi_rx(len(payload)), b"\xff" * len(payload))

    d.spi_transfer(b"\x11\x22\x33\x44\x55")
    check("control transfer rx read is clamped", len(d.spi_rx(32)), 5)

    # a 64 byte transfer is the largest a single data stage carries
    d.spi_transfer(bytes(64))
    check("64 byte transfer", d.spi_bytes() - before, 2 * len(payload) + 5 + 64)

    d.spi_config()  # hand MISO/SCK/MOSI back to the SPI peripheral
    d.spi_set_cs(0xFFFF)
    check("chip select released", d.spi_cs(), None)

    # --- 4. pin levels --------------------------------------------------
    d.drain()
    check("SCK idles low", d.gpio(PC5), 0)
    info("MOSI level after transfers", d.gpio(PC6))

    # --- 5. disable restores the echo path -------------------------------
    d.ctrl(0, SPI_ENABLE)
    check("spi enabled after disable", d.spi_state()[0], 0)

    d.drain()
    d.gpio_dir_input(PC7)
    d.spi_out(b"\xde\xad\xbe\xef")
    check("fifo level after echo write", d.fifo_level(), 4)
    check("EP2 OUT echo after disable", d.read_fifo(4), b"\xde\xad\xbe\xef")

    # --- 6. soak: random payloads through both paths --------------------
    if soak and loopback:
        # section 5 disabled SPI again, and the soak must not silently fall
        # back to the echo path, so re-enable and verify the byte accounting
        d.spi_config()
        d.ctrl(1, SPI_ENABLE)
        check("spi enabled for soak", d.spi_state()[0], 1)
        d.drain()

        rng = random.Random(0x003)
        bad = 0
        started = time.time()
        for _ in range(soak):
            p = bytes(rng.randrange(256) for _ in range(8))
            before = d.spi_bytes()
            d.spi_out(p)
            if d.spi_bytes() - before != len(p) or d.read_fifo(8) != p:
                bad += 1
        check(f"soak EP path ({soak} x 8 B random, clocked + looped back)",
              bad, 0)
        print(f"info EP soak: {soak / (time.time() - started):.0f} round trips/s")

        bad = 0
        started = time.time()
        for i in range(soak):
            n = 1 + (i * 7) % 64
            p = bytes(rng.randrange(256) for _ in range(n))
            before = d.spi_bytes()
            d.spi_transfer(p)
            if d.spi_bytes() - before != n or d.spi_rx(n) != p:
                bad += 1
        check(f"soak control path ({soak} transfers, lengths 1..64)", bad, 0)
        print(f"info control soak: {soak / (time.time() - started):.0f} "
              f"transfers/s")
    elif soak:
        print("info soak skipped: needs a working MOSI<->MISO loopback")

    if failures:
        print(f"\nFAILED: {len(failures)} check(s): {', '.join(failures)}")
        return 1

    print("\nALL TESTS PASSED"
          + ("" if loopback else f" ({len(warnings)} warning: no loopback jumper)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
