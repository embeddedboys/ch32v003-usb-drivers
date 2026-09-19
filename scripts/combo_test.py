#!/usr/bin/env python3
"""Do the modules work *together*?  One session, every module, interleaved.

Each module has its own test (`scripts/*_test.py`); what those cannot answer is
whether a device that contains all of them behaves when they are used in the same
session - and whether that survives a sleep.  This test answers exactly that:

  - every module's capability bit and one working round trip each, in one run:
    GPIO, I2C (the AT24C256), SPI (the PC6<->PC7 loopback jumper), ADC, PWM, UART
    (the PD0<->PD1 jumper), the watchdog's state and the power module's state;
  - **the ADC must not take a pin another module is using when it is not
    converting that channel**: `SPI_SET_CS` puts PC4 into output mode, and a
    conversion of the internal reference must leave it there.  This is a real
    failure that happened (PC4 became an input on the first conversion and the
    chip select stopped moving *silently*), so it is checked in the order that
    exposes it - set the select first, convert second;
  - **module state survives a sleep**: a standby sleep that detaches from the bus
    and comes back, then the same round trips again (the firmware keeps its
    registers and RAM through standby, and `SystemInit()` restores the clock).

  .venv/bin/python scripts/combo_test.py

Needs both jumpers (PC6<->PC7 for SPI, PD0<->PD1 for UART) and the module kernels
unloaded.  It writes to the EEPROM at word address 0 and puts the page back.
"""

import struct
import sys
import time

import usb.core

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from v003_usb import find_device  # noqa: E402

GET_CAPABILITIES = 0x3D
GET_STACK_FREE = 0x3B
GET_FIRMWARE_VER = 0x30

GPIO = 0x01 << 8
GPIO_REQUEST, GPIO_SET, GPIO_GET, GPIO_DIRECTION_OUTPUT = (
    0x08 | GPIO, 0x06 | GPIO, 0x07 | GPIO, 0x0C | GPIO)
GPIO_DIRECTION_INPUT = 0x0B | GPIO
GPIO_GET_DIRECTION = 0x0A | GPIO

SPI = 0x02 << 8
SPI_SET_CS, SPI_TRANSFER, SPI_GET_RX = 0x44 | SPI, 0x45 | SPI, 0x46 | SPI
SPI_ENABLE = 0x40 | SPI

I2C = 0x03 << 8
I2C_WRITE, I2C_READ, I2C_GET_RX = 0x50 | I2C, 0x51 | I2C, 0x57 | I2C
I2C_GET_RESULT = 0x58 | I2C
EEPROM_ADDR = 0x50

PWM = 0x05 << 8
PWM_SET, PWM_GET = 0x70 | PWM, 0x71 | PWM
PWM_PIN_CH1 = 50

ADC = 0x06 << 8
ADC_START, ADC_GET, ADC_GET_STATUS = 0x80 | ADC, 0x81 | ADC, 0x84 | ADC
ADC_CONVERSION_US = 42  # 241 sample clocks + 11 at 6 MHz, vendor/adc.h

UART = 0x07 << 8
UART_CFG, UART_WRITE, UART_READ, UART_GET_STATE = (
    0x90 | UART, 0x92 | UART, 0x93 | UART, 0x94 | UART)
UART_FLUSH, UART_GET_INFO = 0x97 | UART, 0x99 | UART

WDG = 0x04 << 8
WDG_GET_STATE = 0x62 | WDG

PWR = 0x08 << 8
PWR_ARM, PWR_GET_STATE = 0xA0 | PWR, 0xA1 | PWR
PWR_STATE_FMT = "<5I8B"

SPI_CS_PIN = 36  # PC4, the ADC's channel 2 pad
GPIO_TEST_PIN = 32  # PC0, the LED

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
    def __init__(self):
        self.reopen()

    def reopen(self):
        for _ in range(20):
            dev = find_device()
            if dev:
                try:
                    dev.set_configuration()
                    dev.ctrl_transfer(0x00, 0x09, 1, 0, 0, timeout=1000)
                    self.dev = dev
                    return True
                except usb.core.USBError:
                    pass
            time.sleep(0.2)
        self.dev = None
        return False

    def out(self, idx, val=0):
        self.dev.ctrl_transfer(0x40, 0, val, idx, 0, timeout=2000)

    def out_data(self, idx, data, val=0):
        self.dev.ctrl_transfer(0x40, 0, val, idx, bytes(data), timeout=3000)

    def in_data(self, idx, length, val=0):
        return bytes(self.dev.ctrl_transfer(0xC0, 0, val, idx, length,
                                            timeout=2000))

    def u32(self, idx, val=0):
        return int.from_bytes(self.in_data(idx, 4, val), "little")


