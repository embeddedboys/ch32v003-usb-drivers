# Debugging: tooling, case studies, traps

Everything here was paid for. Read it before starting a debugging session on
this device.

## Tooling

| Tool                                   | Use                                                                 |
| -------------------------------------- | ------------------------------------------------------------------- |
| `vendor/` `make`                       | builds and flashes over the WCH-LinkE (`minichlink -w ... -b`)        |
| `minichlink -G`                        | GDB stub on port 3333 (owns the link while running, can wedge mid-halt) |
| `minichlink -T`                        | terminal for the firmware's `printf` (`make DEBUG_EVENTS=1`)         |
| `sudo dmesg`                           | the driver's view: errors, warnings, traces                          |
| `sudo insmod` / `rmmod`                | module load order matters; unload before any pyusb test              |
| `.venv/bin/python scripts/*.py`        | host side protocol tests over raw USB                                |
| `.venv/bin/python tests/*.py`          | uAPI tests (GPIO character device, i2c-dev)                          |
| `GET_STACK_FREE` / `GET_TIMING`        | firmware self measurement (stack headroom, interrupt budget)         |

Privileges on this bench: `insmod`, `rmmod`, `dmesg` are NOPASSWD;
`/dev/gpiochipN` is `plugdev`; `/dev/i2c-N` needs the udev rule in the README.
Anything broader is worth asking for explicitly rather than assuming.

## Case studies

### 1. Silent memory corruption: the stack grows into the statics

*Symptom*: `GET_EP_STATS` returned nonsense (`ep_rx_count[1] = 0x20000064`, a
pointer looking value), the EP data path stopped working, and later a specific
vendor request started failing with EIO while every other request was fine.

*Cause*: 164 bytes of stack. `.bss` ended at `0x2000075c` and the stack starts at
`0x20000800`, so a slightly deeper call chain - the USB interrupt nesting inside
`main()` - overwrote the *end* of `.bss`, which happened to be `ep_rx_count` and
the EP3 IN FIFO.

*How it was found*: the linker map (`_ebss` / `_eusrstack`), then a canary that
paints free RAM at boot and reports the deepest excursion.

*Fix*: shrink the rv003usb UEvent ring (512 bytes for a debug channel), 16 byte
EP buffers, share the EP3 IN FIFO with the frame responses. `.bss` now ends at
`0x200004f4`, and the deepest stack use measured afterwards is 272 of the 780
bytes available.

*Lesson*: on this chip, "one variable has a weird value" means "measure the
stack", not "that code is wrong". And: a diagnostic that runs in the interrupt
can itself be the bug - the first canary used `__builtin_frame_address(0)`,
which is 0 without a frame pointer, so it painted 512 MB of address space and
wedged the chip.

### 2. Endpoint data toggles: successes that move no data

*Symptom*: `ep2.write()` succeeded, `GET_EP_STATS` showed the EP2 receive counter
frozen, frame statistics stayed `handled 0, dropped 0`, nothing on EP3 IN. Every
control transfer worked. The first test run after a reflash passed; everything
after it failed.

*Cause*: rv003usb drops OUT packets whose data toggle does not match, and does
not advance the toggle on a mismatch, so the endpoint stays dead forever; it also
never resets toggles on `SET_CONFIGURATION` while the host restarts endpoints at
DATA0. A fresh boot worked because device and host both started at DATA0.

*How it was found*: bisecting which test step killed it (a checkpoint probe
showing `ep2rx +0`), then reading rv003usb's OUT handler - the "acknowledge and
throw away" branch is three lines long.

*Fix*: firmware resets all endpoint toggles on `SET_CONFIGURATION`; tests send
`SET_CONFIGURATION` before their first endpoint transfer.

*Lesson*: a transfer that "succeeds" proves nothing about the data path. Counters
that do not move are the tell.

### 3. Stale results: reading a status before the work happened

*Symptom*: the kernel I2C driver reported `-ENXIO` for a device that was plainly
there, only sometimes.

