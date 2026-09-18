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

### 6. Test harness bugs that looked like device bugs

- Reading a `GET_CTRL_OUT_SEQ` baseline before the pending increment landed made
  every result look one transaction stale. Settle first, then sample.
- The clock tracer logged phantom SCL edges (one when SCL was already high),
  which shifted every decoded byte, and arming it was not confirmed, so a
  previous trace was read back. Both were tooling, not the device.
- A malformed response (no payload) was parsed as `value = 0`, so a failing check
  looked like a plausible reading. Always assert `result == OK` before a value.
- The I2C "addressing is broken" scare was a control-OUT data stage race: a
  single shared buffer was overwritten by the interrupt while `main()` was still
  working from it, and a slot reused for a same-length request looked complete
  before its payload arrived. The fix is the two-slot queue with the completion
  marker cleared when the slot is claimed.

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
