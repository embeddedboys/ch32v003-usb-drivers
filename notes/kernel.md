# The Linux side

```
usb-mfd.ko      USB transport + MFD core      (kernel/usb-mfd.c, usb-mfd.h)
├── v003-gpio.ko   gpiochip, 56 lines         (kernel/gpio.c)
├── v003-i2c.ko    i2c_adapter                (kernel/i2c.c)
└── v003-spi.ko    spi_controller             (kernel/spi.c)
```

The split follows `drivers/mfd/dln2.c` with `drivers/{gpio,gpio-dln2.c}`,
`drivers/i2c/busses/i2c-dln2.c` and `drivers/spi/spi-dln2.c`: the core owns the
USB device and exports transfer helpers, children are platform drivers
instantiated from `mfd_cell`s and know nothing about USB.

Child modules resolve symbols exported by the core, so **load order matters**:
`usb-mfd` first, then any child.

```shell
cd kernel && make                 # LLVM=1 is detected on clang built kernels
sudo insmod usb-mfd.ko [transport=0] [cs_pin=...]   # core
sudo insmod v003-gpio.ko
sudo insmod v003-i2c.ko  [selftest=0x50] [half_period=12]
sudo insmod v003-spi.ko  [cs_pin=36] [selftest=100]
```

## Two transports

| `transport` | Behaviour                                                            |
| ----------- | -------------------------------------------------------------------- |
| 0 (default) | control transfers for everything                                      |
| 1           | framed endpoint protocol for everything (`SET_FRAME_MODE` at probe)   |
| 2           | frames for writes, control for reads                                  |

Children call `v003_cmd_out()` / `v003_cmd_in()` and never see the difference;
`v003_data_out()` / `v003_data_in()` carry payloads (always control transfers
for now) and `v003_frame_send()` / `v003_frame_xfer()` are the raw framed API.

Measured through the same gpiochip, control wins every single operation
(set 2.00 ms against 3.93 ms, get 3.00 ms against 6.82 ms), so 0 is the default.
The endpoint path is kept because it is the right home for batched and
fire-and-forget commands - see [frame-protocol.md](frame-protocol.md).

Frame mode is enabled at probe and **disabled again on disconnect**, so the
device is left in raw echo mode for the userspace tests.

## Rules the kernel taught us

- **Stack buffers cannot be DMA mapped.** `usb_control_msg()` with a buffer on
  the stack prints `transfer buffer is on stack` and the transfer fails with
  EAGAIN. In-kernel I2C/SMBus clients pass stack buffers routinely, so the core
  bounces data stage payloads through its own buffers (`bounce_tx`/`bounce_rx`)
  and `kernel/i2c.c` reads into its own `rx` buffer before copying out.
  `usb_control_msg_recv()` bounces internally; that is why
  `v003_transfer_in()` was safe from the start.
- Prefer `get_unaligned_le16()` / `linux/unaligned.h` over pointer casts.
- The gpiochip must be created with `can_sleep = true`: every operation is a USB
  transfer that sleeps.
- `gpio_chip.set` returns `int` since 6.16, `init_valid_mask()` likewise, and
  `need_valid_mask` no longer exists.
- A child module for a capability the device does not have simply does not bind
  (verified with a GPIO-only firmware: `v003-i2c.ko` loaded and sat idle), which
  is why the capability report is what makes build time selection safe.
- The child cells are built from the capability report: `v003_add_cells()` walks a
  name+capability table and adds only what the device says it has (dln2 does the
  same with its hardware revision check).  A device that predates the command is
  treated as the original GPIO+I2C+SPI firmware.  Do not put the capability bit
  in `mfd_cell.id`: that field is the platform device instance number, and using
  it renamed every child device (`v003-gpio.0`, `v003-i2c.3`).
- `usb_interrupt_msg()` is synchronous and allocates a message per call. A URB
  pool (dln2 style) would remove some host side latency, but the 3 frame control
  transfer stays hard to beat for a single small result.

## GPIO child

56 lines (`V003_NGPIO`).  `init_valid_mask()` takes the reserved pins from the
**core**, which got them from the device's capability report plus its own
`reserved=<pin>[,<pin>]` parameter - so the mask covers the USB pins (51-54), the
pins of the enabled modules (PC1/PC2 for I2C, PC5-PC7 for SPI), the flat range
16..31 that has no port behind it, and anything the driver itself drives (the SPI
chip select, which the device cannot know about).  The character device test
asserts that all of them are refused at `request` time with EINVAL.

Reserved lines cost nothing: gpiolib only allocates the mask because
`init_valid_mask` is set, and the report itself is a `const` struct in flash.

