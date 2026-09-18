# The framed endpoint protocol

Implemented in `vendor/frame.c`, `vendor/frame.h`, mirrored by
`v003_frame_send()` / `v003_frame_xfer()` in `kernel/usb-mfd.c`, and tested by
`scripts/frame_test.py` (26 checks) and `scripts/stress_test.py --mode ep`.

It follows `drivers/mfd/dln2.c` on purpose: the USB driver is a transport and
the payload commands are framed requests with an id, a handle (module) and an
echo tag.

## Wire format

Little endian, requests on **EP2 OUT**, responses on **EP3 IN**:

```
request   [size u16][id u16][echo u16][handle u16][arg u16][payload...]
response  [size u16][id u16][echo u16][handle u16][result u16][payload...]
```

| Field    | Meaning                                                                   |
| -------- | ------------------------------------------------------------------------- |
| `size`   | total frame length including the header; 8..72 for requests                |
| `id`     | command byte; a control-path encoded `cmd \| module << 8` is accepted too (only the low byte is used, the module always comes from `handle`) |
| `echo`   | host tag, copied into the response. **0 = no response wanted**             |
| `handle` | module id: 0x00 generic, 0x01 GPIO, 0x02 SPI, 0x03 I2C                     |
| `arg`    | the request argument, i.e. the `wValue` of the equivalent control request  |
| `result` | 0 = OK, 1 = EBADCMD, 2 = ETOOBIG                                           |

`size` makes the stream self delimiting: the receiver knows where a frame ends
without relying on packet boundaries or a terminating zero length packet. A bad
`size` drops the rest of the current packet (`frame_rx_resync`) instead of
hunting for a sync pattern in the middle of arbitrary data.

## Behaviour

- **Requests** are accumulated by the EP2 OUT callback into two slots
  (`V003_FRAME_RX_SLOTS`) and executed by `main()`. Two slots are enough because
  a host that waits for a response cannot get ahead; a host that does not gets
  its extras counted in `GET_FRAME_STATS` instead of corrupting a request in
  flight.
- **Responses** are built by `main()` after the command ran and pushed into the
  EP3 IN FIFO, which the interrupt drains one 8 byte packet per IN token. The
  response therefore *cannot* be stale - the synchronisation that the control
  path needs a completion tag for is free here.
- **`echo == 0`** means fire and forget: two 8 byte packets on the way out and
  no traffic back, which is the only place the endpoint path is cheaper than a
  control transfer (2 frames against 3).
- **Mode switch**: `V003_SET_FRAME_MODE` (0x39, zero length control OUT, `wValue`
  1/0). Enabling clears both the request slots and the EP3 IN FIFO, so leftovers
  from the raw echo stream cannot arrive as a bogus first response. The switch is
  applied by `main()`, so anything that blocks `main()` blocks it too.
- **Framed and raw modes are mutually exclusive**: in frame mode EP1/EP2 OUT are
  no longer echoed to EP3 IN (the firmware simply drops EP1 OUT), because a
  mixed stream cannot be parsed.

## Measured performance

Sequential round trips, `scripts/frame_test.py --bench 60`:

| Path                       | per operation         |
| -------------------------- | --------------------- |
| control transfer (EP0)     | 2.98 ms               |
| framed (EP2 OUT + EP3 IN)  | 7.77 ms               |

Through the kernel drivers (`tests/gpio_chardev.py --bench 60`), where the same
gpiochip is driven over either transport:

| Operation | control              | framed                |
| --------- | -------------------- | --------------------- |
| set       | 2.00 ms (500/s)      | 3.93 ms (254/s)       |
| get       | 3.00 ms (333/s)      | 6.82 ms (147/s)       |

Fire and forget, 200 frames of 10 bytes: 400 EP2 OUT packets in 784 ms
(~0.51 packets/ms, i.e. two frames per transfer), all 200 handled, 0 dropped.

Why the framed path loses, in one line: a low speed control transfer finishes a
small command in 3 frames, while the endpoint path needs at least 4 - two
packets of request, then a poll of EP3 whose answer cannot be in the same frame,
and rv003usb answers that poll with a zero length packet because it has no NAK.
See [usb.md](usb.md).

Where it wins, or would if the rest existed:

1. **fire and forget** commands (2 frames, no response, no completion tag);
2. **batched** commands: several frames in one OUT transfer is the real prize,
   but it needs either a deeper request queue than two slots (72 byte slots cost
   RAM that is not there) or an explicit batch command carrying several
   (pin, value) pairs in one frame, like dln2's port multi write;
3. **payloads** bigger than a single control data stage;
4. any command whose result must be synchronised with its execution, for free.

## Testing

`scripts/frame_test.py` covers: generic and gpio commands over frames, the
control-path encoded id form, echo tags, `echo = 0` staying silent, unknown
module and unknown command (EBADCMD with no payload), a bogus length followed by
a working frame (resynchronisation), a full 72 byte frame (9 OUT packets), the
mode switch and the raw EP1 -> EP3 echo afterwards.

Two traps this test is built around:

- it sends `SET_CONFIGURATION` before the first request, otherwise the device's
  toggles from the previous session desynchronise from the host's and EP2 OUT
  dies silently ([usb.md](usb.md));
- a response that arrives without a payload must not be read as "value 0": the
  test asserts `result == OK` before looking at a value, because an EBADCMD
  response has no payload and looks exactly like a zero.
