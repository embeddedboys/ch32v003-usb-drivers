# TODO

Living task list for the CH32V003 USB drivers (firmware + Linux driver).
Legend: `[x]` done, `[ ]` open, `[~]` in progress, `[!]` blocked.

## I2C / i2c-tools

- [x] `i2cdetect -y N` uses SMBus quick write (or read byte with `-r`) to probe,
  and the adapter only advertised `I2C_FUNC_I2C`, so it answered "Bus doesn't
  support detection commands".  `kernel/i2c.c` now advertises the SMBus
  transfers the firmware can express and lets the i2c core emulate them
  (`0x7f0001`: quick, byte, byte data, word data; the block sizes stay out
  because their emulation needs `I2C_M_NOSTART`).  Verified on hardware:
  `i2cdetect -y 8` and `i2cdetect -y -r 8` both find the AT24C256 at 0x50 and
  nothing else, and each `i2cget` form returns exactly what the equivalent
  `i2ctransfer` sequence returns (read byte - no command - matches the current
  address read; read byte data matches `w1 0xNN r1`; read word data matches
  `w1 0xNN r2`), which is the protocol equivalence that matters - the value
  itself is the device's answer to a one byte command on a two byte word
  address part.  `tests/i2c_dev_test.py` now asserts the advertised set.

## Room for more modules (ADC, PWM, UART)

- [x] Measured the budget instead of guessing: flash is 51.9 % used (7.9 kB free)
      and each peripheral wrapper costs ~0.4-1.0 kB, so three more modules fit
      easily; RAM is the constraint (716 B of statics and 456 B of measured
      stack margin).  Numbers and the per module table are in notes/firmware.md.
- [x] `V003_GET_CAPABILITIES` (0x3d): a 16 byte struct through a data stage
      (append only, so it can grow) with the module mask (`0x47` = gpio, spi,
      i2c, frame), the channel counts, and the pins the device owns.  It is a
      `const` in flash pointed at by the data stage, so it costs no RAM.
      Verified from the host and against the kernel log.
- [x] Compile time module selection: `make MODULES=gpio,spi,i2c` (default),
      `make MODULES=gpio,spi`, `make MODULES=gpio`, `make TRACE=0`, with `#if`
      around the includes, the dispatch cases and the hot path the tracer sits
      in.  Verified by building four variants (8548/1332 default down to
      5624/1028 for GPIO only) and by flashing the GPIO-only build, which
      reports `0x41`, gets one cell and leaves `v003-i2c.ko` idle.
- [x] Kernel: the core builds its child cells from the capability mask
      (name+capability table, dln2 style) and a device without the command is
      treated as the original build; the GPIO child takes the reserved mask from
      the core, so it now refuses pins 16..31 (no port behind them), the I2C and
      SPI pins, and the USB pins.  `reserved=<pin>,...` on the core adds pins the
      *driver* owns (a chip select), and `v003-spi.ko` warns when its `cs_pin` is
      missing from the mask - that closes the SPI chip select item.
- [x] The I2C clock tracer is behind `make TRACE=0` (-224 B flash, -136 B RAM,
      the knob lives in `vendor/i2c.h`).  The UEvent ring size is already a knob
      (`RV003USB_NUMUEVENTS`); exposing it as a Makefile variable is still open.
- [x] PWM (`V003_MODULE_PWM`, `V003_CAP_PWM`): TIM1 channels 1 and 2 on PD2 and
      PA1 (the only two of the four the board has free - the other two are its
      spare GPIO and the SPI chip select), `PWM_SET`/`PWM_GET`/`PWM_GET_INFO`
      taking a period in ns and a duty in permille, 32 bit arithmetic so the
      module costs 592 B of flash instead of 1992 B, and both pins reported as
      reserved.  Verified with `scripts/pwm_test.py`: four periods read back
      within 0.01 %, duty 0 % and 100 % measured on the pins through the GPIO
      module, an out of range channel refused.  The `pwm_chip` child driver is
      done too (`v003-pwm.ko`, npwm from the capability report, `apply`/
      `get_state` in ns and permille) and its `selftest=1` round trips a 1 ms
      period and a 25 % duty through the same callbacks sysfs would use, since
      exporting a channel needs root.
- [x] ADC (`V003_MODULE_ADC`, `V003_CAP_ADC`, module id 0x06, commands
      0x80-0x85): one conversion per request, no buffering, 628 B of flash and
      12 B of RAM.  `V003_ADC_START` is an OUT request (wValue = channel)
      because a conversion takes 42 us - three orders of magnitude over the
      4.5 us interrupt budget - and the first version, which converted inside
      the control-IN handler, made every request fail with `EIO`; `main()` runs
      it and `V003_ADC_GET` returns `(tag << 16) | (valid << 15) | value`.
      Verified with `scripts/adc_test.py` (three clean runs):
      the internal 1.2 V reference (channel 8) reads 365 counts = 1176 mV, the
      internal calibration voltage (channel 9) reads 511 counts = 1647 mV at
      2/4 AVDD and 767 counts = 2472 mV at 3/4 AVDD, both within 5 mV of
      0.5/0.75 x 3.3 V; channel 1 follows PA1 driven by PWM channel 2 exactly
      (1023 counts at 100 % duty, 0 counts at 0 % duty), which is a real
      external input measured without any wiring; one read after START already
      carries the new completion tag; and an out of range channel is counted as
      a request but not as a conversion (`V003_ADC_GET_STATUS` reports
      `err=0 us=42`, and a conversion that times out is published with the valid
      bit clear instead of as a plausible looking 1023 counts).  **The part is 10 bit, not 12** - scaling as 12 bit
      reported the 1.2 V reference as 294 mV, which is the whole story of how
      this module looked broken for an afternoon; see notes/adc.md.