`v003-spi.ko` warns when its `cs_pin` is missing from the mask, because then
userspace can request the line the driver is driving.

## Watchdog child

`v003-wdt.ko` exposes the firmware's IWDG as a `watchdog_device`.  Three things
had to be learned from the kernel rather than from the datasheet:

- **A watchdog device with no `.stop` callback must set `max_hw_heartbeat_ms`**
  or `watchdog_register_device()` fails with `-EINVAL` and no log line saying
  why.  `max_timeout` does not satisfy the check.  The IWDG genuinely cannot be
  stopped, so the driver has no `.stop` and declares the hardware limit instead
  (26 s, the most the 12 bit reload and the slowest prescaler can do).
- **The watchdog API counts in seconds**, not milliseconds: `timeout`,
  `min_timeout` and `max_timeout` are seconds, while the firmware command takes
  milliseconds.  Registering with 10000/100/26000 was rejected.
- The reset cause is read once at probe (the device clears it when answering) and
  the IWDG bit becomes `WDIOF_CARDRESET` in a per-device copy of
  `watchdog_info`, which is how a user finds out that the board rebooted on its
  own.

`min_timeout` is one second: the firmware can go below that, but a whole-second
API cannot express it.

## PWM child

`v003-pwm.ko` is a `pwm_chip` with the channel count the device reports
(`npwm` from the capability report, so a firmware without PWM gets no chip),
allocated with `devm_pwmchip_alloc()` and registered with `devm_pwmchip_add()`.
Two things bit me:

- **`v003_data_out()` returns the number of bytes it sent on success**, which is
  the right thing for a transport but not for `.apply()`: returning 12 made the
  PWM core report a failure for a write that had worked, and the same value would
  have made every sysfs write fail.  The SPI and I2C children only ever test
  `ret < 0`, which is why this had not surfaced before.
- In this kernel `struct pwm_chip` has no `parent` field (the parent comes from
  `devm_pwmchip_alloc(dev, ...)`) and `chip->dev` is a struct, not a pointer.

Driving a channel through `/sys/class/pwm/` needs root, so the driver carries a
`selftest=<0|1>` parameter that applies a state and reads it back through the
same callbacks and logs the result - that is what verifies the ns/permille
translation.  The firmware side is covered by `scripts/pwm_test.py`.

## ADC child

`v003-adc.ko` is an IIO device in direct mode: ten channels (0..7 external, 8 the
internal 1.2 V reference, 9 the internal calibration voltage), `read_raw()` doing
one conversion per read, and no buffers or triggers because the device has no way
to convert anything by itself.  The channel count comes from `nadc` in the
capability report.

`read_raw()` maps onto the firmware's two step protocol, and the interesting part
is the completion tag: the conversion runs in the firmware's main loop, so reading
the result right after `V003_ADC_START` can answer with the *previous* conversion.
The driver reads the tag first, sends the request, and polls until the tag changes
(bounded, and it logs what it saw when it gives up).  One read is enough in
practice - a control IN takes ~3 ms against a 42 us conversion - but that is a
property of the timing, not of the protocol.

Two details worth knowing:

- the scale is `IIO_VAL_FRACTIONAL` with `avdd_mv` over 1024, so userspace gets
  exactly `AVDD/1024` (3.22265625 mV on this board) instead of a rounded float.
  The driver cannot measure the board's supply, hence the parameter;
- `industrialio` has to be loaded **before** `insmod v003-adc.ko`, otherwise
  insmod fails with `Unknown symbol devm_iio_device_alloc`.  On the bench machine
  the module ships compressed (`industrialio.ko.zst`), which `insmod` cannot read
  either, so the load sequence is `modprobe industrialio` (or `zstdcat ... > /tmp/
  industrialio.ko` and `insmod` that), then `usb-mfd.ko`, then `v003-adc.ko`.

Verified with `tests/adc_iio_test.py` (three clean runs) against
`selftest=1` on insmod:

| channel | source | raw | mV | via pyusb |
| ------- | ------ | --- | -- | --------- |
| 8 | internal reference | 361-363 | 1163-1170 | 361-366 |
| 9 | internal calibration, 2/4 AVDD | 511 | 1647 | 511 |
| 5, 6 | USB pins (driven high) | 1023 | 3297 | 1023 |
| 7 | USB pin, pulled | 959 | 3087 | 958 |

The last two rows are the useful ones: two independent host paths - sysfs through
the driver, and the vendor protocol over pyusb - report the same counts, which is
what says the driver is not inventing values.  Floating inputs (channels 0..3)
move by 20-40 counts between reads; the internal channels repeat to within one or
two, and only those are asserted on.

