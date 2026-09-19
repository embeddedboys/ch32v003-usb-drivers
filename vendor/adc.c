#include "ch32fun.h"
#include "rv003usb.h"

#include "adc.h"

/*
 * One conversion per request, on ADC1.  Everything here happens in main()
 * context, so the sampling loop can take the ~42 us a conversion needs without
 * bothering the USB interrupt.
 */

static volatile u8 adc_on;
static volatile u8 adc_channel = 0xff;
/* the last conversion, done in main() and read back by an IN request */
static volatile u32 adc_seq;
static volatile u16 adc_value;
static volatile u8 adc_valid;

/* Diagnostics, so a host can tell a wrong reading from a lost request:
 * adc_reqs counts the OUT requests this module was handed and adc_seq the
 * conversions that actually ran (they differ when a request carried an out of
 * range channel), adc_us is how long the last conversion took - the main loop
 * is blocked for that long - and adc_err records the step that timed out. */
static volatile u8 adc_reqs;
static volatile u8 adc_err;
static volatile u16 adc_us;

/* The external channels' pins come from the package table in the datasheet, which
 * this project does not have: WCH's own ADC example uses PC4 for ADC_IN2, and
 * ADC_IN1 is PA1 (measured, vendor/adc.h).
 *
 * **This module configures no pin at all.**  It did once: the first conversion
 * put PC4 into analog mode, which took it away from the SPI module's chip select,
 * and because the SPI module only writes the output data register for its select,
 * the select stopped moving silently while the clocking carried on (measured: PC4
 * reported itself as an output after `SPI_SET_SELECT`, and as an input after one
 * conversion of the internal reference - a test that checks the data still
 * passed).  Configuring it per channel instead was worse: converting channel 1
 * turned PA1 into an analog input mid-test and killed the PWM output on the very
 * same pad (`pwm_test` and `adc_test` failed immediately).
 *
 * So the rule is the caller's to decide: the converter reads the pad whatever
 * mode it is in - a driven pin reads its driven level, a floating one reads
 * whatever it floats to - and a host that wants the datasheet's analog mode sets
 * it through the GPIO module, where it is visible and reversible.  A conversion
 * only ever touches the ADC's own registers.
 */
#define ADC_PIN_CH2 PC4 /* channel 2, for the record: this module does not touch it */
#define ADC_PIN_CH1 PA1 /* channel 1 */
#define V003_ADC_PIN_CH2_PIN 36

/* CTLR1 CALVOL[1:0]: 01 = 2/4 AVDD, 10 = 3/4 AVDD, anything else invalid
 * (RM 9.3).  Reset leaves it at 00, so it has to be written. */
#define ADC_CALVOL_HALF	 (1u << 25)
#define ADC_CALVOL_THREEQ (2u << 25)
#define ADC_CALVOL_MASK	 (3u << 25)

/* Reset calibration + calibration, both bounded: a hardware fault must not hang
 * the main loop, which is what owes the host its zero length control OUTs. */
static u8 adc_calibrate(void)
{
	u32 guard;

	ADC1->CTLR2 |= ADC_RSTCAL;
	guard = 100000;
	while ((ADC1->CTLR2 & ADC_RSTCAL) && --guard)
		;
	if (!guard) {
		adc_err = ADC_ERR_CAL;
		return 0;
	}

	ADC1->CTLR2 |= ADC_CAL;
	guard = 100000;
	while ((ADC1->CTLR2 & ADC_CAL) && --guard)
		;
	if (!guard) {
		adc_err = ADC_ERR_CAL;
		return 0;
	}

	return 1;
}

/*
 * ADCCLK = HBCLK / 8 = 6 MHz, well inside the 24 MHz the manual allows, and the
 * conversion time the module reports is measured against this divider.
 *
 * It is written before *every* conversion, not once at setup, because the power
 * module's standby wake calls `SystemInit()`, which rewrites RCC->CFGR0 and
 * clears ADCPRE back to its reset value (HBCLK/2 = 24 MHz).  Measured: 42 us on a
 * fresh boot and 11 us after a sleep - correct readings either way, since 24 MHz
 * is legal, but the reported conversion time would be a lie and the number in
 * adc.h would no longer describe the device.
 */
static void adc_clock_setup(void)
{
	RCC->CFGR0 &= ~RCC_ADCPRE;
	RCC->CFGR0 |= RCC_ADCPRE_DIV8;
}

