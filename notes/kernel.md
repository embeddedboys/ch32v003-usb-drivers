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
- `usb_interrupt_msg()` is synchronous and allocates a message per call. A URB
  pool (dln2 style) would remove some host side latency, but the 3 frame control
  transfer stays hard to beat for a single small result.

## GPIO child

56 lines (`V003_NGPIO`), `init_valid_mask()` clears the pins the device itself
uses: 51/52/53 = PD3/PD4/PD5 (D+, D-, the D- pull-up switch) and 54 = PD6 (boot
button). Userspace must not be able to drive the bus it is talking over, and the
character device test asserts that those four are refused.

Reserved lines cost nothing: gpiolib only allocates the mask because
`init_valid_mask` is set.

**Open**: a chip select pin configured for `v003-spi.ko`, and PC1/PC2 (I2C),
PC5/PC6/PC7 (SPI) are still offered by the gpiochip. The child drivers claim
what they use, but the gpiochild's valid mask is built before they probe, so the
clean fix is a reservation mask in the core (or a `reserved=` parameter on the
core) - see [TODO.md](../TODO.md).

## I2C child

`i2c_algorithm.master_xfer()` maps to the firmware's one-transaction-per-request
model:

| Linux message pattern                     | Firmware command            |
| ----------------------------------------- | --------------------------- |
| write (`!I2C_M_RD`)                       | `WRITE` + data stage        |
| read directly after a write in one message| `WRITE_READ` (repeated START) |
| lone read                                 | `READ` (current address read) |
| read data                                 | `GET_RX` (control IN data stage) |

- `I2C_M_TEN` and `I2C_M_NOSTART` are refused (`-EOPNOTSUPP`); SMBus emulation is
  deliberately not advertised.
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