*Cause*: a control-OUT data stage is answered by the interrupt before `main()`
executes it, so the status read returned the previous request's status.

*How it was found*: logging the raw status and re-reading it - `status 0x2`
(address NACK, left over) against `0x1201` (OK, 18 bytes).

*Fix*: `V003_I2C_GET_RESULT` puts the main loop's request counter in the top
byte; the driver polls until the tag changes. Re-reading the plain status is not
enough, because identical failures are indistinguishable.

### 4. Finding the interrupt budget

*Method*: a command that spins `wValue` iterations and returns the SysTick delta
(`V003_GET_TIMING`). 10 and 12 iterations answered, 13 and up failed with EIO.
That is how "~4.5 us" stopped being a guess, and why all real work moved to
`main()`.

### 5. A blocked main loop disables features silently

*Symptom*: the framed path worked once after a reflash and never again; the frame
stats showed the request was never even received.

*Cause*: `make DEBUG_EVENTS=1` style printing of the UEvent ring over the
bit-banged debug channel blocks `main()` for milliseconds per event. Zero length
control OUTs (`SET_FRAME_MODE`) are queued for `main()`, so frame mode was never
enabled - and EP2 OUT then fell back to the raw echo path, which the test parser
rejected as malformed.

*Fix*: debug printing is off by default.

### 6. The ADC module that reported 294 mV for a 1.2 V reference

The first ADC attempt converted inside the control-IN handler, and every request
failed with `EIO` - a conversion plus the calibration a channel change needs is
tens of microseconds against a ~4.5 us interrupt budget, the same signature as
every other overrun handler (case 4). The module was restructured into an OUT
request that records the channel plus an IN request that reads the result back
with a completion tag, and then it returned *numbers*: the internal reference
read 365 counts, which is 294 mV at 12 bits and 1176 mV at 10 bits.

It was the second one. RM 9.1 calls the converter a 10 bit SAR and RM 9.2.2 says
the regular data register holds a 10 bit value, so the whole module had been
scaled by 4. The value was correct the entire time; only the interpretation was
wrong, and the wrong interpretation was the one that looks like a hardware
problem ("the reference is 4x low" sends you to look at wiring and sample time).

*Check that would have caught it*: `V003_ADC_GET_INFO` now reports the resolution
the firmware actually converts with, and `scripts/adc_test.py` asserts it against
the scaling it uses for the millivolt conversion, so the two cannot drift apart
silently. The general rule is in [adc.md](adc.md).

### 7. A request with no dispatch case is invisible from the host

The ADC's control-IN case was added to the switch in
`usb_handle_control_out_request`-shaped form but the OUT side never was, so
`V003_ADC_START` fell through to `default:` and did nothing. The host saw: a
successful control transfer (rv003usb ACKs the SETUP, the request is queued, and
the queue is drained by `main()`), a zero return value, and a reading that never
changed. Nothing in the transfer itself says "ignored", and the capability report
said the module was there, so the reading looked like a hardware fault.

