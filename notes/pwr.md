# Power management

`vendor/pwr.c`, module id `0x08`, commands `0xa0`-`0xa2`. This is the one module
that changes whether the device exists as far as the host is concerned: CH32V003
low speed USB is bit banged, so a sleeping core cannot answer anything.

## What the hardware offers (RM 2.3)

| | enter | wake sources | clocks | keeps |
| --- | ----- | ------------ | ------ | ----- |
| SLEEP | `SLEEPDEEP=0, PDDS=0`, `WFI`/`WFE` | any interrupt or event | core stopped, peripherals and IO untouched | everything |
| STANDBY | `SLEEPDEEP=1, PDDS=1`, `WFI`/`WFE` | any EXTI interrupt or event, the AWU, NRST, an IWDG reset | HSE/HSI/PLL off | **SRAM, the register file and the IO states**, plus the IWDG and the LSI running |

An **AWU wake does not reset the chip**: the clock comes back on HSI and the
firmware continues after the `WFE`, so the RAM counters survive and the module can
report how many times it slept.  `SystemInit()` takes the clock back to 48 MHz and
SysTick has to be re-armed, because it is set up in `handle_reset()`.

The AWU counts the internal ~128 kHz LSI through `AWUPSC` (1, 1, 2, 4, ... 10240,
61440) into a six bit window (`AWUWR`, 0..63), so a sleep is between one tick and
63 ticks of the coarsest division: **10 ms to 30 s**, with a granularity that
depends on the divider (2 ms, 8 ms, 16 ms at the durations tested).

## The rules this module follows

- **Nothing sleeps unless a host asks.**  A firmware that sleeps by itself cannot
  be reprogrammed over SWIO while it does - the debug interface is part of what
  standby switches off - which is the trap WCH's own examples warn about.
- **Every sleep has an AWU backstop**, even when a wake pin is configured, so no
  request can park the device with no way back.  (Operationally: in standby the
  programmer cannot reach the chip either, so a device that sleeps forever needs a
  power cycle.)
- **The sleep starts a few milliseconds after the control transfer that asked for
  it** (`V003_PWR_ARM` with a start delay, default 10 ms), from the main loop.
  Sleeping inside the request would mean the host never sees its own reply.
- **A sleep longer than the watchdog timeout is refused** with a reason
  (`PWR_REF_WDG`) rather than executed: the IWDG keeps counting in standby, so
  that sleep would be a reset in the middle of the night.
- **Standby releases the USB pull-up first** (`PWR_CAP_DETACH`): the host sees a
  clean disconnect and a fresh enumeration instead of a device that ignores it,
  and because the serial number comes from the factory id it is recognisably the
  same device coming back.

## Protocol

| command | direction | meaning |
| ------- | --------- | ------- |
| `0xa0` `PWR_ARM` | OUT data stage, 8 bytes | `{duration_ms, mode, wake, detach, delay_ms}`; refuses out of range values, an unknown mode or wake source, a second request while one is armed, and anything longer than the IWDG timeout |
| `0xa1` `PWR_GET_STATE` | IN data stage, 28 bytes | `{sleep_count, awu_ticks, requested_ms, nominal_ms, caps}` + the last refusal, mode, wake reason, detach flag, armed flag, AWU divider |
| `0xa2` `PWR_GET_INFO` | IN u32 | `(max ms << 16) \| min ms` = `(30000 << 16) \| 10` |

There is no kernel child: a sleeping device is *not on the bus*, so a kernel
driver that put it to sleep would be fighting its own USB stack.  A host command
and a test script are the honest interface for now (a sysfs control on the MFD
core, or runtime PM with the semantics thought through, are the two candidates if
one is ever wanted).

## Measured

`scripts/pwr_test.py`, five clean runs plus the opt-in cases:

| check | result |
| ----- | ------ |
| 100 / 300 / 1000 ms sleep, AWU backstop, mode sleep | 50 × 2 ms = 100, 37 × 8 ms = 296, 62 × 16 ms = 992 ms nominal; wake reason AWU; no reset |
| standby + detach, 500 and 2000 ms | device leaves the bus and comes back: 968 ms and 2469 ms from arming |
| the same two points fitted | the sleep runs at **0.992 of nominal**, i.e. an LSI of about **127 kHz** against the nominal 128 kHz, with a fixed **468 ms** for the host to see the device again (re-enumeration) |
| a poke during a 5000 ms sleep | ends early (answered after 468 ms) - the host's traffic wakes it |
| 10 sleeps in a row | counted 10, `tx`/`rx` paths unaffected afterwards |
| out of range duration, unknown mode, unknown wake source, second request | refused, with the reason in `last_refused`, and no sleep |
| IWDG armed at 2000 ms + a 5000 ms sleep | refused (`PWR_REF_WDG`); letting the watchdog bite resets the chip (reset cause `WDG`), the state is fresh and the device is usable again |
| the boot button during a 20 s standby sleep | pressed: the device came back after 10.5 s with reason **button** |

