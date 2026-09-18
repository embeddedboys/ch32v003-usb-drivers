# UART

`vendor/uart.c`, module id `0x07`, commands `0x90`-`0x9a`. USART1, remapped to
**PD0 (TX)** and **PD1 (RX)**, a 64 byte receive ring filled by the interrupt and
a 32 byte transmit ring it drains.

## Which pins, and why not the default ones

RM 7.3.2.1 gives four mappings (AFIO_PCFR1, the field is `[bit21, bit2]`):

| remap | TX | RX | usable here? |
| ----- | -- | -- | ------------ |
| 00 (default) | PD5 | PD6 | no - PD5 drives the USB pull-up, PD6 is the boot button |
| 01 | PD0 | PD1 | **yes**, and it is what this module uses |
| 10 | PD6 | PD5 | no - same two pins as the default |
| 11 | PC0 | PC1 | no - PC1 is the I2C SDA line, PC0 is the board's LED |

**PD1 is the chip's SWIO debug pin.** The WCH-LinkE holds it, so the receive pin
cannot be used as an input on its own: measured, PD1 reads high with the internal
pull-down selected, and driving it low through the GPIO module still reads back
high.  A **jumper between PD0 and PD1** turns the transmitter into the receiver's
peer and is what `scripts/uart_test.py` needs, exactly like the PC6<->PC7 jumper
the SPI loopback test needs.

### Telling whether the jumper is actually connected