- [x] UART (`V003_MODULE_UART`, `V003_CAP_UART`, module id 0x07, commands
      0x90-0x9a): USART1 on PD0 (TX) / PD1 (RX).  The default mapping is the USB
      pull-up line and the boot button on this board, remap 10 is those same two
      pins, and remap 11 is the LED pin and the I2C SDA line - so remap 01 is the
      only usable one.  A 64 byte receive ring filled by the interrupt and a
      32 byte transmit ring it drains, configuration with its *actual* baud read
      back, and counters for queued/available/dropped plus the receiver's
      isr/overrun/framing/parity.  Verified with `scripts/uart_test.py` (three
      clean runs; needs a PD0<->PD1 jumper because **PD1 is the chip's SWIO debug
      pin** and the programmer holds it): 32 byte patterns round trip byte for
      byte at 9600, 115200, 921600 and 3000000 baud with exactly one interrupt per
      received byte and no error counter moving; the read back baud error is
      0.00 / -0.08 / +0.16 / 0.00 % (921600 -> 923076 is the manual's own rounding
      example); an out of range baud, word length, parity or stop bit count is
      refused as a whole; queueing 128 bytes at the slowest rate reports the
      backlog and counts what did not fit; flooding the receiver fills the ring to
      63 and counts the overflow; flush drops what it held and reports how much.
      Costs 1292 B of flash and 128 B of RAM, so 32 B were taken back by shrinking
      the zero length vendor OUT request ring to 4 entries
      (`V003_NUM_SIMPLE_REQUESTS`); the measured stack margin is 376 B.  **The
      interrupt is the first one in this firmware besides USB**, so it was
      measured against the thing that matters: 29,696 bytes of loopback traffic at
      3 Mbps while 1,961 USB control transfers completed with no errors and none
      slower than 20 ms.  HDSEL half duplex was measured too and does *not* echo,
      so it is not used (notes/uart.md).
- [ ] UART kernel child driver: a UART belongs behind a TTY, which is a bigger
      piece of work than the other children (`tty_port` or `serdev`, a receive
      path that survives an arbitrary reader, termios mapped onto
      `V003_UART_CONFIG`).  The protocol is mirrored in `kernel/usb-mfd.h`; the
      MFD cell is deliberately not added yet.
- [ ] Flashing with the PD0<->PD1 jumper attached fails while the UART is
      enabled: it holds the SWIO line and `minichlink` reports `nothing connected
      to linker`.  The port releasing both pins when it is disabled and
      `scripts/wdg_test.py --reset` are the two workarounds.
- [ ] ADC kernel child driver: an IIO device (one `iio_chan_spec` per channel,
      the two internal channels with a `IIO_CHAN_INFO_SCALE`, `read_raw` mapped
      onto START + GET).  The MFD cell is deliberately not in
      `kernel/usb-mfd.c` yet: a cell without a driver would only show up as an
      unbound platform device.
- [x] Watchdog (`V003_MODULE_WDG`, `V003_CAP_WDG`): `WDG_START`/`WDG_FEED`/
      `WDG_GET_STATE`/`WDG_GET_RESET_CAUSE` in the firmware (WCH's IWDG sequence,
      prescaler picked to fit the 12 bit reload, the *actual* timeout reported
      because the LSI is only good to +-20 %), and `v003-wdt.ko` as a
      `watchdog_device` with `WDIOF_CARDRESET` when the last reset was the
      watchdog.  Verified by letting it bite: `scripts/wdg_test.py --reset 2000`
      arms it, feeds it, stops feeding, sees the device vanish and come back
      reporting `watchdog` as the cause with the watchdog off.  Costs 484 B of
      flash and 8 B of RAM; it is part of the default build now, and the
      capability bit keeps slimmer builds honest.
- [ ] Power management (`V003_MODULE_PWR`, `V003_CAP_PWR` reserved): sleep and
      standby entry, wakeup source configuration, and reporting the wake reason.
      It needs its own design pass because a sleeping device stops answering USB,
      so the core's suspend/resume and the userspace test flow both have to agree
      on how it is entered and left.

## Device identity from the chip

- [x] The ESIG unique id (chapter 15 of the reference manual) is exposed:
      `V003_GET_DEVICE_UID` (0x3c) returns the 12 bytes, `V003_GET_DEVICE_SN`
      (0x31) now returns its first word instead of the placeholder `0x12345678`,
      and the USB serial number *string* is built from it at boot, so every board
      reports its own.  Verified against an independent read through the
      programmer (`minichlink -r ... 0x1FFFF7E8 16`): `cd ab 0b 92 82 bc 5a fa ff
      ff ff ff`, byte for byte what the host sees as `iSerial` and through 0x3c,
      and what the kernel logs at probe (`serial 0x920babcd, unique id
      cdab0b9282bc5afaffffffff`).  Only 64 of the documented 96 bits are
      programmed, so the last word reads blank on this part.
- [x] Cost: 64 bytes of RAM (the serial string, a 12 byte copy of the id and
      alignment) and ~130 bytes of flash; the stack measurement still reports 480
      bytes free afterwards.
- [x] Reading the ESIG from inside the USB interrupt does not work (the host gets
      EIO, the same signature as an overrun handler) - `main()` copies it once at
      boot and everything else uses the copy.
- [ ] The frame path cannot carry the unique id yet: a frame response payload
      tops out at the 8 byte buffer in `v003_frame_poll()`, so `frame_test.py`
      reads it over the control path.  Widening that buffer would also let other
      payload commands use the framed transport.

## USB identity

- [x] One source of truth for the vendor/product id: `lib/v003_usb_ids.h` is
      included by the firmware descriptor and by the kernel driver (kbuild gets
      `-I$(src)/../lib`), and `scripts/v003_usb.py` parses it, so the host tools
      cannot drift.  All twelve scripts now import `VID`/`PID` (or call
      `find_device()`) instead of spelling `0x1209`/`0xC303` out; each of them
      still runs, and the four suites pass.
