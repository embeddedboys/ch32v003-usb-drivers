# Notes

Knowledge base for this project: what the hardware actually does, why the code
looks the way it does, and which mistakes have already been paid for.

The other documents in the repository answer different questions and are kept
separate on purpose:

| Where           | Answers                                                            |
| --------------- | ------------------------------------------------------------------ |
| `README.md`     | how do I build, flash, load and run this?                          |
| `TODO.md`       | what is done, what is open, what was verified on hardware           |
| `notes/` (here) | *why* - measured limits, protocol details and debugging case studies |
| `AGENTS.md`     | the rules to follow when changing anything in this repository       |

## Index

| Note                                     | Contents                                                                |
| ---------------------------------------- | ----------------------------------------------------------------------- |
| [firmware.md](firmware.md)               | firmware structure, the command reference, the two hard budgets, the RAM map |
| [usb.md](usb.md)                         | what software USB on a CH32V003 costs: frame maths, toggles, the ACK budget |
| [frame-protocol.md](frame-protocol.md)   | the framed endpoint protocol, its wire format and measured performance   |
| [i2c.md](i2c.md)                         | bit banged I2C: wiring, clock calibration, memory devices, result tags    |
| [kernel.md](kernel.md)                   | the Linux side: MFD core, transports, GPIO/I2C/SPI child drivers          |
| [debugging.md](debugging.md)             | tooling, case studies of every bug found, and test harness traps          |

## Reference documents

| Document                          | Where                                                      |
| --------------------------------- | ---------------------------------------------------------- |
| CH32V003 application manual V1.9  | `hardware-docs/CH32V003RM.PDF` (tracked) - RCC, GPIO, SPI1, USB and SysTick registers, and the 2 kB SRAM map |
| BH1750, AT24C256 datasheets        | `hardware-docs/` (untracked, they belong to the bench)      |
| Diolan DLN-2 drivers              | the kernel tree: `drivers/mfd/dln2.c`, `drivers/{gpio,i2c/busses,spi}/*-dln2.c` |
| rv003usb                          | `rv003usb/` (vendored; `rv003usb.h` documents the stack's contracts) |

## How to use this

- Before changing firmware, read [firmware.md](firmware.md) and
  [usb.md](usb.md): two of the three constraints there (the ~4.5 us interrupt
  budget and ~790 bytes of stack) are invisible until something breaks in a
  bizarre way.
- Before touching a driver, read [kernel.md](kernel.md); before writing a test,
  read the traps section of [debugging.md](debugging.md).
- When a new hardware measurement contradicts something written here, fix the
  note. Numbers in these files are measured, not estimated, and every one of
  them should be reproducible with the commands next to it.