There is a software-only way to check it, which is what the loopback failure
"nothing came back" really needs to be distinguished from a firmware bug: put the
port at the slowest rate it accepts (733 baud, 13.6 ms per byte) and write a few
`0x00` bytes, so the transmitter holds the line low for milliseconds at a time,
then sample **PD0 and PD1 through the GPIO module** (`V003_GPIO_GET`, which only
reads and does not disturb the UART's pin configuration).

Measured with a good connection: PD0 is low in 17 of 20 samples and PD1 follows
it in 14 of them (a pair is two control transfers a few milliseconds apart, so a
sample can land on the stop bit instead - what matters is that PD1 follows at
all).  Measured with the connection broken: PD0 is still low in 16 of 20 samples
while PD1 reads high in *every* sample - the transmitter is doing its job and
nothing reaches the receiver.  That distinction took one measurement and saved a
debugging session that would otherwise have started in the firmware.

The same symptom has a second cause worth checking first, because it is not
physical: the WCH-Link holds PD1 (SWIO) while it is attached, and it holds it
*actively* after a flashing session, strongly enough that PD0 cannot pull the line
low through the jumper.  Then the loopback stops working with no change to the
firmware at all (measured: PD0 low, PD1 high, on a bench where the same binary had
passed three times an hour earlier), and re-plugging the programmer is what fixes
it.  A chip reset does not: the state is on the programmer's side.

### The jumper blocks flashing while the port is enabled

PD0 is an alternate function push-pull output when the UART is configured, and
through the jumper it holds the SWIO line: `minichlink` then reports

    link error, nothing connected to linker (4 = [81 55 01 01]).
    Trying to put processor in hold and retrying.

Measured twice more, in two different flavours:

    link error, nothing connected to linker (4 = [81 55 01 01]).
    Trying to put processor in hold and retrying.
    HARTINFO: ffffffff
    Could not setup interface.

The first appears while the firmware is driving PD0 (an enabled UART
transmitter), the second after host-side pin experiments that drove PD0 low and
high through the jumper.  Releasing the pins is *not* always enough to get the
programmer back - both times what did work was a **reset**:

    .venv/bin/python scripts/wdg_test.py --reset 400   # or any short WDG_START

which only needs the USB link (the watchdog is in the default build): after the
reset nothing has touched PD1 yet and `minichlink` gets in.  Removing the jumper
works too, and is the only option if the chip is wedged hard enough that USB is
gone as well.

So the practical rules are: do not drive PD0 or PD1 in the minute before a flash,
do not run the pin inventory (it drives every pin in turn) right before one, and
when `minichlink` complains, reset the chip rather than retrying the same
command.  `V003_UART_CONFIG` with `enable = 0` still releases both pins - that is
the module being a good citizen, and `uart_test.py` does it in a `finally:` - but
it is not on its own a guarantee that the next flash will work.

## Measured: the full duplex round trip

32 byte pattern `(i * 7 + 1) & 0xff`, written and read back at four rates with a
PD0<->PD1 jumper in place (`scripts/uart_test.py`, three clean runs):

| requested | actual baud | error | bytes back | interrupts |
| --------- | ----------- | ----- | ---------- | ---------- |
| 9600 | 9600 | 0.00 % | 32/32 | 32 |
| 115200 | 115107 | -0.08 % | 32/32 | 32 |
| 921600 | 923076 | +0.16 % | 32/32 | 32 |
| 3000000 | 3000000 | 0.00 % | 32/32 | 32 |

`baud = HCLK / BRR` exactly (HCLK = 48 MHz, RM 12.3), so `BRR = 48000000 / baud`
rounded is the whole calculation, and the read back is what the hardware really
divides to.  921600 is the manual's own rounding example: it says the closest
BRR gives 923076 bps, 0.16 % off, and this device produces exactly that.

The interrupt count is checked per byte on purpose: it separates "the bytes
arrived" from "the bytes arrived and every one of them interrupted".

## Measured: the interrupt does not disturb USB

The receiver runs in an interrupt (`USART1_IRQHandler`, RXNE) because bytes
arrive when they want to and `main()` is busy for milliseconds at a time.  That
is the first peripheral interrupt in this firmware besides the USB one, so it had
to be measured rather than assumed: with the loopback running at 3 Mbps,
**29,696 bytes flowed while 1,961 USB control transfers completed with zero
errors and none slower than 20 ms**.

Why it is safe: interrupts are not nested, so the USB handler is never preempted;
the cost is a delay of at most one UART interrupt (~1 us, it only moves a byte)
if the two arrive at the same moment.  The transmit side is the same interrupt
(TXE) - a synchronous send would have blocked `main()` for 66 ms on a 64 byte
write at 9600 baud, which is an order of magnitude more than the slowest thing
main does today.

## Ring sizes, and what they buy

| ring | size | covers |
| ---- | ---- | ------ |
| receive | 64 B | 5.5 ms at 115200 baud, 2.8 ms at 230400 |
| transmit | 32 B (holds 31) | one 64 byte `UART_WRITE` needs two rounds of flow control |

For comparison, the stalls `main()` can have while another module transfers: a
64 byte SPI transfer is ~6 ms, a 64 byte I2C block read ~15 ms, an ADC
conversion 42 us.  So a UART receiving at 115200 while I2C does a block read
*will* overflow the ring: the byte that does not fit is dropped and counted
(`rx_dropped`).  A host that needs both has to lower the baud or accept the
losses; the numbers are what says so, not a feeling.

A single producer/single consumer ring of N bytes holds N-1 (the head == tail
test cannot tell empty from full), which is why the transmit ring reports at most
31 queued bytes and why `scripts/uart_test.py` flow-controls its writes through
`tx_queued` instead of assuming a payload fits.

## What was tried and does not work

- **HDSEL half duplex (RM 12.5) does not echo.**  The manual says TX and RX are
  connected inside the chip, so a transmitted byte would come back as input and
  the module could test itself with no wiring.  Measured with the remap fixed and
  the transmitter confirmed alive (PD0 idles high, the byte counter moves): zero
  receiver interrupts, zero framing errors, no data, both with the TX pin as a
  push-pull and as the open-drain output the manual asks for.  So the self test
  went away rather than staying in as an unverified feature.
- **Clock a frame in through the RX pin's weak pull.**  The pin can be kept an
  input (so the USART's receive path still sees the pad) while the output data
  register selects pull-up or pull-down (RM 8.x: "1 = pull-up input, 0 =
  pull-down input").  It reads high either way on this board, so there is nothing
  to clock with; the jumper is the answer.

## Cost

1292 bytes of flash and 128 bytes of RAM against the same build without it
(`TRACE=0`: 10036/1176 -> 11328/1304).  The default build is 11552 bytes of
flash and 1408 bytes of RAM, with **376 bytes of stack margin** measured through
`V003_GET_STACK_FREE` - the rings are paid for out of that margin, so 32 bytes
were taken back by shrinking the zero length vendor OUT request ring from 8
entries to 4 (`V003_NUM_SIMPLE_REQUESTS`, drops counted by `V003_GET_REQ_DROPS`).

## Kernel side: a TTY on /dev/ttyV0

`kernel/uart.c` -> `v003-uart.ko` is the other consumer of this module's
protocol: a `tty_port` based TTY, termios mapped onto `V003_UART_CONFIG`, the
receive path polled (the firmware's ring is the buffer, the poll interval comes
from the baud rate) and writes flow controlled by the firmware's transmit ring.
[kernel.md](kernel.md) has the design and the measurements: 32 byte patterns
round trip byte for byte at 9600 and 115200 through the jumper, and one 256 byte
`write()` at 9600 - nine times the transmit ring - completes in the time the wire
needs with every byte coming back.

Closing the TTY disables the firmware port, which is the same "release PD0 and
PD1" behaviour the flashing section above depends on: a host that closes
`/dev/ttyV0` hands the debug pin back.

## Open

- Flow control in the other direction (RTS/CTS) is not wired: the remap puts
  RTS on PC2 and CTS on PC3/PC6, and PC2 is the I2C SCL line, so the TTY driver
  clears CRTSCTS when a caller asks for it.