static void adc_hw_setup(void)
{
	if (adc_on)
		return;

	RCC->APB2PCENR |= RCC_APB2Periph_ADC1 | RCC_APB2Periph_GPIOC;

	adc_clock_setup();

	/* reset the peripheral so no register keeps a state from before */
	RCC->APB2PRSTR |= RCC_APB2Periph_ADC1;
	RCC->APB2PRSTR &= ~RCC_APB2Periph_ADC1;

	/* software trigger for the regular group: EXTSEL = 111, the value
	 * ch32fun's adc_polled example and WCH's ADC_ExternalTrigConv_None both
	 * use */
	ADC1->RSQR1 = 0; /* one conversion in the regular sequence */
	ADC1->RSQR2 = 0;
	ADC1->CTLR1 = ADC_CALVOL_HALF;
	ADC1->CTLR2 = ADC_ADON | ADC_EXTSEL;

	/* Calibration, after the ADC has been on for at least two ADCCLK
	 * periods (RM 9.2.2); the register writes above take far longer than
	 * that.  The manual asks for it on every power up. */
	adc_calibrate();
	/* the two internal channels need their buffer enabled */
	ADC1->CTLR2 |= ADC_TSVREFE;

	adc_on = 1;
}

static u32 adc_convert(u8 channel)
{
	u32 guard;
	u32 t0;

	adc_hw_setup();
	adc_clock_setup();

	/* sample time 7 = 241 ADCCLK, the longest one, which is what a floating
	 * internal channel needs: TCONV = 241 + 11 = 252 ADCCLK = 42 us at
	 * 6 MHz, the number V003_ADC_GET_STATUS reports */
	ADC1->SAMPTR2 &= ~(7u << (channel * 3));
	ADC1->SAMPTR2 |= 7u << (channel * 3);
	ADC1->RSQR3 = channel;

	if (adc_channel != channel) {
		adc_channel = channel;
		/* a channel change needs a new calibration before the next
		 * conversion; this is what makes a channel switch cost three
		 * conversion times instead of one */
		adc_calibrate();
	}

	t0 = SysTick->CNT;
	ADC1->CTLR2 |= ADC_SWSTART;
	guard = 100000;
	while (!(ADC1->STATR & ADC_EOC) && --guard)
		;
	adc_us = (u16)((SysTick->CNT - t0) / 48); /* SysTick counts CPU cycles */

	if (!guard) {
		adc_err = ADC_ERR_EOC;
		return 0xffffffff;
	}

	/* reading the data register clears EOC */
	return ADC1->RDATAR & V003_ADC_MAX;
}

/* main loop context: one conversion, its result kept for V003_ADC_GET */
void handle_adc_out_request(u16 cmd, u16 data)
{
	u32 raw;

	if (cmd == V003_ADC_SET_CALVOL) {
		/* 0 = 2/4 AVDD, 1 = 3/4 AVDD; a host can command the change and
		 * watch channel 9 follow it, which is what proves the reading
		 * is live rather than stuck.  The selection is part of what the
		 * ADC latches at power up, so it takes an ADON cycle and a new
		 * calibration (writing it with the converter running measured
		 * no change at all). */
		adc_hw_setup();
		ADC1->CTLR2 &= ~ADC_ADON;
		ADC1->CTLR1 = data ? ADC_CALVOL_THREEQ : ADC_CALVOL_HALF;
		ADC1->CTLR2 |= ADC_ADON;
		adc_calibrate();
		ADC1->CTLR2 |= ADC_TSVREFE;
		return;
	}

	if (cmd != V003_ADC_START)
		return;

	adc_reqs++;

	if (data >= V003_ADC_CHANNELS)
		return;

	raw = adc_convert((u8)data);
	/* 0xffffffff is the "end of conversion never arrived" answer: publish it
	 * as invalid rather than as the 1023 counts it would otherwise look
	 * like, and still advance the tag so a host waiting for completion does
	 * not hang on it */
	adc_valid = raw != 0xffffffff;
	adc_value = raw & V003_ADC_MAX;
	adc_seq++;
}

u32 handle_adc_in_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_ADC_GET:
		return ((u32)(adc_seq & 0xff) << 16) | (adc_valid ? (1u << 15) : 0) |
		       (adc_value & V003_ADC_MAX);
	case V003_ADC_GET_SEQ:
		return adc_seq;
	case V003_ADC_GET_INFO:
		return ((u32)V003_ADC_BITS << 16) | V003_ADC_CHANNELS;
	case V003_ADC_GET_STATUS:
		/* (error << 24) | (last conversion us) << 16 |
		 * (requests) << 8 | conversions */
		return ((u32)adc_err << 24) | ((u32)adc_us << 16) |
		       ((u32)(adc_reqs & 0xff) << 8) | (adc_seq & 0xff);
	default:
		return 0;
	}
}
