#include "ch32fun.h"
#include "rv003usb.h"

#include "pwm.h"

/*
 * PWM on TIM1, channels 1 and 2 (PD2 and PA1 in the default pin mapping, see
 * pwm.h).  The timer runs from APB2 at the core clock, so a period in
 * nanoseconds is cycles = period_ns * 48 / 1000 and the counter is 16 bits:
 * pick the prescaler that makes the reload fit, then the compare value from the
 * duty in permille.
 */

#define PWM_CHANNELS 2

static struct v003_pwm_cfg pwm_cfg[PWM_CHANNELS];
static volatile u8 pwm_on;

#define PWM_TIMER_HZ FUNCONF_SYSTEM_CORE_CLOCK

const struct v003_pwm_cfg *pwm_channel_state(u8 channel)
{
	if (channel >= PWM_CHANNELS)
		return 0;

	return &pwm_cfg[channel];
}

static void pwm_hw_setup(void)
{
	if (pwm_on)
		return;

	RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOD |
			  RCC_APB2Periph_TIM1;

	/* both channels as alternate function push-pull outputs */
	funPinMode(V003_PWM_PIN_CH1, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF);
	funPinMode(V003_PWM_PIN_CH2, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF);

	/* PWM mode 1 on channels 1 and 2: the output is high while the counter
	 * is below the compare value.  WCH names these registers CHCTLR1/2
	 * (STM32 calls them CCMR1/2), and each channel occupies the high byte of
	 * an 8 bit field OCM + the preload enable bit. */
	TIM1->CCER = 0;
	TIM1->CHCTLR1 = (6 << 4) | TIM_OCPreload_Enable |
			(6 << 12) | (TIM_OCPreload_Enable << 8);
	TIM1->CCER = TIM_CC1E | TIM_CC2E;
	TIM1->CTLR1 = TIM_ARPE;
	/* an advanced timer only drives its outputs with the main output enable
	 * set, which is why this is here and not in the SPI code */
	TIM1->BDTR |= TIM_MOE;
	TIM1->SWEVGR = TIM_UG; /* load the shadow registers */
	TIM1->CTLR1 |= TIM_CEN;

	pwm_on = 1;
}

static void pwm_apply(const struct v003_pwm_cfg *cfg)
{
	u32 cycles, arr, ccr, presc = 0;
	struct v003_pwm_cfg *state = &pwm_cfg[cfg->channel];
	u32 period_ns = cfg->period_ns;

	if (!period_ns)
		period_ns = 1000000; /* a sane default: 1 kHz */

	/* cycles = period_ns * (core clock) / 1e9.  PWM_TIMER_HZ / 1000000 is
	 * cycles per microsecond (48), so this is the same sum written so that
	 * nothing overflows a u32 and no 64 bit division (about 1.5 kB of libgcc)
	 * comes along.  Getting that constant wrong by a factor of 1000 made a
	 * 1 ms request run at 1 s - which the read back caught and a pin level
	 * check could not. */
	cycles = (period_ns / 1000u) * (PWM_TIMER_HZ / 1000000u);
	cycles += (period_ns % 1000u) * (PWM_TIMER_HZ / 1000000u) / 1000u;
	if (cycles < 2)
		cycles = 2;

	/* the prescaler register is 16 bit, the reload is 16 bit as well */
	presc = cycles / 65536;
	if (presc > 65535)
		presc = 65535;

	arr = cycles / (presc + 1);
	if (arr < 1)
		arr = 1;
	if (arr > 65535)
		arr = 65535;

	ccr = arr * cfg->duty_permille / 1000u;
	if (ccr > arr)
		ccr = arr;

	pwm_hw_setup();

	/* a prescaler change only takes effect on an update event */
	TIM1->PSC = presc;
	TIM1->ATRLR = arr;
	if (cfg->channel == 0)
		TIM1->CH1CVR = ccr;
	else
		TIM1->CH2CVR = ccr;
	TIM1->SWEVGR = TIM_UG;

	/* report what the hardware really does */
	state->channel = cfg->channel;
	state->enable = cfg->enable;
	state->duty_permille = cfg->duty_permille;
	state->period_ns = cfg->period_ns;
	{
		/* actual = arr * (presc + 1) cycles.  In microseconds first (one
		 * cycle is 1000/48 ns), and clamped to the four seconds a u32 of
		 * nanoseconds can hold: multiplying first would overflow, which is
		 * how this returned 72 ns for a 1 ms period. */
		u32 cyc = arr * (presc + 1);
		u32 us = cyc / (PWM_TIMER_HZ / 1000000u);

		state->actual_ns = us > 4000000u ? 4000000000u : us * 1000u;
	}

	if (!cfg->enable) {
		/* duty 0 % keeps the pin low, which is what a disabled channel
		 * should look like without giving up the timer */
		if (cfg->channel == 0)
			TIM1->CH1CVR = 0;
		else
			TIM1->CH2CVR = 0;
	}
}

void pwm_handle_control_data(const u8 *data, int len)
{
	struct v003_pwm_cfg cfg;

	if (len < (int)sizeof(cfg)) {
		LogUEvent(0xdead0001, len, 0, 0);
		return;
	}

	memcpy(&cfg, data, sizeof(cfg));
	if (cfg.channel >= PWM_CHANNELS) {
		LogUEvent(0xdead0002, cfg.channel, 0, 0);
		return;
	}

	pwm_apply(&cfg);
}

u32 handle_pwm_in_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_PWM_GET_INFO:
		return PWM_CHANNELS;
	default:
		return 0;
	}
}