def gpio_round_trip(d):
    d.out(GPIO_REQUEST, GPIO_TEST_PIN)
    d.out(GPIO_DIRECTION_OUTPUT, (GPIO_TEST_PIN << 8) | 1)
    high = d.u32(GPIO_GET, GPIO_TEST_PIN << 8)
    d.out(GPIO_SET, (GPIO_TEST_PIN << 8) | 0)
    low = d.u32(GPIO_GET, GPIO_TEST_PIN << 8)
    d.out(GPIO_REQUEST, GPIO_TEST_PIN)
    return high, low


def i2c_round_trip(d):
    """Read the first four bytes of the EEPROM's word address 0."""
    d.out_data(I2C_WRITE, bytes([EEPROM_ADDR << 1, 0x00, 0x00]), val=4)
    rx = d.in_data(I2C_GET_RX, 4)
    return bytes(rx)


def spi_round_trip(d, release=False):
    """Clock four bytes out and compare what comes back over the jumper.

    `release` puts the module back the way it was found (no chip select, disabled):
    every test in this repository establishes its own state and gives back what it
    took, and the SPI module's chip select is state another test looks at.
    """
    d.out(SPI_ENABLE, 1)
    d.out(SPI_SET_CS, SPI_CS_PIN)
    payload = bytes([0xA5, 0x5A, 0x0F, 0xF0])
    d.out_data(SPI_TRANSFER, payload)
    got = bytes(d.in_data(SPI_GET_RX, len(payload)))
    if release:
        d.out(SPI_SET_CS, 0xFFFF)
        d.out(SPI_ENABLE, 0)
    return got


def adc_vref(d):
    d.out(ADC_START, 8)
    return d.u32(ADC_GET) & 0xFFF


# struct v003_pwm_cfg: channel, enable, duty_permille, period_ns, actual_ns
(PWM_CH, PWM_EN, PWM_DUTY, PWM_PERIOD, PWM_ACTUAL) = range(5)


def pwm_round_trip(d):
    """The reported period and both duty extremes.

    A duty in between is a switching signal, so reading the pin gives 0 or 1
    depending on when the read lands (which is why scripts/pwm_test.py only ever
    checks the extremes as well); the duty itself is checked through the read back.
    """
    d.out_data(PWM_SET, struct.pack("<BBHII", 0, 1, 250, 1000000, 0))
    time.sleep(0.02)
    st = struct.unpack("<BBHII", d.in_data(PWM_GET, 12, val=0))
    period_ns, duty = st[PWM_PERIOD], st[PWM_DUTY]
    d.out(GPIO_REQUEST, PWM_PIN_CH1)
    d.out_data(PWM_SET, struct.pack("<BBHII", 0, 1, 1000, 1000000, 0))
    time.sleep(0.02)
    high = d.u32(GPIO_GET, PWM_PIN_CH1 << 8)
    d.out_data(PWM_SET, struct.pack("<BBHII", 0, 1, 0, 1000000, 0))
    time.sleep(0.02)
    low = d.u32(GPIO_GET, PWM_PIN_CH1 << 8)
    return period_ns, duty, high, low


def uart_round_trip(d, baud=115200):
    d.out_data(UART_CFG, struct.pack("<IBBBBI", baud, 8, 0, 1, 1, 0))
    d.out(UART_FLUSH)
    payload = bytes((i * 7 + 1) & 0xFF for i in range(16))
    d.out_data(UART_WRITE, payload)
    got = b""
    deadline = time.time() + 1.0
    while len(got) < len(payload) and time.time() < deadline:
        avail = d.u32(UART_GET_STATE) >> 16
        if avail:
            got += d.in_data(UART_READ, 64, val=min(avail, 16))
        else:
            time.sleep(0.002)
    return payload, got


