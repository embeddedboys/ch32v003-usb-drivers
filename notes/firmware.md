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
| `vendor/wdg.c`   | module 0x04: IWDG and the reset cause                                  |
| `vendor/pwm.c`   | module 0x05: TIM1 channels on PD2 and PA1                              |
| `vendor/adc.c`   | module 0x06: ADC1, one conversion per request                          |
| `vendor/uart.c`  | module 0x07: USART1 on PD0/PD1, a ring in each direction               |
| `vendor/pwr.c`   | module 0x08: sleep and standby, wake sources, the wake reason          |
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
| 0x00   | 0x31 | GET_DEVICE_SN           | -                                          | first word of the unique id                     |
| 0x00   | 0x3c | GET_DEVICE_UID          | -                                          | 12 byte ESIG unique id (IN data stage)          |
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
| 0x04   | 0x60 | WDG_START               | timeout in milliseconds                    | starts the IWDG (one way door)                  |
| 0x04   | 0x61 | WDG_FEED                | `0xaaaa`                                   | reloads the counter                             |
| 0x04   | 0x62 | WDG_GET_STATE           | -                                          | `(actual timeout ms << 8) \| running`           |
| 0x04   | 0x63 | WDG_GET_RESET_CAUSE     | -                                          | cause flags of the last reset, then cleared     |

`scripts/*.py` are the reference hosts for all of this, and
`kernel/usb-mfd.h` mirrors the command set for the Linux side.

## The factory unique id (ESIG)

Chapter 15 of the reference manual: the ESIG sits in the system memory area, is
programmed by WCH and is read only.  `R16_ESIG_FLACAP` (flash capacity) is at
`0x1FFFF7E0`, the 96 bit unique id (`UNIID1..3`) at `0x1FFFF7E8..F0` - ch32fun
already declares all of it as `ESIG_TypeDef`, so the firmware uses
`&ESIG->UNIID1` rather than a hardcoded address.

Two things were learned the hard way:

- **Reading it from the USB interrupt breaks the bit banging.**  Pointing the
  control-IN data stage straight at the ESIG looked free (no RAM copy, the host
  reads the memory mapped id) and failed: the host got EIO, exactly like a
  handler that overruns the ACK window.  `main()` now copies the 12 bytes into a
  RAM array once at boot (`serial_from_esig()`) and both the command and the
  serial number string use that copy.  The ESIG is a slow read, and the ISR has
  a few microseconds.
- **Only 64 of the 96 bits are programmed on this part.**  An independent read
  through the programmer (`minichlink -r ... 0x1FFFF7E8 16`) returns
  `cd ab 0b 92 82 bc 5a fa ff ff ff ff`, so the third word is blank and the
  serial number string ends in `ffffffff`.  That is the chip's answer, not a
  read failure - `minichlink -i` reports the same eight bytes as its part UUID.

The serial number string is built from that id at boot (hex, UTF-16LE) into
`v003_serial_descriptor`, which costs 50 bytes of RAM; the descriptor table stays
in flash because it holds a pointer to the buffer rather than to a literal.
Total cost of the feature: 64 bytes of RAM (the string, the 12 byte id copy and
alignment) and ~130 bytes of flash.

## Room for more modules