## What a standby wake does to the other modules

Standby keeps the register file, so a module's own registers survive - and the
UART, SPI, I2C, PWM, ADC and GPIO all keep working across a sleep, which
`scripts/combo_test.py` checks by running one round trip per module before and
after one.  What does *not* survive is the **RCC configuration**: the wake path
calls `SystemInit()` to get back to 48 MHz, and that rewrites `RCC->CFGR0`.  For
this firmware that means the ADC's prescaler (ADCPRE) goes back to its reset value
- measured: a conversion took 42 us before a sleep and 11 us after it, because the
ADC clock went from 6 MHz to 24 MHz.  The ADC module now writes ADCPRE before
every conversion.  The other modules are unaffected: a UART's BRR, an SPI's
prescaler, a timer's PSC and an I2C's bit-banged timing all derive from registers
that survive (the bit banger counts SysTick, whose rate is restored with the
clock).

## What does not work, and why

Four attempts, all measured, all written down because each one cost a build:

- **The button as an EXTI *event*** (the first attempt): it wakes the core, but an
  event-only line does not latch `EXTI->INTFR`, so the firmware could not say who
  had woken it - a 20 s standby sleep ended early when the button was pressed and
  still reported AWU.  The fix is the hook upstream provides for exactly this:
  `RV003_ADD_EXTI_MASK`/`RV003_ADD_EXTI_HANDLER` (set in `vendor/usb_config.h`)
  teach rv003usb's `EXTI7_0` handler that line 6 is not the USB line, so the
  button can be a real interrupt and our `pwr_button_edge()` gets called instead
  of the USB state machine being fed a phantom packet.  The USB path was re-tested
  afterwards (`v003`, `uart`, `adc`, SPI loopback, stress: all pass).
- **"The host talked to us during the sleep"** is not reportable.  The USB handler
  consumes the EXTI pending bit before the module can read it (attempt one), and
  the USB stack's own activity marker `se0_windup` moves on *any* bus traffic -
  including the SOFs a host sends to a device that is merely attached - so
  comparing it before and after reports "USB" for every sleep (attempt two,
  measured: every quiet sleep came back as USB activity).  The protocol therefore
  has no such field: a host sees the early answer and knows why.
- **A peripheral timer cannot measure a sleep.**  TIM2 was set up to count
  milliseconds for exactly this, and it does while the core runs (the ~10 ms start
  delay reads back), but it does not advance during a 500 ms sleep: in sleep mode
  the core clock stops and takes the counter with it.  The measurement moved to
  the host, where it is a wall clock - see the fitted 0.992 above.
- **UART activity as a wake reason** is implemented (a byte in, or the
  transmitter draining its own ring - in sleep mode *any* enabled interrupt ends
  the sleep) but could not be produced on this bench: sending a byte *in* needs a
  second UART device on PD1, and the only thing wired there is the loopback jumper
  to PD0.  `scripts/pwr_test.py --uart` tries anyway and skips with that
  explanation.

## Cost

**1128 bytes of flash and 44 bytes of RAM** against the same build without it
(`TRACE=0`: 11376/1240 -> 12504/1284), which makes it the largest module after
the UART - the AWU divider table and the two sleep paths are most of it.  The
default build is 12740 bytes of flash (77.8 %) and 1424 bytes of RAM, with
**352 bytes of stack margin** measured through `V003_GET_STACK_FREE`.  Keeping that margin above the ~350 byte rule
meant shrinking the debug UEvent ring from 4 to 2 entries (`make UEVENTS=4` brings
it back) and, when the combined-module test showed the margin at 308 bytes under a
real workload, turning the I2C clock tracer off by default (`make TRACE=1`) - both
are debug facilities, which makes them the first things to give.

## Open

- A sleep does not quiet the peripherals it does not need.  RM 2.3.2 is right that
  every clock left running costs current, but on this bench the power cannot be
  measured (the board is powered over USB and there is no current meter), and
  turning off a module the host configured would silently break it.  A host that
  wants a quiet device disables the modules it is not using.
- The `wake` mask has one bit today (the boot button).  Any other pin the board
  leaves free (PC3, PD2) could join it with the same EXTI hook, and a second
  available pin would also make the UART wake testable.
