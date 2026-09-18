# Firmware

The firmware in `vendor/` makes a CH32V003 look like a USB to GPIO / I2C / SPI
bridge. It is not a Linux kernel driver: it has 16 KB of flash, 2 KB of RAM and
a bit banged USB stack, so almost every design decision here is a consequence of
those three numbers.

## Structure

| File             | Contents                                                              |
| ---------------- | --------------------------------------------------------------------- |
| `vendor/vendor.c`| control endpoint handling, request rings, the main loop, EP3 IN FIFO    |
| `vendor/frame.c` | framed protocol on the endpoint path (bytes in, responses out)          |
| `vendor/gpio.c`  | module 0x01: pin mode, set, get                                        |
| `vendor/i2c.c`   | module 0x03: bit banged master                                         |
| `vendor/spi.c`   | module 0x02: hardware SPI1 master                                      |
| `rv003usb/`      | the software USB stack (vendored, patchable)                           |

Two contexts, and the split between them is the single most important thing to
understand:

- **USB interrupt**: receives packets and answers control transfers. Everything
  it does must finish in **~4.5 us** (see below), so it only ever moves bytes:
  it copies OUT payloads into queues and drains the EP3 IN FIFO.
- **`main()`**: executes every command. GPIO writes, I2C bit banging and SPI
  clocking can take milliseconds, which is why they are queued, and why a result
  is not ready the moment a control transfer returns (see *completion tags*).

## Command reference

Requests are vendor control transfers: `bmRequestType` `0x40` (OUT) / `0xC0`
(IN), `bRequest` 0, `wValue` carries the argument, `wIndex` is
`cmd | (module << 8)`, and commands with a payload use a control data stage.