*Check that would have caught it*: the module counts the requests it was handed
and the conversions that ran, and `V003_ADC_GET_STATUS` (0x84) reports both. The
first reading of the module's own counters showed `requests=18,
conversions=17` - one rejected channel - and that number being non-zero is what
told the difference between "the device is not converting" and "the request
never arrived". Any new module wants that pair of counters.

### 8. A remap field written backwards: "the UART sends nothing"

Setting up USART1 on this board means not using the default pin mapping (PD5/PD6
are the USB pull-up line and the boot button), so the module writes the remap
field.  RM 7.3.2.1 describes it as two bits spread over one register, written
`[bit21, bit2]`: value 01 selects TX/PD0, RX/PD1, and value 10 selects the default
pair again.  The first version set bit21 and cleared bit2 - the value read
backwards - which silently selected remap 10, left PD0 as an undriven pad, and
left the boot button line as the transmitter.

Symptom: the host got a successful control transfer, a "bytes sent" counter that
moved by exactly the payload length, and a receiver that never interrupted.  Two
things made it obvious in the end:

- reading the transmit *pin* through the GPIO module: in remap 01 with the
  transmitter enabled PD0 idles **high** (it is a push-pull alternate function
  output).  The first version read 0;
- the counter that says a module did something.  `V003_UART_GET_COUNTS` moved
  even though nothing arrived, which separated "the queue is broken" from "the
  transmitter is not on the pin I think it is".

*Check that would have caught it*: assertion on the transmit pin's idle level
before blaming the receiver.  A GPIO read is one control transfer.

### 9. Taking the board's own debug pin, and locking the programmer out

UART receive needs PD1, and on this part **PD1 is SWIO** - the single wire the
WCH-LinkE uses to program the chip.  Two consequences, both measured:

- PD1 cannot be pulled low by the firmware.  Driving it low as a GPIO output
  still read back high, and an input with the internal pull-down selected did too;
  the programmer holds the pin.  That is why the UART loopback test needs a jumper
  between PD0 and PD1 (the same pattern as the SPI test's PC6<->PC7 jumper);
- with that jumper in place, a firmware that drives PD0 (an enabled UART
  transmitter, or simply a pin inventory test that ends with a pin driven high)
  holds the SWIO line and `minichlink` can no longer talk to the chip:

      link error, nothing connected to linker (4 = [81 55 01 01]).
      Trying to put processor in hold and retrying.

  Recovery without touching the hardware: arm the watchdog over USB and stop
  feeding it - the chip resets, nothing drives PD1 at boot, and flashing works
  again.  `scripts/wdg_test.py --reset 400` does exactly that, and it is worth
  knowing before a debugging session ends with a board that "cannot be flashed".

*Fix that keeps it from happening twice*: the UART module puts both pins back to
floating input when it is disabled, and `scripts/uart_test.py` disables the port in
a `finally:` block, so neither a failing check nor an exception can leave the
board unflashable.

### 10. A single producer/consumer ring with the wrong emptiness test

A ring of N bytes holds N-1 entries: the producer refuses to enqueue when
`next(head) == tail` (that is *full*), and the consumer stops when `tail == head`
(that is *empty*).  The UART transmit interrupt had the full test in the empty
place:

```c
	u8 next = (u8)((tx_tail + 1) & TX_MASK);
	if (next == tx_head)      /* this is the FULL condition */
		USART1->CTLR1 &= ~USART_CTLR1_TXEIE;
	else { ... }