Measured, so the question "can we add ADC/PWM/UART" has an answer instead of a
feeling.  The flash column is what a module costs in the default build (the
default LTO build is 11552 bytes; these modules are about three quarters of it,
rest is `vendor.c`'s control path, the descriptors and the ring code):

| module                  | lines | flash  | RAM    |
| ----------------------- | ----- | ------ | ------ |
| `i2c.c` (bit banging)   | 648   | ~2.4 kB | 192 B |
| `spi.c` (peripheral)    | 177   | ~1.0 kB | 64 B  |
| `frame.c`               | 238   | ~0.8 kB | 152 B |
| `gpio.c`                | 52    | ~0.36 kB | 0 B  |
| `wdg.c`                 | 127   | ~0.48 kB | 8 B  |
| `pwm.c`                 | 157   | 592 B   | 24 B |
| `adc.c` (peripheral)    | 199   | 628 B   | 12 B |
| `uart.c` (peripheral)   | 360   | 1292 B  | 128 B |
| `pwr.c` (peripheral)    | 330   | 1128 B  | 44 B  |
| `rv003usb.c`            | 463   | ~0.87 kB | 208 B |

Current budget, default build (every module, I2C tracer on): flash 12740/16384
(**3,644 bytes free**), RAM 1424/2048, with the stack measurement reporting
**352 bytes free** on a fresh boot and **448 bytes** after the whole test suite has
exercised every module - the number that matters, because the canary reports the
deepest use it has seen, and a mixed workload gets closer to the statics than an
idle device does (measured: 308 bytes before `scripts/combo_test.py` started
asserting it).  The I2C tracer, which is now off by default, is still the biggest
single thing a build can give up or take back: `make TRACE=1` costs 232 bytes of
flash and 140 of RAM.

- **Flash is not the constraint.** The peripheral wrappers are all of the
  `spi.c`/`gpio.c` class and measured: PWM 592 bytes, ADC 628, UART 1292, PWR 1128
  (the AWU divider table and two sleep paths).  The default build has 3.6 kB of
  flash free.
- **RAM is the constraint.** Statics are 1424 bytes and the rest is stack, so a
  new buffer is paid for out of that 352 byte margin (a floor of ~150-200 bytes
  of margin is what the current call chains need).  PWM took 24 bytes, ADC 12 and
  PWR 44, all state rather than buffers; UART took 128 for its two rings, the
  largest single allocation any module has asked for.  Those were paid for by
  shrinking the zero length vendor OUT request ring from 8 entries to 4
  (`V003_NUM_SIMPLE_REQUESTS`, 32 bytes, drops visible through
  `V003_GET_REQ_DROPS`) and the debug UEvent ring from 8 entries to 4 and then 2
  (`make UEVENTS=4` brings it back), because the rule is to keep ~350 bytes of
  margin.
- Easy RAM to reclaim first, and it argues for the same mechanism: the I2C clock
  tracer (136 B of RAM, 224 B of flash) is a debug facility, and the UEvent ring
  (128 B for 8 entries) could shrink.  Gating those buys a UART ring outright.

So the sane shape is: **compile time module selection plus a capability report**,
and the second half of that is in place:

- `V003_GET_CAPABILITIES` (0x3d) returns a 16 byte struct through a data stage,
  so it can grow by appending fields: which modules exist (`V003_CAP_*`), the
  channel counts, and **which pins the device owns**.  It is a `const` in flash
  and the data stage points straight at it, so it costs no RAM at all.  The
  device currently reports `0x1ff` (gpio, spi, i2c, adc, pwm, uart, wdg, pwr and
  the framed protocol), 56 gpio lines and the reserved mask below.  That is every
  module the firmware has, which is also why the guard that used to refuse the
  reserved names (`uart`, then `pwr`) is gone: there are none left.
- The reserved pin mask covers the USB pins, the pins of the modules that are
  enabled, and the flat range 16..31 that has no port behind it - so userspace
  cannot write into an address that is not a GPIO, cannot drive the bus it is
  talked to over, and cannot fight the I2C or SPI pins.
- The kernel core builds its child cells from that mask instead of a hardcoded
  list, and the GPIO child takes the mask from the core, so a firmware with a
  module compiled out does not get a child driver probing it.  A device without
  the command is treated as the original GPIO+I2C+SPI build.
- **Compile time selection works**: `make MODULES=gpio,spi,i2c` (default) or
  `make MODULES=gpio,spi`, `make MODULES=gpio`, plus `make TRACE=1` for the I2C
  clock tracer.  Measured on the same tree:

  | build                       | flash | RAM   |
  | --------------------------- | ----- | ----- |
  | `gpio,spi,i2c` + trace      | 8548  | 1332  |
  | `gpio,spi,i2c`, `TRACE=0`   | 8324  | 1196  |
  | `gpio,spi`, `TRACE=0`       | 6500  | 1112  |
  | `gpio`                      | 5624  | 1028  |

  A GPIO-only build reports `0x41` (gpio + frame) and a reserved mask with only
  the USB pins, which is the whole mechanism working end to end: the kernel core
  added one cell, and `v003-i2c.ko` loaded without a device and stayed idle
  instead of probing something that is not there.
- `V003_MODULE_*` macros default to today's set, so a plain `make` is unchanged
  (byte for byte in flash and RAM).  `adc`, `pwm`, `wdg`, `uart` and `pwr` are
  all implemented and in the default build; the reserved-name compile error that
  guarded the ones without an implementation is gone with the last of them.
- Compatibility: capabilities are additive.  A host that does not know the
  command treats the device as the old fixed set (that is what the core does
  when the read fails).
- Pins the *driver* owns (a chip select line, an IRQ) cannot come from the
  device, so the core takes `reserved=<pin>[,<pin>]` and merges it in;
  `v003-spi.ko` warns when its `cs_pin` is not in the mask.

Two constraints a new module has to respect, both already learned here:

- **Pin ownership is real now.**  With PWM/ADC/UART enabled, "all 56 lines are
  GPIO" stops being true, and letting userspace drive a line a peripheral owns is
  the same silent breakage as the SPI NSS/PC1 collision.
- **The interrupt budget stays 4.5 us.**  Anything that wants an ISR competes
  with the USB SETUP ACK.  The UART is the first module to take one (a single
  handler for RXNE and TXE, moving one byte and nothing else) and it had to be
  measured rather than argued: 29,696 bytes of loopback traffic at 3 Mbps while
  1,961 USB control transfers completed with no errors and none slower than
  20 ms ([uart.md](uart.md)).  Interrupts are not nested here, so a peripheral
  ISR can only *delay* the USB handler by its own length - anything longer than a
  few microseconds is still not allowed.

## Modules together

The modules are designed to share the device, and `scripts/combo_test.py` is what
checks it: one round trip per module in one session, then all of them again after
a standby sleep.  Two rules came out of that exercise, both of them from failures:

- **A module may read a pad another module drives, but must not take it away.**
  The ADC's channel 2 is PC4 (the SPI chip select) and its channel 1 is PA1 (PWM
  channel 2), so the ADC configures no pin at all; the module that drives a pin
  re-asserts its mode when it matters (`pwm_apply()` does, which is what keeps a
  channel driving after a host has read that pin's level through the GPIO module).
- **What a module depends on outside its own registers has to be re-asserted.**
  A standby wake calls `SystemInit()`, which rewrites `RCC->CFGR0`: the ADC's
  prescaler went back to its reset value and conversions silently ran four times
  faster (42 us -> 11 us) while every reading stayed correct.  The divider is now
  written per conversion.

## What this board actually has

Probed pin by pin with the GPIO module (drive 0, drive 1, read back), because "it
is in the reference manual" is not the same as "it is bonded out and nothing else
holds it":

| pins | result |
| ---- | ------ |
| PA1, PA2 | drivable |
| PA0, PA3..PA15 | read 0 whatever is written - not connected on this package |
| PC0..PC7 | drivable (PC0 is the LED, PC1/PC2 the I2C pair, PC4 the SPI chip select and ADC_IN2, PC5..PC7 the SPI pins) |
| PD0, PD2 | drivable (PD2 is PWM channel 1) |
| PD1 | **held high by the programmer: it is SWIO**; needs a jumper to PD0 to be an input ([uart.md](uart.md)) |
| PD3..PD6 | the USB pair, the USB pull-up and the boot button; not probed on purpose, driving them breaks the link |

## PWM (TIM1)

`V003_MODULE_PWM`, module id 0x05, two channels on **PD2** and **PA1**.
`PWM_SET` takes a data stage of `{channel, enable, duty in permille, period in
ns}` - the shape the Linux PWM API hands a driver - and `PWM_GET` reads the same
structure back with the period the prescaler and reload actually work out to.

The pin choice is forced by the chip being a 20 pin part with no PA8..PA11: the
four TIM1 channels only exist where the remap table puts them, and in the default
mapping (表 7-8, `TIM1_RM=00`) that is CH1 = PD2, CH2 = PA1, CH3 = PC3,
CH4 = PC4.  PC3 and PC4 are this board's spare GPIO and its SPI chip select, so
only the first two are offered; both are reported in the reserved pin mask, which
is how userspace finds out that PA1 is not a GPIO any more.

Two things worth remembering:

- **The arithmetic is deliberately 32 bit.**  Writing `period_ns * 48 / 1000`
  directly pulls `__udivdi3` in and costs about 1.5 kB of flash for a two channel
  PWM - more than the whole module.  Dividing first (cycles per microsecond is a
  constant) brings the module down to 592 bytes of flash and 24 bytes of RAM.
- **A read back caught what a level check could not.**  The pin level at 0 % and
  100 % duty is static at any frequency, so it only proves the timer drives the
  pin; the period read back is what exposed a factor of 1000 in the cycle
  constant (a 1 ms request was running at 1 s).  `scripts/pwm_test.py` checks
  both, and says in its header what still needs a scope: any duty in between.

## Watchdog (IWDG)

`V003_MODULE_WDG`, module id 0x04.  WCH's sequence, exactly as their own example
does it: `0x5555` to `IWDG->CTLR` opens the prescaler and reload registers,
`0xaaaa` reloads the counter (this is also the feed), `0xcccc` starts it.  The
firmware picks the smallest prescaler whose reload fits the 12 bit register, so
any timeout from about a millisecond to 26 s can be requested, and it reports the
timeout it *configured* rather than the one that was asked for, because the LSI is
only good to about +-20 % (32..40 kHz, RM chapter 9).

Three things worth knowing:

- **It is a one way door.**  The IWDG cannot be stopped once started, so
  `WDG_FEED` is the only way to keep the chip alive from then on.  Feeding takes
  the key `0xaaaa` and does nothing with a wrong value, so a host cannot kick it
  by accident.
- **The reset cause is captured at boot and cleared**, so `WDG_GET_RESET_CAUSE`
  answers with the flags of the *last* reset (pin, power-on, software, IWDG, WWDG,
  low power) and answers once.  The kernel driver turns the IWDG bit into
  `WDIOF_CARDRESET`, so `wdctl` and friends report it.
- **After a reset the IWDG is off again** (that is the hardware, not the
  firmware), so `running` is truthful and a host has to re-arm it.  A device that
  resets in a loop because nothing feeds it is exactly what the flags are for.

Verified on hardware by letting it bite: `scripts/wdg_test.py --reset 2000` arms
2 s, feeds it for another 1.5 s (no reset), stops feeding, sees the device
disappear, and finds it back with `watchdog` as the reset cause and the watchdog
off.  The non-destructive half of that script runs by default and is also part of
`scripts/v003_test.py`.

Cost: 484 bytes of flash and 8 bytes of RAM (the 32 bit divides by the prescaler
table pull in `__udivsi3`, which is most of it).

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