| Module | cmd  | Name                    | Argument (wValue)                          | Result / data stage                             |
| ------ | ---- | ----------------------- | ------------------------------------------ | ----------------------------------------------- |
| 0x00   | 0x30 | GET_DEVICE_VER          | -                                          | `0x1010`                                        |
| 0x00   | 0x31 | GET_DEVICE_SN           | -                                          | `0x12345678`                                    |
| 0x00   | 0x32 | GET_EP_STATS            | 0-2 RX bytes, 3 EP3 IN packets, 4 bytes    | counter; `wValue 0xff` on OUT resets them        |
| 0x00   | 0x33 | GET_FIFO_LEVEL          | -                                          | bytes queued for EP3 IN                         |
| 0x00   | 0x34 | GET_FIFO_DROPS          | -                                          | bytes dropped when that FIFO was full           |
| 0x00   | 0x35 | GET_REQ_DROPS           | -                                          | OUT requests dropped because the ring was full  |
| 0x00   | 0x36 | GET_CTRL_OUT_DATA       | -                                          | last control-OUT data stage (IN data stage)     |
| 0x00   | 0x37 | GET_CTRL_OUT_SEQ        | -                                          | requests the main loop has finished             |
| 0x00   | 0x38 | GET_CTRL_OUT_DROPS      | -                                          | data stages dropped because both slots were busy |
| 0x00   | 0x39 | SET_FRAME_MODE          | 0/1                                        | framed EP path off/on (zero length OUT)         |
| 0x00   | 0x3a | GET_FRAME_STATS         | -                                          | `handled << 16 \| dropped`                      |
| 0x00   | 0x3b | GET_STACK_FREE          | -                                          | untouched bytes at the deepest stack point      |
| 0x01   | 0x06 | GPIO_SET                | `pin << 8 \| value`                        | -                                               |
| 0x01   | 0x07 | GPIO_GET                | `pin << 8`                                 | 0/1                                             |
| 0x01   | 0x08 | GPIO_REQUEST            | `pin << 8`                                 | claims the pin as input with pull-up/down       |
| 0x01   | 0x09 | GPIO_FREE               | `pin << 8`                                 | -                                               |
| 0x01   | 0x0a | GPIO_GET_DIRECTION      | `pin << 8`                                 | 1 = input, 0 = output                           |
| 0x01   | 0x0b | GPIO_DIRECTION_INPUT    | `pin << 8`                                 | -                                               |
| 0x01   | 0x0c | GPIO_DIRECTION_OUTPUT   | `pin << 8 \| value`                        | -                                               |
| 0x02   | 0x40 | SPI_ENABLE              | 0/1                                        | -                                               |
| 0x02   | 0x41 | SPI_CONFIG              | `prescaler << 8 \| mode`                    | -                                               |
| 0x02   | 0x42 | SPI_GET_STATE           | -                                          | `prescaler << 16 \| mode << 8 \| enabled`       |
| 0x02   | 0x43 | SPI_GET_STATS           | -                                          | MOSI bytes since reset                          |
| 0x02   | 0x44 | SPI_SET_CS              | pin or 0xffff                              | -                                               |
| 0x02   | 0x45 | SPI_TRANSFER            | -                                          | OUT data stage up to 64 bytes                   |
| 0x02   | 0x46 | SPI_GET_RX              | -                                          | MISO bytes of that transfer (IN data stage)     |
| 0x02   | 0x47 | SPI_GET_CS              | -                                          | the chip select pin, 0xffff when none           |
| 0x03   | 0x50 | I2C_CONFIG              | half period in 100 ns units                | -                                               |
| 0x03   | 0x51 | I2C_WRITE               | -                                          | OUT data stage `[addr<<1][bytes...]`            |
| 0x03   | 0x52 | I2C_READ                | -                                          | OUT data stage `[addr<<1\|1][count]`            |
| 0x03   | 0x53 | I2C_GET_RX              | -                                          | bytes read by the last transfer                 |
| 0x03   | 0x54 | I2C_GET_STATUS          | -                                          | status of the last transaction                  |
| 0x03   | 0x55 | I2C_SCAN                | first 7 bit address                        | probes 32 addresses into a bitmap               |
| 0x03   | 0x56 | I2C_GET_SCAN            | -                                          | that bitmap                                     |
| 0x03   | 0x57 | I2C_WRITE_READ          | bytes to read back                         | OUT data stage `[addr<<1][bytes...]`, repeated START |
| 0x03   | 0x58 | I2C_GET_RESULT          | -                                          | `(request counter << 24) \| status`             |
| 0x03   | 0x59 | I2C_GET_CFG             | -                                          | half period in 100 ns units                     |
| 0x03   | 0x5a | I2C_TRACE               | 1 arms, 0 stops                            | clock tracer, see `scripts/i2c_trace.py`        |
| 0x03   | 0x5b | I2C_GET_TRACE           | byte offset into the trace                 | trace entries (IN data stage)                    |
| 0x03   | 0x5c | I2C_GET_TRACE_INFO      | -                                          | `count \| overflow << 8 \| unit << 16`          |
| 0x03   | 0x5d | I2C_WAIT_READY          | timeout in ms                              | OUT data stage `[addr<<1\|0]`, ACK polls        |
| 0x03   | 0x5e | I2C_GET_WAIT_US         | -                                          | duration of the last WAIT_READY                 |
| 0x03   | 0x5f | I2C_MEM_WRITE_READ      | read count, 1 byte word addr, ACK poll bits | write + optional ACK poll + random read in one request |

`scripts/*.py` are the reference hosts for all of this, and
`kernel/usb-mfd.h` mirrors the command set for the Linux side.

## The two budgets

### 1. ~4.5 us inside the USB interrupt

A vendor request handler runs *before* rv003usb acknowledges the SETUP packet.
If it takes too long, the ACK misses its low speed window and the host fails the
transfer with EIO - a symptom that looks like a broken device rather than a slow
handler.

Measured with `V003_GET_TIMING` (0x3f, `wValue` = loop iterations, returns the
SysTick delta; SysTick counts CPU cycles here, 48 MHz):

| iterations | ticks | outcome |
| ---------- | ----- | ------- |
| 10         | 171   | answered |
| 12         | 203   | answered (~4.2 us) |
| 13+        | 260+  | host reports EIO |

