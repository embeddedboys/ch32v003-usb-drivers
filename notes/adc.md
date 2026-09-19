# ADC

`vendor/adc.c`, module id `0x06`, commands `0x80`-`0x85`. One conversion per
request, no buffering, no DMA: a host that wants a stream asks repeatedly, which
is what a link that moves 21 kB/s can honestly offer for two bytes per sample.

## The part is 10 bit, not 12

RM 9.1: "ADC 模块包含 1 个 10 位的逐次逼近型的模拟数字转换器，最高允许 24MHz 的
输入时钟" - a 10 bit successive approximation converter, ADCCLK up to 24 MHz.
RM 9.2.2 repeats it for the data register ("规则组通道的数据寄存器 ADC_RDATAR 保存
的是实际转换的 10 位数字值"), and the analog watchdog thresholds are 10 bit as
well.

So a reading is `0..1023` and

    mV = reading * AVDD / 1024        (AVDD = 3.3 V on this board)

The first version of the module scaled readings as 12 bit and reported the 1.2 V
internal reference as 294 mV, which is exactly the kind of number that reads as
"the wiring is wrong" rather than "the arithmetic is wrong": 365 counts is
1176 mV at 10 bits and 294 mV at 12 bits. The lesson is the general one: check
the resolution against the manual before interpreting a reading.

## Measured readings

| channel | source | raw | voltage |
| ------- | ------ | --- | ------- |
| 8 | internal reference, nominal 1.2 V | 365 | 1176 mV |
| 9 | internal calibration voltage, 2/4 AVDD | 511 | 1647 mV |
| 9 | internal calibration voltage, 3/4 AVDD | 767 | 2472 mV |
| 1 | PA1 driven by PWM channel 2 at 100 % | 1023 | 3297 mV |
| 1 | PA1 driven by PWM channel 2 at 0 % | 0 | 0 mV |

`scripts/adc_test.py` prints all of them and checks them; the internal reference
is checked to +-10 % (the datasheet tolerance on the reference, not on the
conversion), the two calibration levels to +-150 mV, and the PWM driven pin to
within 10 counts of full scale and ground.

## Conversion time

Sample time 7 is the longest the `SMPx` field offers, 241 ADCCLK, and
RM 9.2.2 gives `TCONV = sample time + 11 ADCCLK`. With ADCPRE = HBCLK/8 (6 MHz
at 48 MHz core clock) that is

    (241 + 11) / 6 MHz = 42 us

which is what `V003_ADC_GET_STATUS` reports on hardware (`us=42`), so the
register arithmetic and the measurement agree. A channel change adds a reset
calibration and a calibration, which is why the first conversion of a new
channel is slower than the number above.

**This is why the conversion does not happen in the USB interrupt.** The
interrupt has ~4.5 us before the SETUP handshake is late (notes/usb.md), and the
first attempt at this module ran the conversion inside a control-IN handler:
every request failed with `EIO`. The request is therefore an OUT request
(`V003_ADC_START`, wValue = channel) that only records what to convert, `main()`
runs it, and `V003_ADC_GET` hands the result back with a completion tag.

Note that `main()` is blocked for those 42 us, which is 2 % of the 1 ms USB
frame and far below the thing that actually breaks `main()` (a blocking `printf`,
which stops the main loop answering zero length control OUTs at all).

## Facts that cost a rebuild each

- **The calibration voltage selection is latched at power up.** Writing
  `CALVOL[1:0]` (CTLR1 bits 26:25) while the converter is running measured no
  change whatsoever (channel 9 read 512 counts for both settings). Clearing
  ADON, writing CALVOL, setting ADON and recalibrating makes it work (512 ↔ 768
  counts, 1650 mV ↔ 2475 mV). `V003_ADC_SET_CALVOL` does that.
- **EXTSEL must select SWSTART.** Both ch32fun's `examples/adc_polled` and WCH's
  own example set `ADC_EXTSEL` (`ADC_ExternalTrigConv_None` in the standard
  peripheral library is `0x000E0000`), so the module writes it too.
- **A peripheral reset first.** `RCC->APB2PRSTR` gets the ADC to a known state
  instead of whatever a previous run of the firmware left in it.
- **`V003_ADC_GET_STATUS` exists because of the two questions a reading cannot
  answer**: was the request seen at all, and did the conversion run? It returns
  the request count, the conversion count, the last conversion time in us and
  the first step that timed out. During development a lagging tag looked like a
  stale result until the counters showed that requests and conversions agreed.

## Pin table

The datasheet (package pin/pad table) is not in this repository, so the channel
to pin mapping is only partially known from what can be measured or found in
WCH's examples:

| channel | pin | how it is known |
| ------- | --- | --------------- |
| 1 | PA1 | measured: PWM channel 2 drives PA1 and channel 1 follows it exactly |
| 2 | PC4 | `EVT/EXAM/ADC/ADC_DMA` configures PC4 as `ADC_IN2` in its `ADC_Function_Init()` |

The other external channels are accepted and convert whatever their pin carries;
while scanning them for a driven pin, channels 5, 6 and 7 all read full scale
(1023 counts), which is consistent with them landing on the USB pins
(PD4/PD5/PD6 are pulled and driven by the USB stack - see the reserved pin mask
in `vendor/vendor.h`) and is a reminder that "an unconnected channel" is not a
thing on this package.

## Pins: this module configures none

The converter reads a pad whatever mode it is in - driven pins read their driven
level, floating ones read whatever they float to - so a host that wants the
datasheet's analog mode sets it through the GPIO module, where it is visible and
reversible.  The module only ever touches its own registers.

That is the third design, and the first two were measured to be wrong:

- **One-time setup** (the original): the first conversion put PC4 into analog
  mode.  PC4 is also this board's SPI chip select, and the SPI module only writes
  the output data register for it, so the select stopped moving **silently** while
  the clocking carried on - `spi_test` passed, because it checks data.  Measured:
  PC4 reported itself as an output after `SPI_SET_CS` and as an input after a
  single conversion of the internal reference.
- **Per channel**: converting channel 1 configured PA1 as an analog input, and
  PA1 is PWM channel 2, so the PWM output died mid-test (`pwm_test` and `adc_test`
  failed immediately).  Taking a pad away from whatever drives it is the bug, not
  the configuration timing.

The counterpart lesson for the modules that *do* drive pins: re-assert the mode
when it matters (`pwm_apply()` does, so a channel keeps driving after something
else has touched the pin).

## The ADC clock is re-asserted per conversion

`RCC->CFGR0`'s ADCPRE field is written before every conversion, not once at setup:
the power module's standby wake calls `SystemInit()`, which rewrites `CFGR0` and
leaves ADCPRE at its reset value (HBCLK/2 = 24 MHz instead of the /8 = 6 MHz this
module documents).  Measured: 42 us on a fresh boot, **11 us after a sleep** -
correct readings either way, since 24 MHz is legal, but the conversion time the
module reports would have been a lie and the 42 us in this file wrong.  The
check that caught it is `adc_test`'s assertion on that number, plus the same
assertion in `scripts/combo_test.py` after a sleep.

## Protocol

| command | direction | argument / result |
| ------- | --------- | ----------------- |
| `0x80` `V003_ADC_START` | OUT | wValue = channel, no data stage |
| `0x81` `V003_ADC_GET` | IN -> u32 | `(sequence << 16) \| (valid << 15) \| value` |
| `0x82` `V003_ADC_GET_INFO` | IN -> u32 | `(10 << 16) \| 10` |
| `0x83` `V003_ADC_GET_SEQ` | IN -> u32 | conversions the main loop has run |
| `0x84` `V003_ADC_GET_STATUS` | IN -> u32 | `(error << 24) \| (us << 16) \| (requests << 8) \| conversions` |
| `0x85` `V003_ADC_SET_CALVOL` | OUT | wValue 0 = 2/4 AVDD, 1 = 3/4 AVDD |

The tag in `V003_ADC_GET` follows the same rule as `V003_I2C_GET_RESULT`: the
sequence advances with every conversion, so a host that knows the previous tag
can tell a fresh answer from a cached one. The test asserts that one read after
`START` already carries the new tag, i.e. that there is no lag to hide.

## Cost

628 bytes of flash and 12 bytes of RAM against the `gpio,spi,i2c,wdg,pwm` build
(9408/1228 -> 10036/1240 with `TRACE=0`), which makes it the cheapest module
next to `gpio.c` and `wdg.c`.  Measured when this module landed: the default
build (with the I2C tracer) was 10264 bytes of flash and 1376 bytes of RAM with
408 bytes of stack margin; the default build has grown since (see
[firmware.md](firmware.md) for the current numbers), which is why the margin is
watched after every module rather than once.

## Kernel side: an IIO device

`kernel/adc.c` -> `v003-adc.ko`, an IIO device in direct mode with one
`iio_chan_spec` per channel (0..7 external, 8 Vref, 9 Vcal), `read_raw()` doing
one conversion per read and the scale as `AVDD/1024` (`IIO_VAL_FRACTIONAL`, with
`avdd_mv` a module parameter because the driver cannot measure the board's
supply).  There are no buffers and no triggers: the device cannot convert
anything by itself.

The completion tag is what the driver has to handle: it reads the tag, sends
`V003_ADC_START`, and polls until the tag moves, because the conversion runs in
the firmware's main loop and a fast reply would otherwise be the previous
result.  Measured through sysfs (`tests/adc_iio_test.py`, three clean runs):

    in_voltage8_raw = 361..363   -> 1163..1170 mV   (nominal 1200 mV)
    in_voltage9_raw = 511        -> 1647 mV         (2/4 AVDD = 1650 mV)
    in_voltage5_raw = in_voltage6_raw = 1023        (the USB pins, driven high)
    in_voltage0_scale = 3.22265625                  (3300/1024 mV)

The same counts come out of the raw pyusb test, which is the cross-check that
matters: two host paths, one protocol.  Loading needs `industrialio` first
(`insmod` on the compressed `industrialio.ko.zst` fails, see
[kernel.md](kernel.md)).

## Open

- There is no way to put a channel's pin into the chip's analog input mode from a
  host: the GPIO module offers pull-up/pull-down input and push-pull output, and
  the ADC deliberately configures nothing.  A command that sets a pin's mode to
  analog (or an `ADC_START` variant that does it for the channel's pad) would be
  the way to do it, and it would want to undo it again afterwards - see the two
  failures in the "Pins" section above before adding one.
- The full channel to pin table, which needs the CH32V003 datasheet (only the
  reference manual and the EVT package are in `hardware-docs/`).
