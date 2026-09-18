# Bit banged I2C

`vendor/i2c.c`: a software master on **PC1 = SDA, PC2 = SCL**, open drain
outputs, external pull-ups required. The firmware drives one transaction per
request; a host driver builds the transactions.

## Wiring rules

- External pull-ups only. There used to be a trick that enabled the internal
  pull-ups; it was removed on request and must stay removed (the bench modules
  all carry their own pull-ups, and the internal ones are too weak and too slow
  to be useful).
- The pins must be configured before use: `V003_I2C_CONFIG` (0x50) sets them up
  as open drain, releases the bus and performs a bus recovery. Commands sent
  before that answer with `V003_I2C_ST_NOT_CONFIGURED`.

## Clock calibration

`V003_I2C_CONFIG` takes the **half period in 100 ns units**; 0 keeps the current
value, the default is 12 (= 1.2 us). One bit costs about three half periods, so
the default measures **~280 kHz** on the wire (verified with the clock tracer
against an AT24C256 rated 400 kHz). Slower settings are a fallback for poor
wiring: 25 ~ 140 kHz, 50 ~ 70 kHz.

The delay loop was calibrated the hard way: `I2C_CYCLES_PER_ITER` is 10, not 2 -
the naive estimate made the bus run at 21 kHz while the code "asked" for 100 kHz.

`I2C_TRACE` (0x5a) arms a tracer that samples SDA at every **rising** SCL edge -
exactly what a slave latches - together with the time since the previous edge,
and `scripts/i2c_trace.py` decodes it. It is the only way to see what actually
went onto the wire, and it settled the "our addressing is broken" scare by
showing that the bytes the host sent and the bytes the firmware echoed matched
while a *stale* payload was on the bus.

## Transaction commands

| Command          | What it does                                                              |
| ---------------- | ------------------------------------------------------------------------- |
| `WRITE` 0x51     | START, `addr<<1\|0`, payload, STOP (data stage `[addr][bytes...]`)         |
| `READ` 0x52      | START, `addr<<1\|1`, `count` bytes, STOP (data stage `[addr][count]`)      |
| `WRITE_READ` 0x57| START, write, **repeated START**, read `wValue` bytes - the register idiom |
| `WAIT_READY` 0x5d| ACK polls `addr<<1\|0` until the device answers (memory write cycles)      |
| `MEM_WRITE_READ` 0x5f | write + optional ACK poll + random read in one request                |
| `SCAN` 0x55 / `GET_SCAN` 0x56 | probe 32 addresses into a bitmap                              |

Status (`GET_STATUS` 0x54) is a bit field: OK, address NACK, data NACK, clock
stretch, not configured, read-phase NACK, timeout, plus the number of bytes moved
and the index of the byte that NACKed.

## Memory devices (verified against the AT24C256 datasheet)

- 32 KB, 2 byte word address, **64 byte pages**.
- A write that runs past a page boundary **wraps around inside the page** - it
  does not continue in the next one. Getting this wrong is easy and it cost a
  debugging round; split writes at page boundaries.
- The write cycle (tWR) is 5 ms maximum, but ACK polling measured **326 us**
  (min = median = max over 30 writes), so `WAIT_READY` replaces the fixed 6 ms
  sleeps that were needed before.
- A read immediately after a write **must** NACK until the cycle finished. A test
  that does not expect that NACK is testing nothing.
- `MEM_WRITE_READ` (0x5f) does write + ACK poll + random read in one request:
  18.0 ms per round against 42.0 ms for the three-request version (2.3x).

## The completion tag, or: why reading a status right after a request lies

I2C commands travel in a control-OUT data stage, and a data stage is answered by
the USB interrupt **before** `main()` has executed it. A status read sent
immediately after the request can therefore return the *previous* request's
status.

Measured on hardware, from `kernel/i2c.c` while it was still reading
`GET_STATUS`:

```
addr NACK: status 0x2, re-read 0x1201
```

`0x2` is the address NACK of the request before (a probe of an empty address),
`0x1201` is the truth: OK, 18 bytes moved. And no amount of re-reading
`GET_STATUS` makes this reliable, because two requests that fail the same way
produce identical status words.

`V003_I2C_GET_RESULT` (0x58) returns `(main loop request counter << 24) | status`
so the result carries its own freshness tag; a host remembers the tag it last
accepted and polls until it changes. In practice the firmware has long finished
by the time the read arrives, so this costs nothing; when it has not, one extra
control transfer fixes it. `GET_STATUS` remains for hosts that already know when
the work is done (`GET_CTRL_OUT_SEQ`).

The framed endpoint protocol has this synchronisation built in, because `main()`
builds the response after running the command - that is where I2C payload
commands belong long term.

## Known limitations

- A bit banged master misreads an ACK slot roughly **once in 300** transactions
  when probing an address nobody answers (0 on the real device). The firmware
  reports it faithfully; tests retry single shot checks instead of pretending it
  does not happen.
- Some slaves NACK a second consecutive data byte in one transaction (the BH1750
  does, with `DATA_NACK` at byte 1), so one command byte per transaction is the
  safe pattern.
- No 10 bit addressing.
- SMBus: quick, byte, byte data and word data work (the i2c core emulates them
  as exactly the sequences the firmware already produces - verified by comparing
  each `i2cget` against the equivalent `i2ctransfer`: identical results), but
  the block sizes do not, because their emulation continues a transfer with
  `I2C_M_NOSTART`.  `kernel/i2c.c` advertises `0x7f0001` for that reason.
- `i2cget` is a poor fit for a 2 byte word address device like the AT24C256: an
  SMBus read sends **one** command byte, so the address the device ends up using
  is its own business (the retained high byte shows through).  Use
  `i2ctransfer -y 8 w2@0x50 0x00 0x00 r16` (or the at24 driver) for EEPROM
  addressing; `i2cdetect -y N` works and is a fine presence check.
