#ifndef __ADC_H
#define __ADC_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_ADC_MODULE_ID 0x06
#define V003_ADC_CMD(cmd)  V003_CMD(cmd, V003_ADC_MODULE_ID)

/*
 * ADC1, one conversion per request.  No buffering, no DMA and no averaging: a
 * host that wants a stream asks repeatedly, which is honest for a link that
 * moves 21 kB/s while one conversion costs 42 us and two bytes.
 *
 * The converter is **10 bit**, not 12: RM 9.1 says the ADC is a 10 bit SAR with
 * an ADCCLK of at most 24 MHz, and RM 9.2.2 says the regular data register
 * holds a 10 bit value.  A reading is therefore 0..1023 and the input voltage
 * is reading * AVDD / 1024, with AVDD = 3.3 V on this board.  Scaling a reading
 * as 12 bit was the first attempt and it reported Vref as 294 mV instead of
 * 1.18 V (see notes/adc.md).
 *
 * Channels 0..7 are the external inputs, 8 is the internal reference and 9 the
 * internal calibration voltage (RM 9.2.2).  Those two are what make this module
 * verifiable without wiring anything up, and both were measured:
 *
 *   channel 8 (Vref, nominal 1.2 V)      365 counts = 1176 mV
 *   channel 9 (Vcal, 2/4 AVDD = 1650 mV) 511 counts = 1647 mV
 *   channel 9 (Vcal, 3/4 AVDD = 2475 mV) 767 counts = 2472 mV
 *
 * The Vcal level is selectable (V003_ADC_SET_CALVOL): 2/4 or 3/4 of AVDD, so a
 * host can command a change and watch channel 9 follow it - which is what
 * distinguishes a live reading from a cached one.  The selection is latched
 * when the converter powers up: writing CALVOL with the ADC running measured
 * no change at all, so the command cycles ADON and recalibrates.
 *
 * The pin of each external channel comes from the package table in the
 * datasheet, which this project does not have (only the reference manual and
 * the EVT package).  Two of them are known:
 *
 *   channel 1 = PA1  measured: PWM channel 2 drives that pin, and a conversion
 *                    of channel 1 reads 1023 counts at 100 % duty and 0 counts
 *                    at 0 % duty (scripts/adc_test.py)
 *   channel 2 = PC4  from WCH's own ADC example (`EVT/EXAM/ADC/ADC_DMA`), the
 *                    one pin configured as an analog input here
 *
 * Other external channels are accepted and convert whatever their pin carries.
 */

#define V003_ADC_CHANNELS 10
#define V003_ADC_BITS	  10
#define V003_ADC_MAX	  0x3ffu
#define V003_ADC_VREF_CHANNEL	8
#define V003_ADC_VCAL_CHANNEL	9
#define V003_ADC_VREF_MV	1200 /* nominal, measured 1177 mV */
#define V003_ADC_CALVOL_HALF	0    /* 2/4 AVDD */
#define V003_ADC_CALVOL_3Q	1    /* 3/4 AVDD */

/*
 * OUT, wValue = channel: convert it.  This is an OUT request on purpose - a
 * conversion plus the calibration a channel change needs takes tens of
 * microseconds, an order of magnitude over the ~4.5 us the USB interrupt has,
 * and doing it there fails the control transfer with EIO.  Main loop context
 * does it.
 */
#define V003_ADC_START V003_ADC_CMD(0x80)
/* IN -> ((sequence & 0xff) << 16) | (valid << 15) | the 10 bit result.  The
 * sequence advances with every conversion, so a host that knows the tag from
 * the previous read can tell a fresh result from a stale one (the same
 * completion tag lesson as V003_I2C_GET_RESULT). */
#define V003_ADC_GET V003_ADC_CMD(0x81)
/* IN -> (resolution << 16) | channel count */
#define V003_ADC_GET_INFO V003_ADC_CMD(0x82)
/* IN -> the number of conversions the main loop has done */
#define V003_ADC_GET_SEQ V003_ADC_CMD(0x83)
/* IN -> (error << 24) | (last conversion in us << 16) | (requests << 8) |
 * conversions.  "Requests" is what this module was handed and "conversions"
 * what actually ran, so a host can tell a lost or rejected request from a wrong
 * reading.  Error 1 = calibration timeout, 2 = end of conversion never came. */
#define V003_ADC_GET_STATUS V003_ADC_CMD(0x84)
/* OUT, wValue: 0 = calibration voltage 2/4 AVDD, 1 = 3/4 AVDD (RM 9.3 CTLR1
 * CALVOL).  Anything else selects 2/4. */
#define V003_ADC_SET_CALVOL V003_ADC_CMD(0x85)

#define ADC_ERR_NONE 0
#define ADC_ERR_CAL  1
#define ADC_ERR_EOC  2

extern void handle_adc_out_request(u16 cmd, u16 data);
extern u32 handle_adc_in_request(u16 cmd, u16 data);

#endif /* __ADC_H */