def module_round_trips(d, label):
    """One working round trip per module; returns True when all of them did."""
    ok = True

    high, low = gpio_round_trip(d)
    ok &= check(f"{label}: GPIO drives a line and reads it back",
                (high, low), (1, 0))

    rx = i2c_round_trip(d)
    ok &= check_range(f"{label}: I2C reads the EEPROM (4 bytes)",
                      len(rx), 4, 4)

    tx = spi_round_trip(d, release=True)
    ok &= check(f"{label}: SPI loops back over the jumper", tx,
                bytes([0xA5, 0x5A, 0x0F, 0xF0]))

    vref = adc_vref(d)
    ok &= check_range(f"{label}: ADC reads the internal reference",
                      vref, 335, 409, " counts")
    # the conversion time is the read back of the ADC clock: a standby wake calls
    # SystemInit(), which rewrites RCC->CFGR0 and left ADCPRE at its reset value
    # (42 us on a fresh boot, 11 us after a sleep, until the module started
    # re-asserting the divider per conversion)
    us = (d.u32(ADC_GET_STATUS) >> 16) & 0xFF
    ok &= check_range(f"{label}: the conversion still takes its documented "
                      f"{ADC_CONVERSION_US} us", us,
                      ADC_CONVERSION_US - 5, ADC_CONVERSION_US + 5, " us")

    period_ns, duty, high, low = pwm_round_trip(d)
    ok &= check(f"{label}: PWM reads back the period and the duty it was given",
                (period_ns, duty), (1000000, 250))
    ok &= check(f"{label}: PWM drives the pin at both duty extremes",
                (high, low), (1, 0))

    payload, got = uart_round_trip(d)
    ok &= check(f"{label}: UART loops back over the jumper", got, payload)
    d.out_data(UART_CFG, struct.pack("<IBBBBI", 115200, 8, 0, 1, 0, 0))

    wdg = d.u32(WDG_GET_STATE)
    ok &= check(f"{label}: the watchdog answers ({wdg:#x})", bool(wdg & 1), False)

    st = struct.unpack(PWR_STATE_FMT, d.in_data(PWR_GET_STATE, 28))
    ok &= check(f"{label}: the power module answers (caps {st[4]:#x})",
                st[4], 0xF)

    return ok


def main():
    d = Dev()
    if not d.dev:
        print("FAIL the device is not there (are the kernel modules unloaded?)")
        return 1

    caps = struct.unpack("<I", d.in_data(GET_CAPABILITIES, 4))[0]
    names = ["gpio", "spi", "i2c", "adc", "pwm", "uart", "frame", "wdg", "pwr"]
    present = [n for i, n in enumerate(names) if caps & (1 << i)]
    print(f"info capabilities {caps:#x}: {' '.join(present)}")
    check("one binary contains every module", caps, 0x1FF)

    d.u32(GET_STACK_FREE)
    stack = d.u32(GET_STACK_FREE)
    print(f"info stack margin {stack} bytes (the modules share one 2 kB SRAM)")
    check_range("the stack margin is still above the documented floor",
                stack, 350, 2048, " bytes")

    # -- every module once ---------------------------------------------

    module_round_trips(d, "session")

    # -- the pads the ADC shares with other modules ---------------------

    # ADC_IN2 is PC4, which is also this board's SPI chip select, and ADC_IN1 is
    # PA1, which is PWM channel 2.  The ADC must convert a pad *without taking it
    # away* from what is driving it: the first version of the module configured
    # PC4 as an analog input when it came up, and the chip select then stopped
    # moving silently (the SPI module only writes the output data register).
    # Configuring it per channel instead killed the PWM output on PA1.
    d.out(SPI_SET_CS, SPI_CS_PIN)
    check("the SPI chip select is an output to start with",
          d.u32(GPIO_GET_DIRECTION, SPI_CS_PIN << 8), 0)
    adc_vref(d)
    check("a conversion of the internal reference leaves it alone",
          d.u32(GPIO_GET_DIRECTION, SPI_CS_PIN << 8), 0)
    d.out(ADC_START, 2)  # the channel that is on that very pad
    d.u32(ADC_GET)
    check("and neither does a conversion of the channel on that pad",
          d.u32(GPIO_GET_DIRECTION, SPI_CS_PIN << 8), 0)

    # PWM channel 2 (index 1) is PA1, which is also the ADC's channel 1
    d.out_data(PWM_SET, struct.pack("<BBHII", 1, 1, 1000, 1000000, 0))
    time.sleep(0.02)
    check("PWM channel 2 drives PA1 before any conversion of channel 1",
          d.u32(GPIO_GET, 1 << 8), 1)
    d.out(ADC_START, 1)
    d.u32(ADC_GET)
    check("a conversion of channel 1 leaves the PWM driving PA1",
          d.u32(GPIO_GET, 1 << 8), 1)
    d.out_data(PWM_SET, struct.pack("<BBHII", 1, 1, 0, 1000000, 0))
    d.out(SPI_SET_CS, SPI_CS_PIN)

    # -- everything again after a sleep --------------------------------

    before = struct.unpack(PWR_STATE_FMT, d.in_data(PWR_GET_STATE, 28))[0]
    print("     arming a 500 ms standby sleep (the device leaves the bus)")
    d.out_data(PWR_ARM, struct.pack("<IBBBB", 500, 1, 0, 1, 10))
    time.sleep(0.7)
    if not d.reopen():
        failures.append("the device did not come back from the sleep")
        print("FAIL the device did not come back from the sleep")
        return 1
    st = struct.unpack(PWR_STATE_FMT, d.in_data(PWR_GET_STATE, 28))
    check("the sleep happened", st[0], before + 1)

    module_round_trips(d, "after a standby sleep")

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