- [x] Established why `lsusb` prints `Generic`: the vendor id is in the host's
      hwdb (`usb:v1209*` -> `ID_VENDOR_FROM_DATABASE=Generic`, from
      `20-usb-vendor-model.hwdb`), and a known vendor id beats the device's own
      manufacturer string - proven by flashing `VENDOR_ID 0xbeef` and watching
      `lsusb` fall back to `embeddedboys`.  A product specific hwdb drop-in
      cannot override it (a shorter pattern sorts first).  Written up in
      notes/usb-identity.md.
- [ ] Register `1209:C303` at pid.codes: it currently returns 404, while
      `1209:B003` (the bootloader the board is flashed with) is registered.
      Free, requires the open source licence this project already has.
- [ ] After that, add the product entry upstream (`1209  c303  CH32V003 USB
      Bridge` in hwdata's usb.ids) so every machine shows the device by name.
      The vendor field stays `Generic`, that is the shared vendor id.
- [ ] A vendor name of our own (the `lsusb` vendor field reading
      `embeddedboys`) would need a vendor id allocated to embeddedboys - a
      product decision, and then a one line change in `lib/v003_usb_ids.h`.

## Documentation

- [x] `AGENTS.md`: the rules for changing this repository (git discipline, the
      firmware and kernel hard limits, test conventions, documentation duties).
- [x] `notes/`: knowledge base - `firmware.md` (structure, command reference, the
      ~4.5 us interrupt budget, the RAM/stack map), `usb.md` (what low speed
      software USB costs: frame maths, no NAK, data toggle death, the SETUP ACK
      budget), `frame-protocol.md` (wire format and measured performance),
      `i2c.md` (wiring, clock calibration, memory devices, completion tags),
      `adc.md` (10 bit not 12, measured readings, conversion time, the pin
      table gap), `uart.md` (the remap that works here, PD1 being SWIO, ring
      sizing, the measured loopback), `kernel.md` (MFD structure, transports,
      the child drivers), `debugging.md` (tooling, eleven case studies, test
      harness traps).
- [ ] Notes will rot if they are not used: when a measurement or a decision
      changes, the note that describes it has to change in the same turn.

## Build & toolchain

- [x] Document the RISC-V toolchain. The build auto-detects
      `riscv64-unknown-elf-gcc` (distro) and `riscv-none-elf-gcc` (xPack);
      verified with xPack GCC 15.2.0 building `vendor/` for CH32V003.
- [x] Build `minichlink` from `ch32fun/minichlink` and document flashing over
      the WCH-LinkE.
- [ ] The root `Makefile` only builds firmware projects: no `kernel` target,
      no `flash`/`test` convenience targets.
- [ ] `kernel/Makefile` needs `LLVM=1` on clang-built kernels (CachyOS/Arch);
      auto-detect it instead of failing with gcc-only flags.
- [ ] `bear` is documented as a prerequisite but is not in any install list.

## Firmware (`vendor/`, `rv003usb/`)

- [x] Control-OUT data stages are no longer write-only: `GET_CTRL_OUT_DATA`
      (0x36) hands the received stage back to the host, clamped to the length
      rv003usb actually received (so stale bytes are never returned). Verified
      for every length 1..64 on hardware.
- [x] EP data path: EP3 IN TX FIFO served one 8 byte packet per IN token, fed
      by the EP1/EP2 OUT callbacks (echo), with `GET_FIFO_LEVEL` /
      `GET_FIFO_DROPS` for flow control. Verified on hardware.
- [x] `EP3 IN` no longer returns the hardcoded `"Hello!~~"` demo payload.
- [x] OUT callbacks no longer log `data[0]`/`data[1]` unconditionally
      (uninitialised bytes for short packets).
- [x] SPI module (0x02): SPI1 master on PC5/PC6/PC7, `ENABLE`/`CONFIG`/
      `GET_STATE`/`GET_STATS`, EP2 OUT -> MOSI, MISO -> EP3 IN. Verified on
      hardware (byte accounting + MISO sampling with PC7 driven as GPIO).
- [x] SPI control path: `SET_CS`/`TRANSFER`/`GET_RX`/`GET_CS` (0x44..0x47) do
      one atomic, chip select framed transfer of up to 64 bytes through the
      control-OUT data stage. Verified on hardware: bytes clocked, chip select
      levels, clamped read and the 64 byte case. ~5x cheaper than the endpoint
      path for small transfers.
- [x] SPI full duplex verified with a PC6(MOSI)<->PC7(MISO) jumper:
      `spi_test.py --loopback --soak 150` passes both transfer paths with zero
      mismatches (150 x 8 B random on the endpoint path at ~105 round trips/s,
      150 control transfers sweeping lengths 1..64 at ~83/s), each iteration
      also checking `GET_STATS` to prove the bytes really went through SPI
      instead of the echo fallback. An earlier single reading could not be
      reproduced in 200 transfers and is recorded as spurious.
- [ ] Bit order / CPOL / CPHA cannot be verified with a plain MOSI-MISO wire
      (every bit returns on the same clock edge it left on); needs a scope or a
      real slave.
- [x] I2C module (0x03): software master on PC1 (SDA) / PC2 (SCL), `CONFIG`
      (half period in us) / `WRITE` / `READ` / `GET_RX` / `GET_STATUS` / `SCAN`
      / `GET_SCAN`. Restores the "GPIO/I2C/SPI" promise of the module
      description. Verified on hardware against a BH1750 at 0x23.
- [x] I2C combined write-then-read (`WRITE_READ`, 0x57): START, write,
      repeated START, read. Verified against the BH1750 (an address only write
      plus repeated START read returns the same 25.8 lx as the two transaction
      sequence, and a write phase carrying a byte reports 1 write + 2 read
      bytes).
- [x] I2C register idiom verified against a real AT24C256 (32 KB, 2 byte word
      address, 64 byte pages): page aligned markers 8/8, 200 round page safe
      random write + read soak clean.
- [x] I2C ACK polling: `WAIT_READY` (0x5d) polls the device address until it
      acknowledges and `GET_WAIT_US` (0x5e) reports the measured duration, with
      a timeout status.  Measured on the AT24C256: **326 us** per write cycle
      (min = median = max over 30 writes) against the datasheet's 5 ms maximum,
      so the fixed 6 ms sleeps that used to be needed are gone.
- [x] Throughput: `MEM_WRITE_READ` (0x5f) does page write + ACK poll + Random
      Read in one request.  Measured 18.0 ms per round against 42.0 ms for the
      three request version (2.3x, tight polling).  A full 64 byte page commits
      in ~2.9 ms against ~0.33 ms for four bytes.
- [ ] Further round trip trimming: the combined path still spends ~6 control
      transfers (sequence baseline, request, sequence polls, status, read back).
      The status read could be folded into the read back (infer success from the
      response length) and the baseline read could go if the firmware reported a
      per request completion tag.
- [x] I2C clock raised: `CONFIG` now takes the half period in 100 ns units and
      defaults to 12 (1.2 us), which measures ~280 kHz on the wire against the
      AT24C256's 400 kHz rating.  Verified with a **300 round random page safe
      soak, zero failures** and 20.2 ms per round (the three request version
      needed ~60 ms); 12/18/25/50 all ran clean, so the slower settings are only
      a fallback for poor wiring.
- [ ] Host side helper that splits writes at device page boundaries (the
      64 byte roll-over is easy to get wrong - it cost me a debugging round).
- [ ] I2C: a bit banged master misreads an ACK slot roughly once in 300
      transactions when probing an address nobody answers (0 on the real
      device); a real driver would retry, the firmware reports it faithfully
      and the tests retry single shot checks.
- [x] Observed from the hardware: the BH1750 NACKs a *second* consecutive data
      byte in one transaction (DATA_NACK at byte 1), so it takes exactly one
      command byte per transaction.  Worth remembering when a host driver
      batches commands.
- [ ] I2C: consider the CH32V003 hardware peripheral for speed, and 10 bit
      addressing.
- [ ] Transport choice for SPI/I2C payloads: measured on hardware, a 64 byte
      round trip costs ~6 ms through the control-OUT/IN data stage but ~32 ms
      through the interrupt endpoints (they are polled once per 1 ms low speed
      frame). Prefer the control path for small transfers, and only use the
      EP path for larger pipelined transfers.
- [ ] SPI: hardware chip select, 16 bit frames, DMA, and a mode where the
      MISO bytes are returned in the same control transfer for short transfers.
- [x] Framing layer on the endpoint path (`vendor/frame.{h,c}`), modelled on
      `drivers/mfd/dln2.c`: request `[size][id][echo][handle][arg]` on EP2 OUT,
      response `[size][id][echo][handle][result][payload]` on EP3 IN, answered
      from the main loop while the interrupt only accumulates bytes and drains
      the response FIFO.  `echo == 0` means fire and forget, a bad length
      resynchronises at the next packet boundary, and `SET_FRAME_MODE` (0x39)
      switches EP2/EP3 between the frame stream and the raw echo stream.
      `frame_test.py` passes 26 checks (generic and gpio commands over frames,
      echo tags, unknown module/command, resynchronisation, a full 72 byte
      frame, mode switching and the raw echo afterwards).
- [x] Frame path performance, measured with `frame_test.py --bench/--pipe`:
      a sequential framed round trip costs **7.8 ms** against **3.0 ms** for a
      control transfer, and 200 fire and forget frames go out at ~0.5 packets
      per ms.  Two low speed facts explain it: rv003usb answers *every* IN token
      with a packet (a zero length packet when nothing is queued - it has no NAK
      support), so the host always burns one extra poll waiting for a response,
      and an 8 byte multicast endpoint only moves one packet per 1 ms frame.  So
      the endpoint path is not a win for single small round trips; it pays off
      for batched/fire-and-forget traffic and for payloads too big for a control
      data stage.  Keep the control path for single commands.
- [x] **Handler time budget measured**: a vendor request handler runs inside the
      USB interrupt *before* rv003usb acknowledges the SETUP packet, and it has
      about **4.5 us** (203 ticks of the 48 MHz SysTick works, 13 x 20 = 260
      ticks fails) before the ACK misses its low speed window and the host fails
      the transfer with EIO.  Anything slower has to be deferred to the main
      loop - which is why control-OUT data stages and frames are queued.  `GET
      V003_GET_TIMING (0x3f, wValue = iterations)` re-measures it on any build.
- [x] Related trap: the main loop must never block.  Printing the rv003usb
      UEvent ring over the bit-banged debug channel takes milliseconds per event
      and stalls everything the main loop still owes the host - a zero length
      control OUT is queued for main, so `SET_FRAME_MODE` silently never took
      effect and the framed path stayed dead.  UEvent printing is now off by
      default (`make DEBUG_EVENTS=1` to bring it back).
- [x] **RAM budget**: 2 kB of RAM, the stack starts at 0x20000800 and grows down
      into the statics, so an oversized buffer does not fault, it corrupts
      whatever sits at the end of `.bss` (the symptom was `ep_rx_count` and the
      EP3 FIFO holding garbage).  The 32 entry UEvent ring alone ate 512 bytes;
      `RV003USB_NUMUEVENTS` is now configurable and the vendor build uses 8.
      After that (and 16 byte EP buffers instead of 64, and sharing the EP3 FIFO
      with the frame responses) `.bss` ends at 0x200004f4: 780 bytes of stack
      where at most 272 are used.  `V003_GET_STACK_FREE` (0x3b) reports the
      remaining stack, measured by painting the free RAM at boot and scanning it
      from the main loop (the scan alone costs ~150 us, which is 30x the
      interrupt budget, so it must not run in the interrupt).
- [x] **Endpoint data toggles are now reset on SET_CONFIGURATION**.  rv003usb
      only resets the toggle of the endpoint that received a SETUP, so a host
      that reopens the endpoints without a bus reset (a userspace libusb
      program, or the kernel driver after the device was used by something else)
      starts at DATA0 while the device still expects the previous parity.  A
      mismatched OUT packet is acknowledged and thrown away, and the toggle does
      not advance on a mismatch, so EP2 stayed *permanently* dead: `ep2.write()`
      returned success while the firmware never saw a byte.  Cost me a long
      debugging session; the tests now send SET_CONFIGURATION first to resync.
- [ ] `simple_usb_requests` ring buffer: `head` is written from USB interrupt
      context and read from the main loop, and a full ring silently advances
      `tail` (dropping the oldest request) - racy and lossy.
- [ ] `ctrl_out_len` / `ctrl_out_buf` completion protocol is written from
      interrupt context and polled from `main()`; needs a memory barrier and a
      comment block documenting the layout.
- [ ] `V003_GET_EP_STATS` is asymmetric: 0-2 return RX byte counts, 3 returns
      EP3 IN packets and 4 EP3 IN bytes. Document (done in README) or replace
      with explicit per-endpoint requests.
- [ ] PD3/PD4/PD5 (USB D+/D-/DPU) and PD6 (boot button) are exposed as usable
      GPIO lines to the host; they should be reserved/refused.
- [x] `V003_GPIO_REQUEST` semantics: claims the line and puts it into input
      mode with pull-up/down (matches the gpiolib request -> direction_output
      flow). Now covered by a test.
- [ ] `vendor.h` still carries dead definitions (`struct usb_ctrl_msg_ctx`,
      `REQ_EP1_OUT`/`REQ_EP2_IN`, `EP2_IN_ADDR`/`EP3_OUT_ADDR`/`EP4_IN_ADDR`,
      `__maybe_unused`) and `hexdump.c` is compiled but never called.
- [ ] USB string descriptors are still upstream placeholders
      (`"embeddedboys"` / `"CH32V003 Vendor Spec Tester"`, serial `"0000"`).
- [ ] `bootloader/` is upstream code that was never rebuilt or tested here.

## Kernel driver (`kernel/`)

- [x] Restructured after `drivers/mfd/dln2.c`: `usb-mfd.ko` is now only the USB
      transport and MFD core (`v003_transfer_out()`/`v003_transfer_in()`,
      exported, mutex serialised, firmware version check like dln2's
      `dln2_check_hw`, cells registered with `devm_mfd_add_devices()`), and
      `v003-gpio.ko` is a platform driver hanging off the `v003-gpio` cell -
      exactly dln2's split.  `blink.c` is gone.
- [x] The USB pins (PD3/PD4/PD5) and the boot button (PD6) are kept out of the
      gpiochip through `init_valid_mask`, so userspace cannot drive the bus it
      is talking over.
- [x] `tests/gpio_chardev.py`: dependency free GPIO v2 character device test
      (ctypes ioctl, no libgpiod, no deprecated sysfs) which also checks that
      the reserved lines are refused.
- [x] Framed endpoint transport implemented in the core, mirroring dln2's use of
      the interrupt endpoints: requests are built in `v003_frame_exchange()`,
      sent on EP2 OUT and matched on EP3 IN by echo tag (the firmware answers
      every IN token, so a read can come back empty and the exchange keeps
      asking).  `transport=0|1|2` (control / framed / framed writes with control
      reads) selects it, `v003_cmd_out()`/`v003_cmd_in()` keep the child drivers
      transport agnostic, and the core enables frame mode at probe and disables
      it again on disconnect so the userspace tests still find the device in
      echo mode.  Verified on hardware: the gpio chip works identically over
      both transports (all 13 checks pass) - and the framed one is **slower**:
      set 3.93 ms (254/s) and get 6.82 ms (147/s) against **2.00 ms (500/s) and
      3.00 ms (333/s)** for control transfers.  A low speed control transfer
      does a whole small request in 3 frames (SETUP, data, status), while the
      endpoint path needs ~4: two packets to carry a 10 byte request, then the
      host polls EP3 for a response that cannot be in the same frame, and
      rv003usb answers with a zero length packet while it has nothing (it has no
      NAK support), which costs another poll.  So the framed path is *not* an
      improvement for single small commands - it is the right tool for batched,
      fire-and-forget or large transfers, and it is what the protocol needs
      before any of that can be tried.  Recommendation: keep `transport=0` as
      the default and revisit when a batch command exists.
- [x] `v003-i2c.ko`: the firmware's bit banged master behind a Linux
      `i2c_adapter` (`kernel/i2c.c`), modelled on `drivers/i2c/busses/i2c-dln2.c`.
      A message without `I2C_M_RD` becomes WRITE, a read directly after a write
      becomes WRITE_READ (the firmware emits the repeated START), a lone read
      becomes a current address read, and the bytes come back through GET_RX.
      The status the firmware reports maps to errno (address NACK -> -ENXIO,
      data NACK -> -EIO, timeout/stretch -> -ETIMEDOUT).  Verified on hardware
      against the AT24C256: the probe self test (`selftest=0x50 selftest_len=16`)
      reads `e0 e0 e0 e0 e0 e0 e0 e0 a1 09 0a 0b 0c 0d 0e 0f` from word address
      0, and the first eight bytes are the marker `scripts/eeprom_test.py` wrote
      through the *userspace* control path - two independent host paths agreeing
      on the same device.  `selftest=0x51` (nothing there) returns -ENXIO.
- [x] **Stale result read, found by `tests/i2c_dev_test.py`**: a control transfer
      with a data stage is answered by the interrupt before the main loop has
      executed it, so an I2C status read could return the *previous* request's
      status (dmesg caught it: `status 0x2`, address NACK from the request
      before, against `0x1201` on a re-read).  Two requests failing the same way
      cannot be told apart by re-reading, so the firmware grew
      `V003_I2C_GET_RESULT` (0x58) which returns the status with the main loop's
      request counter in its top byte; the driver polls until the tag changes
      (free in the common case, one extra transfer when the firmware lags).  The
      framed endpoint protocol has this synchronisation built in, because the
      main loop builds the response after doing the work - that is where I2C and
      SPI payload commands belong long term.
- [x] `tests/i2c_dev_test.py`: ctypes + i2c-dev test of the *kernel* path (finds
      the adapter by name, functionality bits, empty address -> ENXIO, marker and
      16 byte page write/read back, unaligned write, 10 bit refused, page
      restored).  Passes, three runs in a row.  A udev rule hands `/dev/i2c-N`
      to `plugdev` so it needs no root.
- [x] **Found by the kernel**: USB transfer buffers must not live on the stack
      ("transfer buffer is on stack", the transfer then fails with EAGAIN), and
      in kernel I2C/SMBus clients pass stack buffers routinely.  The core now
      bounces data stage payloads through its own buffers, and the I2C driver
      reads into its own buffer before copying to the caller's.
- [ ] `/dev/i2c-N` belongs to root:i2c, so the adapter can only be exercised
      from userspace with root (or after `usermod -aG i2c`).  Everything below
      that boundary is verified through the parameter gated self test.
- [ ] Batching is the missing piece for the endpoint path: a burst of frames in
      one OUT transfer is what beats one control transfer per operation, but the
      firmware only queues `V003_FRAME_RX_SLOTS` (= 2) requests, so a burst of 8
      loses 6 (counted in `GET_FRAME_STATS`).  Either deepen the queue (RAM is
      short, 72 byte slots) or add an explicit batch command (several
      (pin, value) pairs in one frame, like dln2's port multi write).
- [ ] The dln2 style async URB pool could still win the response poll back
      (submit the EP3 IN URB while the request is on the wire instead of a
      synchronous `usb_interrupt_msg` loop), but the 3 frame control transfer
      stays hard to beat for a single 4 byte result.
- [ ] Transport: the core still uses control transfers.  The dln2 style endpoint
      protocol (framed messages on EP2 OUT / EP3 IN with a `size/id/echo/handle`
      header) is the next step - it removes the ~3 ms per operation control
      transfer cost and allows several requests in flight.

- [x] `usb-mfd.ko` did not compile at all on a current kernel: `gpio_chip.set`
      returns `int` since 6.16, the driver used `void`. Fixed.
- [x] Ignored return values: `v003_usb_tx()`/`v003_usb_rx()` results were
      dropped, so `gpio_get()` returned a bogus `0` on USB failure. All paths
      now propagate errors.
- [x] `dev_info()` on every get/set/request/free (twice per `get`) moved to
      `dev_dbg()`, real failures still use `dev_err()`.
- [x] Added a mutex serialising vendor control transfers, an `offline` flag,
      and `.suspend`/`.resume` handlers so queued gpiolib calls fail fast
      instead of racing a disconnect.
- [x] Compiles cleanly with `make LLVM=1` (the core plus `v003-gpio.ko`,
      `v003-i2c.ko`, `v003-spi.ko`).
- [!] Never loaded on hardware: `insmod`/`rmmod`/`dmesg` need root and this
      sandbox has no usable sudo (`sudo -n` requires a password).
- [ ] Parked by decision: finish the userspace data path first, the driver is
      awkward to iterate on until then (loading it needs root and a reload
      cycle per change). Everything below is compile-verified only.
- [x] `blink.c` is deleted.  It was a leftover experiment that registered
      itself as `DRV_NAME "v003-usb-mfd"` and matched `1209:c303` like the core
      does, so with both built only one could bind, and its 1 s probe delay made
      the failure look like a firmware problem.  It had already been dropped
      from the build when the core took over; now the file is gone too.
- [ ] probe discovers EP1-OUT/EP2-OUT/EP3-IN but never submits an URB to them;
      bulk/interrupt data paths are still dead code.
- [ ] No `get_multiple`/`set_multiple` and no shadow state: one synchronous
      200 ms-timeout control transfer per line operation.
- [x] `v003-i2c.ko` loads from the MFD cell, registers `i2c_adapter` bus 8 and
      probes the EEPROM through it (see the firmware section).
- [x] All four modules load together (`usb-mfd`, `v003-gpio`, `v003-i2c`,
      `v003-spi`) and each one verifies: the gpio character device test passes,
      the I2C self test reads the EEPROM marker and the SPI self test loops 100
      bytes back through the PC6<->PC7 jumper.
- [x] `v003-spi.ko`: `struct spi_controller` over the firmware's SPI module
      (`kernel/spi.c`), modelled on `drivers/spi/spi-dln2.c`.  Chip select is
      driven through the GPIO module (a message, not a transfer, is what sits
      between two CS edges, and the firmware only drives its pin around one
      transfer), transfers longer than the firmware's 64 byte limit are split
      into chunks, and `max_speed_hz` maps onto the SPI_CTLR1 BR field
      (min/max 187.5 kHz / 24 MHz).  Verified on hardware with the
      parameter gated loopback self test through `spi_new_device()` +
      `spi_sync()`: 16 B at 24 MHz, 65 B (a 64 + 1 chunk split) at 1 MHz, 100 B
      at 1 MHz and 4 MHz, 128 B with chip select disabled - every byte came back
      unchanged.
- [ ] Chip select pins (and the I2C/SPI pins themselves) are still offered by
      the gpiochip: the child drivers claim what they use, but the gpio child's
      valid mask is built before they probe.  A reservation mask in the core
      (or a `reserved=` parameter on the core) would keep userspace from
      fighting the SPI driver for its CS line.
- [ ] SPI master driver: `struct spi_controller` over the SPI module
      (`ENABLE`/`CONFIG`) plus the EP data path (EP2 OUT -> MOSI, EP3 IN ->
      MISO), with the GPIO module driving chip select.
- [ ] "MFD" in the name, but no MFD framework usage (no cell/child devices).

## Link stability (hardware)

- [!] The board's USB link became electrically marginal after the first few
      hours of testing. Kernel log over 4 minutes: 35x `error -71` (EPROTO),
      17x `USB disconnect`, 10x `usb usb1-port2: disabled by hub (EMI?),
      re-enabling...` and 7x `can't set config #1, error -71`, with the device
      repeatedly re-enumerating and sometimes absent. The device answers over
      SWIO the whole time (`minichlink -i` detects the CH32V003), so the MCU is
      alive - it is the low speed USB link that is failing. A chip reboot
      (`minichlink -b`) restores it, temporarily.
- [ ] Check the usual rv003usb hardware prerequisites before blaming firmware:
      33 ohm series resistors on D+/D-, short wires, solid ground, and power the
      board from a stable supply instead of the WCH-Link 3V3 rail. The
      bootloader README warns about exactly this.
- [ ] Once the link is stable, re-run `scripts/stress_test.py --mode
      {ctrl,ep,mixed}` and `scripts/v003_test.py` in a loop to separate link
      flakiness from firmware bugs (the full suite passed repeatedly before the
      link degraded, including three consecutive runs).

## I2C debugging lessons (EEPROM session)

- [x] The "I2C addressing is broken" scare was mostly **my test harness**: it
      read the `GET_CTRL_OUT_SEQ` baseline right after issuing *another*
      request, so a still pending increment satisfied the wait and the result
      of the previous transaction was read (one transaction late).  Every
      request now settles the counter first (`sync()`), which turned 1/7 into
      6/7 absolute addresses verifying correctly.
- [x] Clock tracer added: `V003_I2C_TRACE` (0x5a) arms it, `GET_TRACE` (0x5b)
      reads it, `GET_TRACE_INFO` (0x5c) reports count/overflow, and
      `scripts/i2c_trace.py` decodes the SDA level at every rising SCL edge -
      i.e. exactly what a slave latches.  It proved the master clocks
      `0xA0, hi, lo, repeated START, 0xA1` byte for byte correctly.
- [x] **Root cause of the whole "I2C addressing is broken" scare, found and
      fixed.**  It was the control-OUT data stage handoff, with two defects:
      1. one shared buffer, written by the USB interrupt while main() was still
         processing it (I2C bit banging takes milliseconds) - replaced by a two
         slot queue with per-request bookkeeping, and requests that cannot get a
         slot are dropped and counted (`GET_CTRL_OUT_DROPS`, 0x38);
      2. the completion marker *is* the expected length, so a slot reused for a
         request of the same length still looked complete and main() consumed it
         **before the new payload had arrived** - the previous request's bytes
         then went out on the I2C bus even though the host had sent the new
         ones.  The interrupt now clears the marker when it claims the slot.
      Verified with a three way comparison (sent / firmware echo / wire trace):
      6/6 addresses now clock exactly the requested bytes.
- [x] EEPROM path verified end to end after the fix: page aligned markers at
      eight addresses 8/8 correct, and a 200 round soak of page safe random
      write + read with zero failures (plus all three test suites green).
- [x] Learned the hard way: the AT24C256 page write is 64 bytes and a write
      crossing a page boundary **wraps inside the page**, so a 16 byte write at
      an unaligned offset silently discards the tail - that was a test
      methodology trap, not a driver bug.
- [x] I2C clock recalibrated with the tracer: one iteration of the delay loop
      costs about ten cycles on this core (not two), so the bus was running at
      ~21 kHz instead of the intended ~100 kHz.  Now `half_us = 10` measures a
      20 us SCL period (~50 kHz), and the trace time unit is 64 ticks.
- [x] Internal pull-up trick removed again on request (the bench modules all
      carry their own pull-ups).  The manual fact worth keeping: CH32V003 has
      pull-up/pull-down only in input mode (`GPIOx_CFGLR` CNF=10, MODE=00b),
      where `GPIOx_OUTDR` selects pull-up (1) or pull-down (0); a software
      master can use that while releasing a line and switch to open drain
      output to pull it low.

## Tests & tools

- [x] `scripts/v003_test.py` extended: generic module, EP1/EP2 OUT reception,
      EP3 IN echo data path, FIFO level/overflow accounting, FIFO wrap-around,
      512 byte streaming, control-OUT data stage round trip for 23 lengths
      (1..64 bytes, including the "clamped read" case), the GPIO module and
      control-OUT data stages. Passes on hardware.
- [x] `scripts/spi_test.py` added: SPI state/config, byte accounting, MISO
      sampling with PC7 driven from the GPIO module, SCK idle level, and the
      fallback to echo when SPI is disabled. Passes on hardware.
- [x] `scripts/i2c_test.py` added: bus idle levels, address scan, NACK
      handling, and a BH1750 measurement loop (`--watch N`).
- [x] `scripts/stress_test.py` added: hammers one transport (`ctrl`, `ep` or
      `mixed`) and reports the iteration that wedged, to tell link flakiness
      and firmware bugs apart.
- [ ] Run `scripts/spi_test.py --loopback` with a jumper between PC6 (MOSI)
      and PC7 (MISO) to verify true full duplex transfer (bit order included).
      Until then SPI is only verified one direction at a time: MOSI/SCK/CS by
      the clocked byte count and pin levels, MISO by driving PC7 from the GPIO
      module.
- [ ] `tests/gpio_sysfs.py` uses the deprecated sysfs GPIO ABI and hardcodes
      line 32; `/sys/class/gpio` does not even exist on a kernel without
      `CONFIG_GPIO_SYSFS`. Move to the GPIO character device / libgpiod.
- [ ] `tests/gpio` (Rust + rusb) discards all transfer results (`_ = ...`) and
      has no CLI or assertions.
- [ ] `scripts/ctrl_transfer.py` is a scratch file with unused variables.
- [ ] No CI and no `make test` entry point for the userspace suites.

## Debugging (WCH-Link + GDB)

- [x] `minichlink -G` GDB stub on port 3333 works with `riscv-none-elf-gdb`:
      registers, memory and firmware variables readable (`p/x ep3_tx_head`),
      halting/stepping/breakpoints usable, firmware `printf()` streaming.
- [x] Flashing through GDB (`load`) verified end to end (wrote a build with a
      bumped `V003_DEVICE_VER` 0x1011 from GDB, booted it, read 0x1011 back over
      USB; then wrote 0x1010 again). ~2 KB/s.
- [x] Upstream bug found and worked around: `microgdbstub.h` built the memory
      map with `iss->flash_size` (kB) where the map wants bytes, so GDB saw
      16 bytes of flash and fell back to plain writes, making `load` fail.
      Patch kept in `patches/ch32fun-gdb-flash-memory-map.patch` (the ch32fun
      submodule checkout carries it locally) - worth reporting upstream.
- [ ] The stub owns the WCH-Link while it runs; stopping it mid-halt can leave
      the target halted (a `minichlink -b` reboot recovers it). Consider
      documenting/automating a `make gdb` helper.

## Docs

- [x] README: toolchain, build, flash (`minichlink`), kernel module and test
      instructions, plus the full request/endpoint protocol tables.
- [x] README: the empty `[CH32V003 USB Dev Board]()` links are gone.
- [ ] README: add the dev board product link once it exists.
- [ ] Document the bootloader flashing flow (upstream README only).

## Host/test harness lessons

- [x] Reading the `GET_CTRL_OUT_SEQ` baseline right after issuing another
      request made results look one transaction stale; settle the counter first
      (`sync()`) - it cost a whole debugging session of chasing ghosts.
- [x] The clock tracer must only log *real* rising SCL edges (logging one when
      SCL was already high shifted every decoded byte), and arming it must be
      confirmed (poll the entry count to zero) or the previous trace is read
      back.  Both bugs were in my tooling, not the DUT.

## Verified on hardware

_Updated after each real-hardware run (WCH-LinkE + CH32V003)._

- [x] Firmware builds from a clean checkout (FLASH 35.4 %, RAM 54.9 %).
- [x] Firmware flashes over the WCH-LinkE, re-enumerates as `1209:c303`, and
      its `printf()` log is readable with `minichlink -T`.
- [x] `scripts/v003_test.py` passes (33 checks).
- [x] `scripts/spi_test.py` passes (22 checks, plus the soak with `--loopback`).
- [x] SPI full duplex soak: 150 random 8 byte transfers over the endpoint path
      and 150 control transfers (lengths 1..64), zero mismatches, each also
      checked against `GET_STATS` so a fallback to the echo path cannot pass.
- [x] Firmware internals cross-checked over GDB (`ep3_tx_head`/`ep3_tx_tail`
      matched the host side FIFO level after the test run).
- [x] Firmware written through GDB `load` boots and runs (version marker
      round trip).
- [x] After re-plugging the board the link is stable again: the full stress
      matrix passed (`ctrl` 300 iterations at 167 iter/s, `ep` 300, `mixed`
      300 at 54 iter/s) and `scripts/v003_test.py` passes. The earlier wedge
      was the link, not the firmware.
- [x] SPI write/read are verified separately and together: MOSI/SCK/CS by the
      clocked byte count and pin levels, MISO by driving PC7 from the GPIO
      module, and full duplex with the PC6<->PC7 jumper.
- [x] I2C verified with a real slave: scan finds the BH1750 at 0x23, address
      ACK/NACK behaviour is right, hundreds of transactions with no failures,
      and stable illuminance readings (repeated identical values across runs).
- [x] I2C repeated START (`WRITE_READ`) verified: an address only write plus a
      repeated START read returns the same illuminance as the two transaction
      sequence, and a write phase carrying one byte reports 3 bytes moved.
- [x] `usb-mfd.ko` + `v003-gpio.ko` load, the core reports the firmware version
      (0x1010, serial 0x12345678), the child probes off the MFD cell and a
      gpiochip with 56 lines appears.
- [x] GPIO works through gpiolib: PC0 (line 32) drives and reads back, PC4
      (line 36) reads as input, and the reserved lines 51-54 (D+/D-/DPU and the
      boot button) are refused.
- [x] Latency measured through the character device: **set 2.00 ms (503/s),
      get 3.00 ms (333/s)** - one control transfer per gpiolib operation.
- [x] `scripts/frame_test.py` passes (26 checks) and repeats cleanly five times
      in a row (it failed intermittently before the SET_CONFIGURATION toggle
      fix, which was the one real firmware bug of the session).
- [x] Stack canary on hardware: `.bss` ends at `0x200004f4` (780 bytes of stack),
      and after the UEvent printing was turned off the deepest use measured is
      **272 bytes** (508 free) - the printf path had been the deepest stack
      consumer.  Firmware: FLASH 51 %, RAM 62 %
      - the RAM freed by the UEvent ring and the smaller EP buffers is what
      makes the framed path affordable at all.
- [ ] `get_multiple`/`set_multiple`: one request per line operation is the
      current cost model; batching several lines into one vendor request would
      need a firmware command carrying (pin, value) pairs.