## UART child

`v003-uart.ko` puts a TTY on the firmware's USART1: `/dev/ttyV0`, one port, the
standard `tty_port` + `tty_operations` shape used by the serial and USB serial
drivers.  termios maps onto `V003_UART_CONFIG` (baud, 8/9 data bits, parity, 1/2
stop bits), and the rate the hardware really divides to is written back with
`tty_termios_encode_baud_rate()`, so `cfgetospeed()` reports 115107 for a
requested 115200 instead of pretending.  CRTSCTS is cleared and said so in the
log: this board has no RTS/CTS pins (they are the I2C SCL line and SPI pins).

The receive path is **polled**, because nothing about this link pushes data to the
host: the firmware holds up to 64 bytes in its ring, and a delayed work reads the
state, pulls what is there with `V003_UART_READ` and pushes it into the flip
buffer.  The interval is derived from the configured baud so the ring cannot fill
between two polls (half a ring per poll, 1-10 ms), and when the rate is too high
for that - the ring fills faster than one control transfer takes, which is the
case from about 300 kbaud - the driver logs it and reports `lossy=1` in its
`stats` attribute instead of quietly losing bytes.

The transmit path uses the firmware's ring as flow control: `write()` sends what
the ring has room for and returns that count, `write_room()` reports the cached
room, and the poll work calls `tty_wakeup()` once the ring drains so the TTY layer
hands over the rest.  Nothing sleeps in an atomic context, and the only buffers
are the driver's own (the core's bounce buffers carry the transfers).

Two things learned the hard way:

- **A `tty_port` embedded in your own structure must never be `tty_port_put()`.**
  The last put runs the port's destructor, which ends in `kfree(port)` unless the
  driver provides a `->destruct` hook - so putting an embedded port frees a
  pointer *inside* the driver's allocation.  That is an interior free, and it
  corrupts the allocator: after the first load and unload of this driver, the
  next load found the device model's power management list already broken
  (`list_add corruption ... <- device_pm_add <- device_add <-
  tty_register_device_attr <- v003_uart_probe`) and systemd-udevd,
  systemsettings and a kworker all oopsed on the same freed object, with the
  machine needing a reboot.  The port is now destroyed directly
  (`tty_port_destroy()`, the documented alternative in `tty_port_init()`'s
  kerneldoc for "refcounting not used") and the device unregistered with
  `tty_unregister_device()`; there is no `tty_port_put()` in the file.

  *The first write-up of this blamed a double release, because
  `tty_port_unregister_device()` looked like it released the port as well.
  Reading `drivers/tty/tty_port.c` shows it does not - it unregisters the device
  and returns - so the put was not doubling anything, it was the only put, and
  one was already one too many.*
- **`sysfs_emit()` may only be called at the start of the buffer.**  Building a
  multi-line attribute with `sysfs_emit(buf + n, ...)` warns
  (`invalid sysfs_emit: buf:...`, `fs/sysfs/file.c:757`) and returns 0, so every
  line after the first silently disappears; `sysfs_emit_at(buf, n, ...)` is the
  helper for that.

`stats` is a read-only attribute on the tty device: one line of driver state
(`open`, `baud`, `actual`, `poll_ms`, `lossy`), one of firmware state
(`enabled`, `tx_queued`, `rx_available`, the two drop counters), one of the
firmware's traffic counters and one of the driver's.  `stats_reset` (write only,
therefore root only) asks the device to zero its counters, and opening the port
does the same thing: those counters are eight bits wide, the receiver's interrupt
count reaches 255 after a few hundred bytes, and a saturated counter is only
useful as a difference - which is also what lets a test run as an ordinary user
measure anything at all.

Measured with `tests/uart_tty_test.py` (five clean runs, PD0<->PD1 jumper in
place):

| check | result |
| ----- | ------ |
| 32 byte pattern at 9600 and 115200 | byte for byte, `tx_bytes`/`rx_bytes` deltas exact, one receiver interrupt per byte, no drop or error counter moving |
| 256 bytes in one `write()` at 9600 | 252-288 ms (the wire needs 267 ms), all 256 back, `tx_dropped` +0 - the transmit ring holds 31, so this is nine rounds of `tty_wakeup()` |
| requested vs reported rate | 115200 -> the device reports 115107, which is what the driver writes into termios |
| close | firmware `enabled=0`: the port gives PD0/PD1 back, which is what makes the board flashable again |
| reopen | `enabled=1` |

