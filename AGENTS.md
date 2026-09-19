# AGENTS.md

Rules for anyone (human or agent) changing this repository. They exist because
this device punishes guessing: it has 2 KB of RAM, a bit banged USB stack, and
several failure modes that look like something else entirely.

`README.md` = how to build and use. `TODO.md` = task state and what was verified.
`notes/` = why things are the way they are. Read `notes/README.md` first when a
task is not obviously mechanical.

## 1. Git

- **Do not run git write commands** (`commit`, `add`, `reset`, `checkout`,
  `branch`, `stash`, ...) unless the user explicitly asks for that exact action in
  that turn. Work is left in the working tree; committing is the maintainer's
  call.
- Read only git commands (`status`, `diff`, `log`, `show`) are fine, and are the
  right way to see what has changed.

## 2. Language

- Reply to the user in **Chinese**.
- Repository documents, code comments and commit messages are in **English**,
  matching the existing files.

## 3. Bench and environment

- One CH32V003 USB dev board plus a WCH-LinkE programmer is assumed connected.
  Test hardware varies (a PC6<->PC7 jumper, an AT24C256 on I2C, a BH1750 on
  I2C). **Probe for it instead of assuming it**: the SPI loopback self test
  reports a mismatch when the jumper is missing, the I2C self test reports
  `-ENXIO` when nothing answers.
- Firmware builds need the xPack toolchain on `PATH`:
  `export PATH=/home/roym/.local/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin:$PATH`.
- Host tests use `.venv/bin/python` (pyusb). New tests must not add dependencies
  beyond pyusb and the standard library (`ctypes` for uAPIs).
- `sudo` is password protected and approval prompts are disabled. The existing
  NOPASSWD grants (`insmod`, `rmmod`, `dmesg`) are the whole toolbox: **use them,
  do not widen them, and ask the user to run anything else** rather than
  pretending it can be done. `/dev/gpiochipN` and `/dev/i2c-N` are reachable
  without root.

## 4. Firmware rules

These are hard limits, not preferences. Numbers and evidence:
`notes/firmware.md`, `notes/usb.md`.

- **Everything in the USB interrupt must finish in ~4.5 us.** Only move bytes
  there: copy OUT payloads into a queue, drain the EP3 IN FIFO, answer control
  IN requests. Commands execute in `main()`. If a handler needs to do work, queue
  it and give the host a way to see when it finished.
- **Never block `main()` for long.** Anything that blocks it (blocking `printf`
  over the debug channel, a long spin) stops the requests the main loop owes the
  host - including zero length control OUTs such as `SET_FRAME_MODE`, which then
  silently never take effect. Debug printing stays off by default
  (`make DEBUG_EVENTS=1`).
- **Check the RAM budget after adding any buffer.** The stack grows down into
  the statics, so an overrun corrupts variables instead of faulting. Read the new
  `_ebss` from `vendor/vendor.map` (or `nm vendor.elf`), keep at least ~350 bytes
  of stack free, and re-measure with `V003_GET_STACK_FREE` (0x3b) on hardware.
- **A result a host reads must carry a completion tag or follow a documented
  ordering.** Data stages are executed after the transfer returns; see
  `notes/i2c.md` for the failure mode and `V003_I2C_GET_RESULT` for the pattern.
- **Queues drop the new item when full and count the drop** (`GET_*_DROPS`,
  `GET_FRAME_STATS`). Never move the other side's index and never drop silently.
- **Reset endpoint toggles on `SET_CONFIGURATION`** (`usb_reset_endpoint_toggles()`).
  Removing that reintroduces a silent, permanent endpoint death.
- Keep the protocol mirrored in the three places it lives: `vendor/*.h`,
  `kernel/usb-mfd.h`, and the host scripts. Adding a command means updating all
  three (and `TODO.md`).
- `rv003usb/` is vendored but patchable. Keep patches minimal and
  upstream-compatible: guard new knobs with `#ifndef` and keep the upstream
  default (`RV003USB_NUMUEVENTS` is the model).

## 5. Kernel driver rules

- Structure follows `drivers/mfd/dln2.c`: `usb-mfd.ko` is transport + MFD core,
  every function is a child platform driver. New functions get an MFD cell, not
  code in the core.
- Children use `v003_cmd_out()` / `v003_cmd_in()` (transport agnostic) and
  `v003_data_*()` / `v003_frame_*()` where a payload is involved. Do not reach
  into the core's structs.
- Load order matters (children need the core's exported symbols); say so in any
  instructions you write.
- **No USB transfer buffer may live on the stack.** `usb_control_msg()` rejects
  it with `transfer buffer is on stack` and EAGAIN. Use the core's bounce buffers
  or `kmalloc`.
- Kernel coding style, `LLVM=1` is auto-detected on clang built kernels. Modules
  must build with no warnings.
- Unload the modules before any pyusb test: the interface is claimed by the
  driver and pyusb cannot open it (it reports `Resource busy`).

## 6. Tests and verification

- `scripts/` = host side protocol tests over raw USB (pyusb), `tests/` = userspace
  uAPI tests (GPIO character device, i2c-dev). Put a new test where its peers are.
- `tools/` = the bench instruments (state dashboard, pin probe, UART jumper check,
  SWIO recovery, build / module / whole-suite runners). Look there before writing
  another one-off debugging script, and run `tools/run_tests.sh` for both suites
  in the order they require - it also fails the run when dmesg warns about it.
- **Verify on hardware and report measured numbers**, with the command that
  produced them. "It compiles" and "the test passed" are not evidence that the
  feature works.
- **Check the cause, not just the value.** A passing test that went through a
  fallback path is worse than a failing one: assert on the counters that prove
  the path was taken (`SPI_GET_STATS`, `GET_EP_STATS`, `GET_FRAME_STATS`).
- Endpoint level tests start by sending `SET_CONFIGURATION` (toggle resync) and
  leave the device in raw echo mode (frame mode off).
- Tests must establish their own state and restore what they touch (the I2C test
  writes its own marker and puts the EEPROM page back).
- Run a new test at least three times: everything here has been flaky at least
  once.
- Do not hide exit codes behind pipes (`... | tail`); check the status.
- When a bug is fixed, add the check that would have caught it and write the case
  up in `notes/debugging.md`.

## 7. Documentation obligations

- Update `TODO.md` in the same turn as the work: mark what is verified on
  hardware (with the numbers), and record what is open, blocked or deliberately
  parked.
- Durable findings go into `notes/`: measured limits, protocol details, and
  failure modes. If a new measurement contradicts a note, fix the note in the
  same turn - a wrong note is worse than no note.
- `README.md` is usage only: keep protocol internals and debugging stories out of
  it.

## 8. Honesty

- Correct earlier statements, including your own, in the response where the
  correction is relevant. If an earlier claim was wrong, say so and say what the
  measurement is now.
- Do not present a plan, a hypothesis or a guess as a result. Label them.
- Report failures and their symptoms verbatim (error numbers, dmesg lines,
  register values): those are what make the next debugging session short.