```

With an empty ring (head == tail) `next != head`, so the interrupt treated the
ring as non-empty, pushed a stale entry and advanced the tail - the ring then
looked 31 entries deep.  Symptom: a byte lost between the queue and the line,
`tx_bytes` counting a byte the line never carried, and a middle byte of a test
pattern missing at four different baud rates while every error counter stayed at
zero (no overrun, no framing error - the receiver simply never saw it).

*Check that would have caught it*: the loopback test asserts that the number of
bytes the transmitter counted equals the number of bytes that came back, and that
the receive interrupt ran once per byte.  Both disagreed by one.

### 11. Test harness bugs that looked like device bugs

- Reading a `GET_CTRL_OUT_SEQ` baseline before the pending increment landed made
  every result look one transaction stale. Settle first, then sample.
- The clock tracer logged phantom SCL edges (one when SCL was already high),
  which shifted every decoded byte, and arming it was not confirmed, so a
  previous trace was read back. Both were tooling, not the device.
- A malformed response (no payload) was parsed as `value = 0`, so a failing check
  looked like a plausible reading. Always assert `result == OK` before a value.
- A drain-until-empty read loop is wrong for a slow line: at 9600 baud a byte
  takes 1.04 ms while a USB control transfer takes about 3 ms, so the ring is
  empty between bytes and the loop returns after the first gap.  The UART test
  reads until it has the number of bytes it asked for (or times out), and prints
  what it got.
- The same test first assumed a 32 byte transmit ring holds 32 bytes and wrote
  its whole pattern in one call: the ring holds 31, the 32nd byte was dropped
  (correctly, and counted) and the test failed.  A host has to flow-control
  writes through `tx_queued`; the test now does, which also exercises the path.
- The ADC test compared a re-read value against a reading taken *before* the
  CALVOL and PWM exercises, and failed when 365 arrived instead of 363. The
  device was right (an ADON cycle and a recalibration later, one least
  significant bit is noise); the expectation was stale. Compare against a value
  sampled immediately before, or against a range - and never against a number
  from an earlier phase of the same test.
- `GET_STACK_FREE` (0x3b) returns the *cached* value and arms a refresh, so the
  first read after a gap answers with what the last read measured. Reading it
  once returned `0` (nothing measured yet) and looked like a stack overrun;
  reading it twice returned the real number (408 bytes free at the time). Same
  shape as the stale I2C status
  in case 3: a self measurement a host reads has to be armed and then read.
- The I2C "addressing is broken" scare was a control-OUT data stage race: a
  single shared buffer was overwritten by the interrupt while `main()` was still
  working from it, and a slot reused for a same-length request looked complete
  before its payload arrived. The fix is the two-slot queue with the completion
  marker cleared when the slot is claimed.

### 12. One unexplained wedge: the device stopped enumerating

Seen once, after a session that loaded the whole kernel stack (`usb-mfd.ko` plus
the six children, the ADC one among them), ran the userspace tests against it,
and then removed the modules.  The next pyusb run failed with `EIO`, and the host
log said

    xhci_hcd 0000:05:00.3: Trying to add endpoint 0x1 without dropping it.

repeated once per attempt, then

    usb 1-2: device not accepting address 70, error -71
    usb 1-2: Device not responding to setup address.
    usb usb1-port2: unable to enumerate USB device

so it ended with the device not enumerating at all, i.e. the firmware side was
stuck too.  **What fixed it**: flashing the firmware again over SWIO, which resets
the chip; the device came back and every test passed.  Note that the software
reset used elsewhere in these notes (`V003_WDG_START` with no further feed) is no
help here, because it needs the USB link that is broken.

**It did not reproduce**: the same session was repeated twice afterwards, once
minimal (`usb-mfd` + `v003-adc`, the IIO test, `rmmod`, pyusb) and once with all
six children and all the userspace tests, and both were clean.  So the cause is
not known - a stale xhci endpoint slot, a port reset storm, or something the
firmware did under driver control, in that order of likelihood.  Written down
because "the board cannot be flashed and does not enumerate" is exactly the state
that costs an hour when it is not in the notes.

## Traps worth remembering

- **A passing test can pass for the wrong reason.** Verify the cause: byte
  counters (`SPI_GET_STATS`), receive counters (`GET_EP_STATS`), frame stats. The
  SPI loopback test checks `GET_STATS` precisely so a fallback to the echo path
  cannot pass.
- **Device state survives a process exit**: endpoint toggles, frame slots, the
  EP3 IN FIFO, and whatever an earlier test wrote to the EEPROM. Tests should
  establish their own state instead of assuming a fresh device - the I2C test
  writes its own marker and restores the page it used.
- **`cmd | tail -n` hides exit codes** and once produced a commit message about a
  failing test. Check the exit code, not the last line of output.
- **Dropped work must be counted.** Every queue in the firmware counts what it
  dropped (`GET_*_DROPS`, `GET_FRAME_STATS`); without those counters a dropped
  request is indistinguishable from a protocol error.
- **Measure before optimising.** The endpoint protocol was expected to beat
  control transfers and does not; the numbers are in
  [frame-protocol.md](frame-protocol.md). The same measurement discipline found
  the 2.3x win of `MEM_WRITE_READ` and the ~326 us AT24C256 write cycle.