Consequences, all of them already implemented:

- control-OUT data stages are copied to a slot by the interrupt and executed by
  `main()` (two slots, so a second request can arrive while the first runs);
- framed requests are accumulated by the interrupt and answered by `main()`;
- anything expensive (the stack scan behind `GET_STACK_FREE`, I2C bit banging,
  SPI clocking) never runs in interrupt context.

### 2. ~790 bytes of stack

The stack starts at `0x20000800` and grows **downwards into the statics**. There
is no MPU and no fault: an oversized buffer or a deep call chain silently
overwrites whatever sits at the end of `.bss`. That is exactly how the EP data
path appeared to be broken for a while - `ep_rx_count` and the EP3 IN FIFO hold
garbage as soon as anything overruns.

Measured on the current build (`.bss` ends at `0x200004f4`, so **780 bytes** of
stack) with `V003_GET_STACK_FREE` (0x3b): the deepest use since boot is
**272 bytes**, i.e. 508 bytes are still free.  That number went *down* when the
debug printf was switched off - the printf path, not the USB code, had been the
deepest stack consumer.  Re-measure after touching anything that adds call
depth; `nm vendor/vendor.elf | grep _ebss` gives the current limit.

What that cost, and what to do about it:

- the rv003usb UEvent debug ring was 32 entries (512 bytes, a quarter of RAM)
  for a debug channel that actively hurts (see below): `RV003USB_NUMUEVENTS` is
  now configurable and the vendor build uses 8;
- the EP buffers were 64 bytes each although a low speed endpoint can only
  deliver 8 byte packets: they are 16 now;
- the frame responses share the EP3 IN FIFO instead of owning a second ring;
- `V003_GET_STACK_FREE` measures the remainder by painting the free RAM at boot
  and scanning it for the first overwritten byte. The scan takes ~150 us, which
  is 30x the interrupt budget, so it runs from `main()` and the command returns
  a cached value (the first read arms the measurement, the next one reports it).

When adding a buffer, put the new numbers next to these.

## Completion tags: why a result is not ready when the transfer returns

A control transfer that carries a data stage is answered by the interrupt as
soon as the bytes arrived; the command itself is executed later by `main()`.
Hosts that read a result immediately can therefore read the *previous* result.

Two mechanisms exist:

- `V003_GET_CTRL_OUT_SEQ` (0x37) counts the requests `main()` has finished. A
  host can read it, send a request, and poll until the counter moves (that is
  what `scripts/*.py` do through their `sync()` helper).
- `V003_I2C_GET_RESULT` (0x58) folds that counter into the result value itself
  (`(seq << 24) | status`), so one poll loop suffices and there is no baseline
  transfer. Re-reading a plain status is **not** enough: two requests that fail
  the same way look identical.

Full story and the measurement that proved it: [debugging.md](debugging.md).

## The framed endpoint protocol

Requests on EP2 OUT, responses on EP3 IN, `echo` tags for matching, `echo == 0`
for fire and forget. Wire format, semantics and measured performance are in
[frame-protocol.md](frame-protocol.md); `vendor/frame.c` is the implementation.

It exists so that payload heavy commands have a home that does not need a
completion tag (the main loop builds the response *after* doing the work) and so
that batched or fire-and-forget traffic can be tried at all. For single small
commands the control path is faster - see the numbers in
[frame-protocol.md](frame-protocol.md).

## Debug printing

`make DEBUG_EVENTS=1` prints the rv003usb UEvent ring over the bit-banged debug
channel (`minichlink -T` reads it). It is **off by default**: that output costs
milliseconds per event and blocks `main()`, which then stops servicing the
requests it still owes the host. Zero length control OUTs are queued for
`main()`, so a blocked `main()` means `SET_FRAME_MODE` silently never takes
effect and the framed path stays dead - that is not hypothetical, it is how a
morning was spent.

`LogUEvent()` from the interrupt is cheap and still compiled in, so GDB can
inspect the ring (`events`, `eventhead`, `eventtail`).
