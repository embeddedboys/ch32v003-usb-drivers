# tools/

The commands this project kept retyping.  Every one of them exists because
something on this bench failed in a way that looked like something else, and the
only way to tell the two apart was to measure: a chip select that had silently
become an input, a UART that "did not work" because a jumper was missing, a
programmer that could not attach because the firmware was holding SWIO.

They are not tests - the tests live in `scripts/` (vendor protocol over pyusb) and
`tests/` (kernel uAPIs).  These are the instruments: they report what the hardware
and the kernel are doing, they change as little as possible, and where they must
change something they say so.

| Tool | The question it answers | When to reach for it |
| ---- | ----------------------- | -------------------- |
| `status.py` | what state is the device in, right now | first thing to run: firmware version, capabilities, reserved pins, stack margin, ADC/WDG/PWR state, endpoint and drop counters |
| `pin_probe.py` | is this pad bonded and does it drive what we tell it to; which pin is this button on | bringing up a new module, or a pad that reads something unexpected |
| `uart_jumper_check.py` | is "nothing came back" the PD0<->PD1 jumper or the firmware | the UART receive path fails and the counter deltas need attributing |
| `free_swio.py` | why can `minichlink` not attach | `nothing connected to linker` / `HARTINFO: ffffffff` after using the UART |
| `build.sh` | build and flash, and where do flash and RAM stand | after any change to the firmware; it finds the xPack toolchain itself |
| `modules.sh` | load or unload the kernel drivers, and did it work | before/after every driver-side or pyusb-side test |
| `run_tests.sh` | does the whole thing still work, on both sides | before a commit, and after any change that touches two modules |

All of them are run from the repository root:

```
.venv/bin/python tools/status.py --pins
.venv/bin/python tools/pin_probe.py --watch --seconds 60 --pins 54
.venv/bin/python tools/uart_jumper_check.py
.venv/bin/python tools/free_swio.py
tools/build.sh                      # build and flash the default firmware
tools/modules.sh load               # core first, then every child
tools/run_tests.sh --flash          # flash, then the whole suite
```

`status.py`, `pin_probe.py`, `uart_jumper_check.py` and `free_swio.py` talk the
vendor protocol over pyusb, so **the kernel modules have to be unloaded**
(`tools/modules.sh unload`): while they are loaded the interface is claimed by
`usb-mfd` and pyusb reports `Resource busy`.

## What each one cost to learn

- **`status.py`** exists because a session's worth of pyusb one-liners kept
  getting the same two things wrong.  `GET_STACK_FREE` (0x3b) answers with the
  *cached* value and arms a refresh, so a single read reports the previous
  measurement - the tool reads it twice on purpose.  And the reserved pin mask is
  a 64 bit number that is much easier to read as `PA1, PC1..PC2, PC5..PC7,
  PD0..PD6` with `--pins`.
- **`pin_probe.py`** comes from the afternoon the ADC module was written: it
  configured PC4 as an analog input, which turned the SPI chip select into an
  input, and the chip select then stopped moving *silently*.  Reading the pin's
  direction back through the GPIO module (`GPIO_GET_DIRECTION`, 0x0a) is how that
  was found, and it is what the probe automates.  `--watch` is the cheap version
  of a logic analyser: it is how the boot button on PD6 was located, and with
  `--keep-mode` it samples a pad that a module is driving without taking it away.
- **`uart_jumper_check.py`**: the receive path needs PD0 jumpered to PD1, which
  is also SWIO, so "nothing came back" has two completely different causes.  It
  looks at the pads as plain GPIO first (no driving - PD1 is SWIO) and then does
  one slow loopback with the counter deltas that say who moved what.
- **`free_swio.py`**: with the jumper in place and the UART port enabled, the
  firmware drives PD0 as an idle-high output, which through the jumper holds SWIO
  and makes `minichlink` fail.  Disabling the port over USB releases both pads;
  the tool verifies the mode changed instead of trusting the request.  Measured:
  with the port enabled, `make -C vendor` fails with `link error, nothing
  connected to linker (4 = [81 55 01 01])` and `HARTINFO: ffffffff`; after
  `tools/free_swio.py` the same command reports `Image written.`
- **`build.sh`** replaces the `export PATH=...` + `make` + "how much RAM is left"
  sequence.  It reports the numbers that matter here: flash against 16 kB, and
  `_ebss` against the 0x20000800 stack top, because the stack grows down into the
  statics and an overrun corrupts variables instead of faulting.
- **`modules.sh`** replaces `make -C kernel test`, which appends `|| true` to
  every `insmod` and therefore cannot fail.  It also handles the two traps: the
  ADC child needs the IIO core (on Arch kernels `industrialio` only exists as
  `industrialio.ko.zst`, which `insmod` cannot read - so it is decompressed to
  `/tmp` first), and the children have to be loaded after the core that exports
  their symbols.
- **`run_tests.sh`** enforces the order the two halves require and checks dmesg
  for the lines the tests cannot see.  The TTY driver that freed an embedded
  `tty_port` passed every test and showed up only as `list_add corruption` in
  dmesg, which is why a run with warnings is a failing run.

## Still in `scripts/`

Two instruments did not move here because they are test-shaped and live with
their peers:

- `scripts/stress_test.py` - hammers one transport at a time to find the traffic
  that wedges the device; it can leave the chip dead, so it is not in the default
  suite (recover with `scripts/wdg_test.py --reset 400`).
- `scripts/i2c_trace.py` - the clock trace, which needs `make TRACE=1` (the
  tracer costs 136 bytes of RAM the default build cannot spare).