Load, unload and reload three times leaves dmesg clean, which is the check that
matters for the corruption described above.

## I2C child

`i2c_algorithm.master_xfer()` maps to the firmware's one-transaction-per-request
model:

| Linux message pattern                     | Firmware command            |
| ----------------------------------------- | --------------------------- |
| write (`!I2C_M_RD`)                       | `WRITE` + data stage        |
| read directly after a write in one message| `WRITE_READ` (repeated START) |
| lone read                                 | `READ` (current address read) |
| read data                                 | `GET_RX` (control IN data stage) |

- `I2C_M_TEN` and `I2C_M_NOSTART` are refused (`-EOPNOTSUPP`), and the message
  limits differ by direction: a write carries its payload in one control data
  stage (71 bytes next to the address byte), a read is limited by the firmware's
  receive buffer (64).  Using the read limit for both rejected AT24C256 page
  writes (66 bytes) until it was measured; `tests/i2c_dev_test.py` now covers
  that message.
- SMBus is advertised for the sizes the firmware can actually express
  (`0x7f0001`: quick, byte, byte data, word data) and the i2c core emulates them
  on top of `master_xfer`; `i2cdetect` needs quick to probe at all.  The block
  sizes are **not** advertised: their emulation continues a transfer with
  `I2C_M_NOSTART`, which one-transaction-per-request firmware cannot do.
- The firmware's status maps to errno: address NACK `-ENXIO`, data NACK `-EIO`,
  read-phase NACK `-EREMOTEIO`, clock stretch or timeout `-ETIMEDOUT`, not
  configured `-ENODEV`.
- The result is read with `I2C_GET_RESULT` and polled until its tag changes,
  because a control data stage is executed after the transfer completes - the
  story is in [i2c.md](i2c.md).
- `/dev/i2c-N` is `root:i2c`; one udev rule hands it to `plugdev` (see README).
  With it, `tests/i2c_dev_test.py` runs the whole adapter from userspace without
  root.

## SPI child

- Pins (fixed by the chip, no remap needed): SPI1 defaults to **PC5 = SCK,
  PC6 = MOSI, PC7 = MISO** even without touching `AFIO->PCFR1` - the CH32V003
  maps SPI1 to port C by default (RM table 7-11), which the vendor EVT SPI
  examples confirm.  In the flat gpiochip numbering these are lines 37/38/39.
- NSS is the one thing the remap bit moves, PC1 by default and PC0 when
  remapped, and both collide with this project (PC1 is I2C SDA, PC0 is the
  board's LED).  The firmware therefore runs the SPI peripheral with software
  slave management (`SPI_CTLR1_SSM | SPI_CTLR1_SSI`), which leaves NSS unclaimed
  and keeps PC1 usable for I2C.  Switching to hardware NSS would silently take
  the I2C data line over.
- Chip select is a plain GPIO, not the peripheral's NSS: the firmware drives the
  pin given to `V003_SPI_SET_CS` (0xffff = none, the default) and the kernel
  driver drives one from its `cs_pin` parameter through the GPIO module.  PC4
  (line 36, the one used in testing) is free of SPI1 alternate functions.
- `transfer_one` splits transfers longer than the firmware's 64 byte limit into
  chunks; `set_cs` drives the chip select through the **GPIO module** because a
  Linux *message*, not a transfer, sits between two CS edges (a multi-transfer
  message is normal for register access), while the firmware only drives its own
  pin around a single transfer. `V003_SPI_SET_CS` is set to `0xffff` so the
  firmware never interferes.
- `max_speed_hz` maps to the `SPI_CTLR1` BR field: 24 MHz (BR 0 = /2) down to
  187.5 kHz (/256). The prescaler and mode are only re-sent when they change.
- A receive-only transfer sends zeros, because the firmware always clocks
  something out.
- `selftest=<bytes>` loops a pattern through with MOSI jumped to MISO using
  `spi_new_device()` + `spi_sync()`, i.e. the same path a client driver takes.

## I2C child self test

`selftest=<7 bit address>` (plus `selftest_off`, `selftest_len`) reads bytes at
probe with the two message write+read transfer `at24` uses and logs them with
`dmesg`. It exists because `/dev/i2c-N` may not be reachable, and it proves the
whole adapter path in one line of kernel log.

## The upstream `at24` driver

Instantiating it needs root (`/sys/bus/i2c/devices/i2c-N/new_device`) and the
legacy `eeprom=` module parameter no longer exists in current kernels, so there
is no unprivileged way to bind it. The equivalent coverage is
`tests/i2c_dev_test.py`, which performs the same two message transfers through
i2c-dev.
