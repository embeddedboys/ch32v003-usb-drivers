# CH32V003 USB Drivers

This project is based on [rv003usb](https://github.com/cnlohr/rv003usb).

| 硬件信息   |            |                                         |
| ---------- | ---------- | --------------------------------------- |
| MCU        | CH32V003   | 青稞32位RISC-V内核，RV32EC指令集 @48MHz |
| SRAM       | 2 KB       | 易失数据存储区                          |
| Bootloader | 1920 Bytes | 系统引导程序存储区                      |
| Flash      | 16 KB      | 程序存储区                              |
| USB_DP(D+) | PD3        |                                         |
| USB_DM(D-) | PD4        |                                         |
| DPU        | PD5        | 用于重新触发USB枚举                     |
| BOOT Btn   | PD6        | 用于在BootLoader阶段检测是否要烧录程序  |

The following content assumes you are using the embeddedboys CH32V003 USB Dev
Board together with a WCH-LinkE programmer.

## Pin allocation

What the firmware puts on which pin, in the default build (every module).  The
flat number is the one the GPIO module asks for: `port * 16 + pin`, so `PD2` is 50
and `PC0` is 32.  Pins 16..31 are reserved because there is no port behind them.

| Pin | Port | Used by | Notes |
| --- | ---- | ------- | ----- |
| 1   | PA1  | PWM channel 2, **ADC channel 1** | one pad, two modules: the ADC reads it without taking it from the timer |
| 2   | PA2  | free | a plain GPIO |
| 32  | PC0  | free (the board's LED) | |
| 33  | PC1  | I2C SDA | the AT24C256 sits on this bus |
| 34  | PC2  | I2C SCL | |
| 35  | PC3  | free | a plain GPIO |
| 36  | PC4  | **SPI chip select, ADC channel 2** | the kernel SPI driver drives it as `reserved=36`; the ADC only reads it |
| 37  | PC5  | SPI SCK | |
| 38  | PC6  | SPI MOSI | jumper to PC7 for the loopback test |
| 39  | PC7  | SPI MISO | |
| 48  | PD0  | UART TX | jumper to PD1 for the UART tests |
| 49  | PD1  | UART RX, **and SWIO** | the programmer owns this pin: see below |
| 50  | PD2  | PWM channel 1 | |
| 51  | PD3  | USB D+ | |
| 52  | PD4  | USB D- | |
| 53  | PD5  | USB pull-up (DPU) | the power module releases it for a standby sleep, which is what makes the host see a disconnect |
| 54  | PD6  | boot button | also the power module's wake source, on a falling edge |
| -   | PA0, PA3..PA15 | not connected | this package does not have them; a GPIO write reads back 0 |

The device reports this list itself: `GET_CAPABILITIES` (0x3d) carries a 64 bit
mask of the pins it owns, so a driver does not have to hardcode the table above.
Anything the *driver* owns (a chip select, an IRQ line) is added on the kernel
side with `reserved=<pin>[,<pin>]` on `usb-mfd.ko`.

### Things to know before wiring anything up

- **PD1 is the debug pin.**  It is the chip's SWIO line, the WCH-LinkE holds it,
  and it cannot be used as an input while the programmer is attached.  That is why
  the UART tests need a jumper between PD0 and PD1 - and why **that jumper
  conflicts with flashing**: while the UART is enabled, PD0 drives an idle high
  line through the jumper and holds SWIO, so `minichlink` fails
  (`nothing connected to linker`).  Disabling the port releases both pins, and a
  reset always works: `scripts/wdg_test.py --reset 400` or a power cycle.
- **Two pads are shared.**  PC4 is the SPI chip select and the ADC's channel 2;
  PA1 is PWM channel 2 and the ADC's channel 1.  The ADC configures no pin at all
  and simply reads the pad, so it never takes one from the module driving it - and
  the module that drives a pin re-asserts its mode (`pwm_apply()` does, so reading
  a pin's level through the GPIO module does not leave the channel silent).
- **The GPIO module has no "analog" mode.**  It offers a pull-up/pull-down input
  and a push-pull output, which is what a GPIO is asked for; it cannot select the
  chip's analog input mode, so an ADC channel reads its pad in whatever mode it is
  in - a driven pad reads its driven level, a floating one reads what it floats
  to, and both are what the internal reference and calibration channels are there
  to sanity check against.  (`reserved=<pin>` on the core is the opposite thing:
  it tells the kernel a pin belongs to a *driver*, so userspace cannot take it.)
- **The jumpers the tests need**: PC6<->PC7 for `spi_test.py --loopback`,
  PD0<->PD1 for `uart_test.py` and `tests/uart_tty_test.py`.  Everything else runs
  without any wiring (the ADC's internal reference and calibration voltage, the
  I2C EEPROM, the GPIO and PWM pins).
- **Nothing sleeps unless a host asks**, and a standby sleep also switches the
  debug interface off - the auto-wake timer is what brings the device back, which
  is why there is no way to arm a sleep without one.  See
  [notes/pwr.md](notes/pwr.md).

## Repository layout

| Path          | Contents                                                                  |
| ------------- | ------------------------------------------------------------------------- |
| `AGENTS.md`   | Rules for changing this repository (read before editing)                   |
| `notes/`      | Knowledge base: measured limits, protocol details, debugging case studies  |
| `hardware-docs/` | CH32V003 reference manual (`CH32V003RM.PDF`) and the WCH EVT package (`CH32V003EVT.ZIP`), both tracked; the bench device datasheets are not |
| `patches/`    | Patches for the vendored submodule (`ch32fun`), kept as diffs              |
| `vendor/`     | Main firmware: the USB device and nine modules (gpio, spi, i2c, wdg, pwm, adc, uart, pwr, frame) |
| `bootloader/` | Upstream USB HID bootloader (VID 1209, PID B003)                           |
| `rv003usb/`   | Vendored software USB stack (bit-banged low speed device)                  |
| `lib/`        | USB descriptor / type definitions, and the shared VID/PID (`v003_usb_ids.h`) |
| `kernel/`     | Linux driver: `usb-mfd.ko` (core) + `v003-gpio/i2c/spi/pwm/wdt/adc/uart.ko` (children) |
| `scripts/`    | pyusb host side protocol tests and helpers (one per module, plus a combined one) |
| `tests/`      | Userspace uAPI tests (GPIO character device, i2c-dev, IIO, TTY) and the rusb experiment |
| `tools/`      | Bench instruments: state dashboard, pin probe, jumper check, SWIO recovery, build / module / whole-suite runners (see [tools/README.md](tools/README.md)) |

### What the device offers

One CH32V003, one USB link, and nine modules that are compiled in and reported by
the device itself (`GET_CAPABILITIES`, 0x3d) - a host asks what is there instead
of assuming:

| Module | Host interface | Command range | Written up in |
| ------ | -------------- | ------------- | ------------- |
| gpio   | `gpiochip` with 56 lines | 0x06-0x0c | [notes/firmware.md](notes/firmware.md) |
| i2c    | `i2c_adapter` (bit banged) | 0x50-0x5f | [notes/i2c.md](notes/i2c.md) |
| spi    | `spi_controller` (hardware SPI1) | 0x40-0x47 | [notes/firmware.md](notes/firmware.md) |
| pwm    | `pwm_chip`, two TIM1 channels | 0x70-0x72 | [notes/firmware.md](notes/firmware.md) |
| wdt    | `watchdog_device` (the chip's IWDG) | 0x60-0x63 | [notes/firmware.md](notes/firmware.md) |
| adc    | IIO device, ten 10 bit channels | 0x80-0x85 | [notes/adc.md](notes/adc.md) |
| uart   | TTY at `/dev/ttyV0` | 0x90-0x9a | [notes/uart.md](notes/uart.md) |
| pwr    | protocol only (nothing standard fits) | 0xa0-0xa2 | [notes/pwr.md](notes/pwr.md) |
| frame  | the endpoint data path itself | - | [notes/frame-protocol.md](notes/frame-protocol.md) |

Three documents answer three different questions: this README how to build and
run, [TODO.md](TODO.md) what is done and what was verified on hardware, and
[notes/](notes/README.md) why the code looks the way it does.

The USB identity (vendor id, product id) lives in one place, `lib/v003_usb_ids.h`,
and the firmware, the kernel driver and the host scripts all take it from there.
Neither the device nor a local rule can change what `lsusb` prints in front of
the product name - see [notes/usb-identity.md](notes/usb-identity.md) for why,
and for how to register the id and get it into the upstream database.

## Getting Started

### 0. What you need

**Hardware.**  Two cables, and they do different things:

- the board's **own USB port to the PC** - that is the device under test, the one
  this driver binds to (the board's USB pins are PD3/PD4 and its pull-up control
  is PD5, see the [pin allocation](#pin-allocation));
- the **WCH-LinkE to the board's programming pins**: `SWIO` (that is PD1), `3V3`
  and `GND`.  This one is only for flashing and for the `minichlink -T` log, and
  both grounds meet through the PC.

Only the tests that say so need wiring beyond that: **PC6<->PC7** for the SPI
loopback and **PD0<->PD1** for the UART loopback (that one shares the debug pin -
read [Things to know before wiring anything up](#things-to-know-before-wiring-anything-up)
before using it while flashing).  An AT24C256 on the I2C pins (PC1/PC2) is what
the I2C and EEPROM tests talk to; the rest runs without any external parts (the
ADC checks itself against its own internal reference and calibration channels, and
the GPIO test uses PC0, the board's LED).

**Software.**

```shell
git clone <this repository> && cd ch32v003-usb-drivers
git submodule update --init ch32fun     # build rules, minichlink, rv003usb's home

# 1. a bare metal RISC-V toolchain (the build finds either prefix)
sudo pacman -S riscv64-unknown-elf-gcc          # Arch / CachyOS
sudo apt install gcc-riscv64-unknown-elf        # Debian / Ubuntu
# xPack works too, and then it has to be on PATH:
#   export PATH=$HOME/.local/opt/xpack-riscv-none-elf-gcc-*/bin:$PATH

# 2. build tools for the programmer, and for the kernel modules
sudo pacman -S base-devel python libusb clang lld llvm linux-headers  # Arch / CachyOS
sudo apt install build-essential python3 python3-venv libusb-1.0-0-dev \
                 clang lld linux-headers-$(uname -r)              # Debian / Ubuntu

# 3. the host side tests: a virtualenv with pyusb, nothing else
python3 -m venv .venv && .venv/bin/pip install pyusb
```

The kernel module build takes its headers from
`/lib/modules/$(uname -r)/build` (see `kernel/Makefile` if your kernel tree lives
somewhere else) and passes `LLVM=1` by itself on a clang built kernel, which is
why `clang`/`lld` are in the list.

**Permissions.**  Nothing needs root except `insmod` and the rules below: three
device nodes want a udev rule or a group, so install the ones you need and re-plug
the device.

```shell
# the WCH-LinkE itself (this file ships with the submodule)
sudo cp ch32fun/minichlink/99-minichlink.rules /etc/udev/rules.d/

# /dev/i2c-N for the I2C tests
echo 'SUBSYSTEM=="i2c-dev", GROUP="plugdev", MODE="0660"' | sudo tee /etc/udev/rules.d/60-i2c-dev-plugdev.rules

# /dev/ttyV0 for the TTY test
echo 'SUBSYSTEM=="tty", KERNEL=="ttyV*", GROUP="plugdev", MODE="0660"' | sudo tee /etc/udev/rules.d/61-tty-v003-plugdev.rules

sudo udevadm control --reload-rules && sudo udevadm trigger
```

`/dev/gpiochipN` is already in `plugdev`, so the GPIO tests only need your user to
be in that group (`groups | grep plugdev`).  The pyusb tests need the *kernel
modules unloaded* - the interface cannot be claimed twice - so the two test suites
are run one after the other, never at the same time.

**Then check it all works** (three commands, and what they should say):

```shell
make -C ch32fun/minichlink               # builds the programmer and the flasher
make -C vendor                           # builds the firmware, then flashes it
.venv/bin/python scripts/v003_test.py    # "ALL TESTS PASSED"
```

Flashing reboots the chip, so give the device a second to re-enumerate before the
test - `FAIL: device not found` right after a build is usually just that.

If `make` cannot talk to the chip, that is almost always SWIO: see
[Things to know before wiring anything up](#things-to-know-before-wiring-anything-up)
- a held PD1 (the UART's receive pin is that same debug pin, and a jumper to PD0
holds it) makes minichlink report `nothing connected to linker`.

### 1. Toolchain

A bare metal RISC-V GCC is required; the build detects `riscv64-unknown-elf-gcc`
or `riscv-none-elf-gcc` automatically and can be overridden with
`make PREFIX=...`.  Built and verified with xPack GCC 15.2.0-1 here.

`minichlink` needs libusb headers (`libusb-1.0` / `libusb-1.0-0-dev`), and the
`ch32fun` submodule holds both the build rules and `minichlink` itself.

### 2. Build the firmware

The firmware is assembled from modules; the default is everything that exists
today, and what is compiled in is reported by the device (see
`GET_CAPABILITIES` under *Device*), so a host never probes a module that is not
there:

```shell
cd vendor
make                              # every module:             12556 B flash, 1284 B RAM
make MODULES=gpio,spi,i2c,wdg,pwm,adc  # without UART:       10084 B, 1112 B
make MODULES=gpio,spi,i2c,wdg,pwm,adc,uart  # without PWR:  11376 B, 1240 B
make MODULES=gpio                 # GPIO only:                 5624 B,  900 B
make TRACE=1                      # with the I2C tracer:      12788 B, 1424 B (a debug facility)
make UEVENTS=4                    # a deeper debug ring (2 entries by default)
```

`gpio spi i2c wdg pwm adc uart pwr` are the modules that exist - every name the
capability report has - and the device reports which of them it was built with,
so a host never probes a module that is not there.

```shell
cd vendor
make build           # produce vendor.bin / vendor.elf
```

`make` also builds and flashes in one step (see below).

`tools/build.sh` does the same and then reports the two budgets this chip runs
into: `vendor.bin` against the 16 kB of flash, and `_ebss` against the 0x20000800
stack top.  The stack grows *down* into the statics, so an overrun corrupts
variables instead of faulting - the number to watch is the one `GET_STACK_FREE`
(0x3b, via `tools/status.py`) measures on hardware, not the optimistic fresh-boot
value.

### 3. Build the programmer and flash

```shell
make -C ch32fun/minichlink
minichlink -w vendor/vendor.bin flash -b     # write and reboot into the firmware
minichlink -T                                # firmware printf() log over SWIO
```

The chip requires one initial programming with a WCH-LinkE; afterwards the
firmware can also be updated through the USB bootloader (see `bootloader/`).

### 4. Kernel driver

The driver follows `drivers/mfd/dln2.c`: `usb-mfd.ko` is only the USB transport
and MFD core (it exports `v003_transfer_out()` / `v003_transfer_in()` and the
transport aware `v003_cmd_out()` / `v003_cmd_in()` and registers the MFD cells),
and every function of the device is a child platform driver: `v003-gpio.ko`
(gpiochip), `v003-i2c.ko` (`i2c_adapter`), `v003-spi.ko` (`spi_controller`),
`v003-pwm.ko` (`pwm_chip`), `v003-wdt.ko` (`watchdog_device`), `v003-adc.ko` (an
IIO device) and `v003-uart.ko` (a TTY).  The child modules use symbols exported by
the core, so they have to be loaded after it; a module the firmware was not built
with gets no cell and its driver simply has nothing to probe.

```shell
cd kernel
make                       # LLVM=1 is passed automatically on clang built kernels

sudo insmod usb-mfd.ko     # reads the firmware version and capability report,
                           # adds a child cell per module the device reports
sudo insmod usb-mfd.ko reserved=36   # keep the SPI chip select line out of the gpiochip
sudo insmod v003-gpio.ko   # gpiochip with 56 lines, USB pins reserved
sudo insmod v003-i2c.ko    # i2c_adapter, PC1 = SDA, PC2 = SCL
sudo insmod v003-spi.ko    # spi_controller, PC5 = SCK, PC6 = MOSI, PC7 = MISO
sudo insmod v003-wdt.ko    # the chip's IWDG as a watchdog_device
sudo insmod v003-pwm.ko    # TIM1 channels 1 and 2 as a pwmchip
sudo modprobe industrialio # the IIO core, needed by the next one
sudo insmod v003-adc.ko    # ten IIO voltage channels (8 = Vref, 9 = Vcal);
                           # `insmod v003-adc.ko selftest=1` logs both in mV
sudo insmod v003-uart.ko   # /dev/ttyV0, a TTY on PD0 (TX) / PD1 (RX)
./../tests/gpio_chardev.py # GPIO v2 character device test (no libgpiod needed)

sudo rmmod v003-uart v003-adc v003-pwm v003-wdt v003-spi v003-i2c v003-gpio usb-mfd
```

`tests/gpio_chardev.py` drives the chip through the GPIO v2 uAPI with plain
ioctls, so it needs neither libgpiod nor the deprecated sysfs interface, and it
checks that the reserved lines (D+/D-/DPU and the boot button) cannot be
requested.

Both modules take the `transport` parameter of the core into account: `0`
(default) carries commands as control transfers, `1` uses the framed endpoint
protocol, `2` sends writes as frames and reads over the control path.  Measured
through the character device, control wins every single operation (set 2.00 ms,
get 3.00 ms) against the endpoint path (set 3.93 ms, get 6.82 ms), because a low
speed control transfer finishes a small command in three 1 ms frames while the
endpoint path needs at least four - the reason is in [Data path
performance](#data-path-performance).

#### SPI controller

```shell
sudo insmod v003-spi.ko                       # spi0, no chip select driven
sudo insmod v003-spi.ko cs_pin=36             # drive PC4 as chip select (active low)
sudo insmod v003-spi.ko selftest=100          # loop 100 bytes back at probe
sudo insmod v003-spi.ko selftest=16 selftest_speed=24000000   # ... at full clock
```

Chip select is driven through the GPIO module rather than by the firmware,
because a Linux message - not a transfer - is what sits between two CS edges:
`spi_write_then_read()` and friends put several transfers inside one CS
assertion, and the firmware only drives its pin around a single transfer (the
module comment says as much).  `cs_pin` claims that pin at probe, so keep it out
of the gpiochip users.

`selftest=<bytes>` clocks a pattern through with MOSI jumped to MISO and
compares what comes back, which the device test board allows with a PC6<->PC7
jumper.  It runs through the SPI core (`spi_new_device()` + `spi_sync()`), so it
exercises the same path a client driver would.  Verified: 16 B at 24 MHz, 65 B
and 100 B at 1 MHz/4 MHz (transfers longer than the firmware's 64 byte limit are
split into chunks, the 65 byte case covers a chunk of exactly one byte), and
128 B with the chip select pin disabled.  A plain wire cannot verify bit order -
every bit returns on the clock edge it left on - so MSB-first stays unproven.

#### I2C adapter

```shell
sudo insmod v003-i2c.ko                                  # bus appears as i2c-N
sudo insmod v003-i2c.ko selftest=0x50                    # read 8 bytes at word address 0 and log them
sudo insmod v003-i2c.ko selftest=0x50 selftest_len=16    # ... 16 bytes
sudo insmod v003-i2c.ko half_period=25                   # ~140 kHz instead of ~280 kHz
```

Two of the userspace tests need a udev rule before they run as an ordinary user -
`/dev/i2c-N` is `root:i2c` and `/dev/ttyV0` is `root:uucp` - and both rules are in
[step 0](#0-what-you-need) above.

```shell
.venv/bin/python tests/i2c_dev_test.py        # through the kernel I2C stack
```

`tests/i2c_dev_test.py` needs nothing but ctypes: it finds the adapter by name,
checks the advertised functionality, probes an empty address (has to report
`ENXIO`), writes a marker and a 16 byte page to the AT24C256 and reads them
back, tries an unaligned 4 byte write, checks that 10 bit addressing is refused
and puts the page it used back the way it found it.

Without that rule (or as a no-privilege smoke test) the driver can prove the
same path at probe time and log it: `selftest=<7 bit address>` performs the two
message write+read transfer (repeated START) the `at24` driver uses.

```
v003-i2c v003-i2c: self test 0x50[0x00]: e0 e0 e0 e0 e0 e0 e0 e0 a1 09 0a 0b 0c 0d 0e 0f
v003-i2c v003-i2c: self test read of 0x00 at 0x51 failed: -6      # nothing at 0x51: -ENXIO
```

#### Why an I2C result needs a completion tag

A control transfer that carries a data stage is answered by the USB interrupt
*before* the firmware's main loop executes it, so reading the status right after
sending a request can return the previous request's status - measured:
`status 0x2` (address NACK, left over from the request before) against `0x1201`
(OK, 18 bytes) on a re-read.  Two requests that fail the same way are
indistinguishable by re-reading alone, so `V003_I2C_GET_RESULT` (0x58) returns
the status *plus* the main loop's request counter in its top byte, and the
driver polls until that tag changes.  In the normal case the firmware has long
finished and it costs nothing; when it has not, one extra control transfer fixes
it.  The framed endpoint protocol gets the same guarantee for free, because the
main loop builds the response after doing the work, which is where I2C and SPI
payload commands belong long term.

### 5. Userspace tests

```shell
python3 -m venv .venv && .venv/bin/pip install pyusb

# protocol tests: one per module, over raw USB (the kernel modules must be
# unloaded: the interface cannot be claimed twice)
.venv/bin/python scripts/v003_test.py    # commands, EP data path, GPIO, capabilities
.venv/bin/python scripts/i2c_test.py     # I2C over the AT24C256
.venv/bin/python scripts/eeprom_test.py  # page writes and reads
.venv/bin/python scripts/spi_test.py    # SPI bridge
.venv/bin/python scripts/spi_test.py --loopback --soak 150   # with PC6<->PC7 jumper
.venv/bin/python scripts/adc_test.py     # ADC: internal reference and calibration voltage
.venv/bin/python scripts/pwm_test.py     # PWM: reported period and both duty extremes
.venv/bin/python scripts/uart_test.py    # UART: needs a jumper between PD0 and PD1
.venv/bin/python scripts/frame_test.py   # framed endpoint protocol
.venv/bin/python scripts/wdg_test.py --reset 2000   # let the watchdog bite
.venv/bin/python scripts/pwr_test.py     # sleep, standby and the wake reasons
.venv/bin/python scripts/pwr_test.py --wdg --uart   # the watchdog refusal, UART wake
.venv/bin/python scripts/combo_test.py   # every module in one session, before and after a sleep
.venv/bin/python scripts/stress_test.py --mode mixed --iterations 300

# uAPI tests: the kernel modules have to be loaded for these
.venv/bin/python tests/gpio_chardev.py   # GPIO character device
.venv/bin/python tests/i2c_dev_test.py   # i2c-dev, the adapter and its SMBus set
.venv/bin/python tests/adc_iio_test.py   # ADC through IIO: /sys/bus/iio/devices
.venv/bin/python tests/uart_tty_test.py  # UART through /dev/ttyV0 (PD0<->PD1 jumper)
```

`tools/run_tests.sh` runs all of it in the order it has to run in (the driver
side with the modules loaded, the protocol side with them unloaded), prints one
pass/fail table, keeps every log and fails the run when dmesg has warnings in it;
[tools/README.md](tools/README.md) describes the instruments next to it.

`scripts/i2c_trace.py` decodes the firmware's I2C clock trace and needs a firmware
built with `make TRACE=1`: the tracer costs 136 bytes of RAM and 224 of flash, so
it is off by default and the script skips with that explanation when it is not
there.  `tests/gpio_sysfs.py` blinks a line through the *legacy* sysfs GPIO
interface, which current kernels no longer compile in (this bench has
`CONFIG_GPIO_CDEV` only), and it skips for the same kind of reason.

`spi_test.py --loopback` requires a jumper between PC6 (MOSI) and PC7 (MISO);
with it, 150 random transfers over each of the two SPI paths pass with zero
mismatches (~105 round trips/s on the endpoint path, ~83 transfers/s on the
control path). A plain wire cannot verify the bit order - every bit returns on
the clock edge it left on - so MSB-first/CPOL/CPHA still follow `SPI_CTLR1`
without independent proof.

`uart_test.py` requires a jumper between **PD0 (TX) and PD1 (RX)**, because PD1
is the chip's SWIO debug pin: the programmer holds it, so with the WCH-Link
attached the receive pin cannot be used on its own. With the jumper in place the
transmit path is the receiver's peer, and 32 byte patterns round trip byte for
byte at 9600, 115200, 921600 and 3000000 baud (measured baud error 0 % to
0.16 %, interrupts exactly one per byte, no framing or overrun errors).

`combo_test.py` is the one that asks whether the modules work *together*: one
working round trip per module in a single session, then the same again after a
standby sleep, plus the two pads an ADC channel shares with another module (PC4
with the SPI chip select, PA1 with PWM channel 2 - the ADC reads them without
taking them away from what drives them).

`pwr_test.py` arms sleeps the firmware would otherwise never take: nothing sleeps
unless a host asks, every sleep has an auto-wake timer as a backstop, and a sleep
longer than the watchdog timeout is refused. A **standby** sleep releases the USB
pull-up first, so the device leaves the bus and comes back with the same serial
number; a **sleep** stays attached but silent for its duration (the host's own
traffic ends it early). While the device is in standby the debug interface is off
as well, so the programmer cannot reach the chip either - the auto-wake timer is
what brings it back, which is why there is no way to arm a sleep without one.

**The jumper and flashing do not get along**: while the UART is enabled, PD0 is
an output driving an idle high line and through the jumper it holds SWIO, so
`minichlink` fails (`nothing connected to linker`, or `HARTINFO: ffffffff /
Could not setup interface` after host side pin experiments).  Disabling the port
makes the module release both pins, and `tools/free_swio.py` does exactly that
over USB and then reads the pad mode back to prove it:

```shell
# measured: with the port enabled        -> link error, nothing connected to linker
#            after tools/free_swio.py     -> the same make reports "Image written."
.venv/bin/python tools/free_swio.py
make -C vendor
```

If that is still not enough, what always works is a **reset**:
`scripts/wdg_test.py --reset 400` arms the watchdog over USB and lets it bite, and
the watchdog is in the default build. Removing the jumper works too.
`uart_test.py` disables the port in a `finally:` so a failing run does not make
things worse.

### 6. Debugging with GDB

`minichlink -G` also serves a GDB stub on port 3333, so the firmware can be
inspected and flashed with the toolchain's `riscv-none-elf-gdb`:

```shell
# terminal + gdb stub (it owns the WCH-Link until it is stopped)
./ch32fun/minichlink/minichlink -G &

riscv-none-elf-gdb vendor/vendor.elf \
    -ex "target remote :3333" \
    -ex "break handle_gpio_out_request" -ex "continue"

# write the firmware through GDB instead of minichlink -w
riscv-none-elf-gdb -q -batch vendor/vendor.elf \
    -ex "target remote :3333" -ex "load" -ex "detach" -ex "quit"
```

Registers, memory and firmware variables are readable (e.g.
`p/x ep3_tx_head`), the CPU can be halted, stepped and continued, and the
firmware `printf()` log keeps streaming in the stub's terminal. Stop the stub
before running other `minichlink` commands - it holds the programmer.

GDB `load` requires `patches/ch32fun-gdb-flash-memory-map.patch`: the stub
declared the flash region as `iss->flash_size` (kB) where the memory map wants
bytes, so GDB saw 16 bytes of flash and refused to use flash writes. Apply it
with:

```shell
git -C ch32fun apply ../patches/ch32fun-gdb-flash-memory-map.patch
make -C ch32fun/minichlink
```

## Device

The firmware enumerates as a vendor specific device (VID:PID `1209:c303`,
class `0xFF`) with one interface and three interrupt endpoints:

| Endpoint  | Default behaviour                              | With SPI enabled        |
| --------- | ---------------------------------------------- | ----------------------- |
| EP1 OUT   | payload is echoed to the EP3 IN FIFO           | echoed                  |
| EP2 OUT   | payload is echoed to the EP3 IN FIFO           | clocked out on MOSI     |
| EP3 IN    | echoes the EP1/EP2 OUT FIFO (8 bytes / packet) | MISO bytes              |

The serial number string is not a placeholder: the firmware reads the factory
ESIG unique id (96 bits, chapter 15 of the reference manual; this part leaves the
third word blank) at boot and reports it as 24 hex characters, so every board
identifies itself.  `GET_DEVICE_UID` (0x3c) hands the same 12 bytes to a host.

Firmware requests are vendor control transfers: `bmRequestType` `0x40` (OUT) or
`0xC0` (IN), `bRequest` `0`, `wValue` carries the payload and `wIndex` carries
`cmd | (module << 8)`.

| `wIndex` | Module  | Request             | Payload / result                                            |
| -------- | ------- | ------------------- | ----------------------------------------------------------- |
| `0x30`   | generic | `GET_DEVICE_VER`    | IN, `0x1010`                                                |
| `0x31`   | generic | `GET_DEVICE_SN`     | IN, first word of the factory unique id                     |
| `0x3c`   | generic | `GET_DEVICE_UID`    | IN, the 12 byte ESIG unique id (also the USB serial number)  |
| `0x3d`   | generic | `GET_CAPABILITIES`  | IN, 16 bytes: which modules this build has, channel counts, owned pins |
| `0x70`   | pwm     | `PWM_SET`           | OUT data stage: `{channel, enable, duty permille, period ns}` |
| `0x71`   | pwm     | `PWM_GET`           | IN data stage, `wValue` = channel: what the timer really does      |
| `0x72`   | pwm     | `PWM_GET_INFO`      | IN, number of channels                                       |
| `0xa0`   | pwr     | `PWR_ARM`           | OUT data stage `{duration_ms, mode, wake, detach, delay_ms}`: sleep (10 ms..30 s), standby, optional release of the USB pull-up. Refused as a whole if out of range or longer than the watchdog timeout |
| `0xa1`   | pwr     | `PWR_GET_STATE`     | IN data stage, 28 bytes: sleep count, AWU ticks and the nominal ms, capabilities, the last refusal, mode, wake reason and divider |
| `0xa2`   | pwr     | `PWR_GET_INFO`      | IN, `(max ms << 16) \| min ms` = `(30000 << 16) \| 10`  |
| `0x90`   | uart    | `UART_CONFIG`       | OUT data stage `{baud, data_bits, parity, stop_bits, enable}`: refused as a whole if a field is out of range |
| `0x91`   | uart    | `UART_GET_CFG`      | IN data stage, the same struct with `actual_baud` filled in (what BRR really divides to) |
| `0x92`   | uart    | `UART_WRITE`        | OUT data stage: queue bytes for the transmitter |
| `0x93`   | uart    | `UART_READ`         | IN data stage, `wValue` = max bytes: what the receiver holds |
| `0x94`   | uart    | `UART_GET_STATE`    | IN, `(tx_queued << 24) \| (rx_available << 16) \| (tx_dropped << 8) \| rx_dropped` |
| `0x95`   | uart    | `UART_GET_COUNTS`   | IN, `(tx_bytes << 16) \| rx_bytes` (16 bit, wrapping) |
| `0x96`   | uart    | `UART_GET_ERRORS`   | IN, `(isr_entries << 24) \| (overrun << 16) \| (framing << 8) \| parity` |
| `0x97`   | uart    | `UART_FLUSH`        | OUT: drop what the receiver holds (counted as dropped) |
| `0x99`   | uart    | `UART_GET_INFO`     | IN, `(receive ring size << 16) \| ports` |
| `0x9A`   | uart    | `UART_CLEAR_STATS`  | OUT: zero the drop and error counters (they are 8 bit and would otherwise carry a previous session) |
| `0x80`   | adc     | `ADC_START`         | OUT, `wValue` = channel: convert it in `main()` (42 us, so never in the interrupt) |
| `0x81`   | adc     | `ADC_GET`           | IN, `(tag << 16) \| (valid << 15) \| value` - 10 bit, so 0..1023       |
| `0x82`   | adc     | `ADC_GET_INFO`      | IN, `(resolution << 16) \| channels` = `0x0a000a`                      |
| `0x83`   | adc     | `ADC_GET_SEQ`       | IN, conversions the main loop has run                        |
| `0x84`   | adc     | `ADC_GET_STATUS`    | IN, `(error << 24) \| (last conversion us << 16) \| (requests << 8) \| conversions` |
| `0x85`   | adc     | `ADC_SET_CALVOL`    | OUT, `wValue` 0/1 = calibration voltage 2/4 or 3/4 AVDD (channel 9 follows it) |
| `0x60`   | wdg     | `WDG_START`         | OUT, `wValue` = timeout in ms (starts the chip's IWDG, cannot be undone) |
| `0x61`   | wdg     | `WDG_FEED`          | OUT, `wValue` = `0xaaaa` (the reload key)                     |
| `0x62`   | wdg     | `WDG_GET_STATE`     | IN, `(actual timeout ms << 8) \| running`                     |
| `0x63`   | wdg     | `WDG_GET_RESET_CAUSE` | IN, why the chip last reset, and clears it                  |
| `0x32`   | generic | `GET_EP_STATS`      | IN, `wValue`: 0-2 RX bytes of EP0/1/2, 3 EP3 IN packets, 4 EP3 IN bytes. OUT with `wValue == 0xff` resets the counters |
| `0x33`   | generic | `GET_FIFO_LEVEL`    | IN, bytes queued in the EP3 IN FIFO                         |
| `0x34`   | generic | `GET_FIFO_DROPS`    | IN, bytes dropped because the FIFO was full                 |
| `0x36`   | generic | `GET_CTRL_OUT_DATA` | IN, the data stage of the last vendor control-OUT (≤ 64 bytes), clamped to what was received |
| `0x37`   | generic | `GET_CTRL_OUT_SEQ`  | IN, number of vendor OUT requests the firmware main loop has finished. SPI/I2C transfers run outside the USB interrupt, so a host has to wait for this to advance before reading their result |
| `0x06`   | gpio    | `SET`               | OUT, `wValue = (pin << 8) \| value`                         |
| `0x07`   | gpio    | `GET`               | IN, current pin level                                       |
| `0x08`   | gpio    | `REQUEST`           | OUT, claims the pin and puts it into input mode (pull-up/down) |
| `0x09`   | gpio    | `FREE`              | OUT, releases the pin                                       |
| `0x0A`   | gpio    | `GET_DIRECTION`     | IN, 1 = input, 0 = output                                   |
| `0x0B`   | gpio    | `DIRECTION_INPUT`   | OUT                                                         |
| `0x0C`   | gpio    | `DIRECTION_OUTPUT`  | OUT, `wValue = (pin << 8) \| value`                         |
| `0x40`   | spi     | `ENABLE`            | OUT, `wValue` 0/1: route EP2 OUT to MOSI, MISO to EP3 IN     |
| `0x41`   | spi     | `CONFIG`            | OUT, `wValue = (prescaler << 8) \| mode`, mode bits: 1 CPHA, 2 CPOL, prescaler = `SPI_CTLR1.BR` (0 = /2 … 7 = /256) |
| `0x42`   | spi     | `GET_STATE`         | IN, `(prescaler << 16) \| (mode << 8) \| enabled`            |
| `0x43`   | spi     | `GET_STATS`         | IN, bytes clocked out on MOSI                               |
| `0x44`   | spi     | `SET_CS`            | OUT, `wValue` = pin index, or `0xffff` for none: the pin is driven low for a `TRANSFER` |
| `0x45`   | spi     | `TRANSFER`          | OUT with a data stage (≤ 64 bytes): clock the payload out on MOSI, chip select framed, keep the MISO bytes for `GET_RX` |
| `0x46`   | spi     | `GET_RX`            | IN, the MISO bytes of the last `TRANSFER`, clamped to its length |
| `0x47`   | spi     | `GET_CS`            | IN, the configured chip select pin or `0xffff`              |
| `0x50`   | i2c     | `CONFIG`            | OUT, `wValue` = half period in **100 ns units** (0 = keep, default 12 -> ~280 kHz measured; 25/50 for poorer wiring): PC1/PC2 as open drain, resets the bus |
| `0x51`   | i2c     | `WRITE`             | OUT with a data stage `[addr << 1][bytes...]`, result in `GET_STATUS` |
| `0x52`   | i2c     | `READ`              | OUT with a data stage `[addr << 1 \| 1][count]`, bytes in `GET_RX` |
| `0x53`   | i2c     | `GET_RX`            | IN, the bytes read by the last `READ`, clamped to its length |
| `0x54`   | i2c     | `GET_STATUS`        | IN, `ok`/`ADDR_NACK`/`DATA_NACK`/`stretch`, bits 8-15 = bytes moved, 16-23 = failing byte |
| `0x55`   | i2c     | `SCAN`              | OUT, `wValue` = first 7 bit address: probes 32 addresses       |
| `0x56`   | i2c     | `GET_SCAN`          | IN, ACK bitmap of that scan, bit 0 = the first probed address  |
| `0x57`   | i2c     | `WRITE_READ`        | OUT with a data stage `[addr << 1 \| 0][bytes to write...]` and `wValue` = bytes to read: START, write, **repeated START** (no STOP), read into `GET_RX`. The idiom register based devices need |
| `0x58`   | i2c     | `GET_CFG`           | IN, half period in microseconds                             |
| `0x5a`   | i2c     | `TRACE`             | OUT, `wValue` != 0 arms the SDA/SCL clock tracer, 0 stops it |
| `0x5b`   | i2c     | `GET_TRACE`         | IN, `wValue` = byte offset: raw trace entries (delta, SDA level) |
| `0x5c`   | i2c     | `GET_TRACE_INFO`    | IN, `count \| (overflow << 8) \| (trace unit in ticks << 16)` |
| `0x5d`   | i2c     | `WAIT_READY`        | OUT with a data stage `[addr << 1]`, `wValue` = timeout in ms: ACK poll the device until it answers (its write cycle finished) |
| `0x5e`   | i2c     | `GET_WAIT_US`       | IN, duration of the last `WAIT_READY` in microseconds        |
| `0x5f`   | i2c     | `MEM_WRITE_READ`    | OUT with a data stage `[addr << 1][word address][data...]` and `wValue` = (read count) \| (1 byte word address << 8) \| (ACK poll << 9): page write, optional ACK poll, then a Random Read of the same word address into `GET_RX` - the whole memory cycle in one request |
GPIO pins use the ch32fun numbering: `PA1 = 1`, `PA2 = 2`, `PC0..PC7 = 32..39`,
`PD0..PD7 = 48..55`. `PD3`/`PD4`/`PD5` are the USB pins and `PD6` is the boot
button, so they are not usable as GPIO.

SPI1 uses the fixed alternate function pins `SCK = PC5`, `MOSI = PC6`,
`MISO = PC7`. Chip select is either driven by the firmware around
`V003_SPI_TRANSFER` (configured with `V003_SPI_SET_CS`, must not be one of the
SPI pins) or toggled by the host through the GPIO module.

I2C is a **software master** on `SDA = PC1` / `SCL = PC2` (open drain, external
pull-ups required - most breakout modules bring their own). The firmware drives
START/STOP itself. The alternative mappings (`PD1`/`PD0`, or `PC5`/`PC6` with
`I2C1_RM = 1x`) are not used because `PC5`/`PC6` are the SPI pins. A software
master was chosen over the CH32V003 I2C peripheral deliberately: it supports
clock stretching, reports exactly which byte was not acknowledged, and recovers
the bus (nine clocks + STOP) when a slave is left mid byte. Because the pins
rise slowly through the pull-ups, the bit timing waits for a released line to
actually be high, and `CONFIG` burns one throwaway probe on address 0x08 to
absorb the glitch of reconfiguring the pins (that glitch used to make the very
first transaction afterwards misread its ACK slot).

Both the I2C transactions and the SPI control path run in the firmware main
loop, not in the USB interrupt, so the host has to wait for
`GET_CTRL_OUT_SEQ` to advance before reading their result - otherwise it can
still see the previous one.  Wait until the counter has *settled* before taking
the baseline, not merely until it changes: a request that has not been picked up
yet makes the next wait succeed immediately.

### I2C notes for memory devices (verified against the AT24C256 datasheet)

- `WRITE_READ` is the datasheet's *Random Read*: a dummy write of the two word
  address bytes followed by a repeated START and the device address with R/W=1.
  Two byte addressing, most significant byte first; `READ` alone is a *Current
  Address Read* from the device's internal counter.
- Page writes are 64 bytes and **roll over inside the same page**: the low six
  address bits increment, so a write that crosses a page boundary silently
  overwrites the start of that page.  Split long writes at page boundaries.
- tWR is 5 ms max and the device does not answer during it.  Use `WAIT_READY`
  to ACK poll instead of sleeping: the module on this bench finishes a write
  cycle in **326 us** (measured, min = median = max over 30 writes), so a fixed
  5 ms wait wastes most of it.
- The **link**, not the EEPROM, sets the throughput: one control transfer costs
  ~3 ms on this low speed link.  A write + ACK poll + read round needs about 14
  of them (~42 ms with tight polling), which `MEM_WRITE_READ` reduces to about
  six - measured **18 ms, a 2.3x win**.  Making the I2C clock faster does not
  help such small operations at all; fewer control transfers does.
- A full 64 byte page commits in ~2.9 ms (ACK polled) against ~0.33 ms for a
  four byte write, so page sized traffic benefits from the poll far more than
  small writes do.
- It does help *inside* one request though: 64 bytes cost ~11.5 ms of bit
  banging at 50 kHz, which is why the datasheet's 400 kHz (or 1 MHz) matters for
  page sized transfers.
- fSCL may go to 400 kHz at 1.7 V and 1 MHz from 2.5 V up.  The software master
  defaults to a 1.2 us half period, which measures ~280 kHz on the wire (a bit
  costs about three half periods).  That is inside the device rating and was
  verified with a **300 round random page safe soak with zero failures**;
  halving the clock makes it slower without buying reliability.

Two transfer paths exist:

- `V003_SPI_TRANSFER` (control path): one atomic, chip select framed transfer of
  up to 64 bytes for the price of one control-OUT plus one control-IN. Prefer
  this for register style transfers.
- EP2 OUT / EP3 IN streaming: every byte written to EP2 OUT is clocked out
  immediately and the sampled MISO byte is queued to the EP3 IN FIFO. Only
  worth it for larger transfers, because the interrupt endpoints are polled
  once per 1 ms frame.

## Data path performance

Measured on hardware from the host side, 64 bytes in each direction per round
trip (see the informational output of `scripts/v003_test.py`):

| Transport                                            | Round trip | Throughput |
| ---------------------------------------------------- | ---------- | ---------- |
| control-OUT data stage + control-IN (EP0, 64 B each) | 6.0 ms     | 20.8 KiB/s |
| EP1/EP2 OUT + EP3 IN (interrupt, 8 B packets)        | 32.3 ms    | 3.9 KiB/s  |

The interrupt endpoints are polled once per 1 ms low speed frame, so moving 64
bytes needs 8 frames in each direction, while a single control transfer carries
a 64 byte data stage. For small SPI/I2C style transfers the control path wins;
the endpoint path only pays off with larger, pipelined transfers (a kernel
driver submitting multiple URBs) or when the firmware has work to do between
packets.

### Framed endpoint protocol

`vendor/frame.{h,c}` adds a command protocol on the endpoint path, modelled on
`drivers/mfd/dln2.c`:

```
request   [size u16][id u16][echo u16][handle u16][arg/payload...]   EP2 OUT
response  [size u16][id u16][echo u16][handle u16][result u16][...]  EP3 IN
```

`handle` is the module (0 generic, 1 GPIO, ...), `id` the command byte, `echo` a
host tag that comes back in the response (`0` means "no answer wanted") and
`size` the total length, which is what makes a stream of frames self delimiting.
A bad length drops the rest of the packet instead of desynchronising the parser.
`V003_SET_FRAME_MODE` (0x39, a zero length control OUT) switches EP2/EP3 between
the frame stream and the raw echo stream, and `V003_GET_FRAME_STATS` (0x3a)
reports `frames handled << 16 | frames dropped`.  The interrupt only accumulates
bytes and drains the response FIFO; commands run in the main loop.
`scripts/frame_test.py` covers all of it (26 checks).

Measured, the framed path is *not* faster for single small commands:

| Operation                                            | Control (EP0)    | Framed (EP2/EP3) |
| ---------------------------------------------------- | ---------------- | ---------------- |
| gpio set (kernel driver, through `/dev/gpiochipN`)   | 2.00 ms (500/s)  | 3.93 ms (254/s)  |
| gpio get                                             | 3.00 ms (333/s)  | 6.82 ms (147/s)  |
| sequential request/response (pyusb)                  | 2.98 ms          | 7.77 ms          |

A low speed control transfer finishes a small request in three 1 ms frames
(SETUP, data, status).  The endpoint path needs at least four: a two packet
request, then the host must poll EP3 for a response that cannot be in the same
frame - and rv003usb answers *every* IN token, with a zero length packet while
it has nothing queued (it has no NAK support), so the first poll is wasted.  The
framed path is the right tool for fire-and-forget bursts, batched commands and
payloads too big for one control data stage, which is why the control path is
still the default (`transport=0`).

### Two hard firmware budgets

Both were found the hard way and constrain everything the firmware can do:

- **~4.5 us inside the USB interrupt.**  A vendor request handler runs before
  rv003usb acknowledges the SETUP packet; 203 ticks of the 48 MHz SysTick work
  and 13 loop iterations (about 260 ticks) already fail, after which the host
  reports the transfer as EIO.  This is why control-OUT data stages, frames and
  the I2C bit banging all run from the main loop.  `V003_GET_TIMING` (0x3f) with
  `wValue` = loop iterations re-measures the budget on any build.
- **The stack has ~450 bytes of room, not 790.**  It starts at 0x20000800 and
  grows down into the statics, so an oversized buffer does not fault, it silently
  corrupts whatever sits at the end of `.bss` (now 0x20000504).  `V003_GET_STACK_FREE`
  (0x3b) reports what is left, by painting the free RAM at boot and scanning it
  from the main loop - the scan takes ~150 us, 30x the interrupt budget, so it must
  never run in the interrupt.  Note that it reports the *deepest* use the canary has
  seen, so a fresh boot is optimistic: measured 352 bytes right after a reset and
  **448 bytes once `scripts/combo_test.py` has exercised every module**, which is
  the number to plan against.  The debug facilities are what paid for the modules:
  the rv003usb UEvent ring is at `RV003USB_NUMUEVENTS` 2 (`make UEVENTS=4`), the
  I2C clock tracer is off (`make TRACE=1`), and printing the ring at all needs
  `make DEBUG_EVENTS=1` because the bit-banged output blocks the main loop, which
  then stops servicing the requests it owes the host.

## Develop

**The bench instruments live in [`tools/`](tools/README.md)**: `status.py` (one
command that reports what the device is doing), `pin_probe.py` (what is on a pad,
and which pin a button is on), `uart_jumper_check.py` (the jumper or the
firmware), `free_swio.py` (give the debug pin back to the programmer),
`build.sh` (build, flash, and where flash and RAM stand), `modules.sh` (load or
unload the kernel drivers in the order that works) and `run_tests.sh`.

`tools/run_tests.sh` runs both halves of the suite in the order they require -
the driver side with the modules loaded, the protocol side with them unloaded -
prints one pass/fail table, keeps every log, and fails the run when dmesg has
warnings in it:

```shell
tools/run_tests.sh            # 16 passed, 1 skipped, 0 failed on this bench
tools/run_tests.sh --flash    # build, flash, then test what was just flashed
tools/run_tests.sh --repeat 3 # the flake check
```

generate compile_commands.json and use clangd to index code

```bash
bear -- make
```

Go to vscode, Press `Ctrl + Shift + P` , the search `clangd: Restart language server` and press `Enter`.

## Reference

- [rv003usb](https://github.com/cnlohr/rv003usb)
- [ch32fun](https://github.com/cnlohr/ch32fun)
- [Programming with PyUSB 1.0](https://github.com/pyusb/pyusb/blob/master/docs/tutorial.rst)

## Links

- [TODO list](TODO.md)
