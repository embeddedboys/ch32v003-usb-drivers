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
