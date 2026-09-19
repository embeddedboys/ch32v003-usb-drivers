#include "ch32fun.h"
#include "rv003usb.h"

#if V003_MODULE_WDG
#include "wdg.h"
#endif
#if V003_MODULE_UART
#include "uart.h"
#endif

#include "pwr.h"

/*
 * Low power modes; pwr.h has the design and the rules.  Everything happens in
 * main() context: the request only records what to do, and pwr_poll() starts the
 * sleep once the control transfer that carried it is over.
 */

/* LSI / prescaler into the AWU window.  AWUPSC[3:0] per RM 2.4.5. */
static const u16 pwr_awu_div[16] = { 1,	1,    2,	 4,	8,    16,   32,	 64,
				     128, 256,	512, 1024, 2048, 4096, 10240, 61440 };

static volatile u8 pwr_pending;
/* set from the USB stack's EXTI handler when the button's line is the one that
 * fired; see vendor/usb_config.h for why the handler has to be told about it */
static volatile u8 pwr_button_woke;
static u32 pwr_arm_at;
static struct v003_pwr_arm pwr_req;
static struct v003_pwr_state pwr_state;

const struct v003_pwr_state *pwr_state_ptr(void)
{
	return &pwr_state;
}

/* microseconds per AWU tick: div * 1e6 / 128000, written as div * 125 / 16 so
 * that the coarsest divider (61440) cannot overflow a u32 */
static u32 pwr_tick_us(u8 code)
{
	return (u32)pwr_awu_div[code] * 125u / 16u;
}

/*
 * Pick the finest prescaler whose 63 tick window still covers the request, then
 * the number of ticks that comes closest to it.  Returns the nominal duration in
 * milliseconds; *code and *ticks are what the registers get.
 */
static u32 pwr_awu_program(u32 ms, u8 *code, u8 *ticks)
{
	u32 us = ms * 1000u;
	u32 tick_us;
	u32 n;
	u8 i;

	for (i = 0; i < 16; i++) {
		tick_us = pwr_tick_us(i);
		if (tick_us * V003_PWR_AWU_TICKS_MAX >= us)
			break;
	}

	if (i == 16)
		i = 15;

	tick_us = pwr_tick_us(i);
	n = us / tick_us;
	if (!n)
		n = 1;
	if (n > V003_PWR_AWU_TICKS_MAX)
		n = V003_PWR_AWU_TICKS_MAX;

	*code = i;
	*ticks = (u8)n;

	return tick_us * n / 1000u;
}

/* The EXTI line the boot button is on.  Lines 0..7 exist and each is the pin
 * index within its port, so PD6 is line 6 - the flat pin number (54) is a
 * different numbering and using it here would address line 22, which is not a
 * line at all. */
#define PWR_BTN_LINE (1u << (V003_PIN_BOOT_BTN & 0xf))

/*
 * Called from the USB stack's EXTI handler (vendor/usb_config.h points
 * RV003_ADD_EXTI_HANDLER here) when the button's line is the one that fired.  It
 * runs inside that handler, so it does nothing but remember it: the assembly
 * clears the pending bits on the way out.
 */
void pwr_button_edge(void)
{
	pwr_button_woke = 1;
}

static void pwr_button_arm(void)
{
	/* the boot button pulls the pin low when it is pressed, so it needs a
	 * pull-up and a falling edge */
	funPinMode(V003_PIN_BOOT_BTN, GPIO_Speed_In | GPIO_CNF_IN_PUPD);
	funDigitalWrite(V003_PIN_BOOT_BTN, FUN_HIGH);

	/* AFIO_EXTICR maps EXTI lines 0..7 to ports, two bits each.  rv003usb
	 * writes the whole register for its own D- line, so this has to *or* -
	 * and it has to happen after usb_setup() for the same reason. */
	AFIO->EXTICR &= ~(0x3u << (6 * 2));
	AFIO->EXTICR |= (GPIO_PortSourceGPIOD << (6 * 2));

	pwr_button_woke = 0; /* a press from before this sleep does not count */
	EXTI->INTENR |= PWR_BTN_LINE;
	EXTI->FTENR |= PWR_BTN_LINE;
	EXTI->INTFR = PWR_BTN_LINE;
}

static void pwr_button_disarm(void)
{
	EXTI->INTENR &= ~PWR_BTN_LINE;
	EXTI->FTENR &= ~PWR_BTN_LINE;
	EXTI->INTFR = PWR_BTN_LINE;
	funPinMode(V003_PIN_BOOT_BTN, GPIO_Speed_In | GPIO_CNF_IN_PUPD);
	funDigitalWrite(V003_PIN_BOOT_BTN, FUN_HIGH);
}

/*
 * Arm the wake timer.  This is never optional: even a request that asks to be
 * woken by the button gets the AWU as well, so no sleep can outstay its welcome
 * and leave a device nobody can reach (in standby the debug interface is off, so
 * a lost device means a power cycle).
 */
static u32 pwr_awu_arm(u32 ms, u8 *ticks)
{
	u8 code;

	u32 nominal;

	RCC->APB1PCENR |= RCC_APB1Periph_PWR;
	RCC->RSTSCKR |= RCC_LSION;
	while (!(RCC->RSTSCKR & RCC_LSIRDY))
		;

	/* the AWU is wired to EXTI line 9, as an event: no interrupt handler and
	 * no PFIC enable needed, it only wakes the core */
	EXTI->EVENR |= EXTI_Line9;
	EXTI->FTENR |= EXTI_Line9;

	nominal = pwr_awu_program(ms, &code, ticks);
	pwr_state.last_div = code;
	PWR->AWUPSC = code;
	PWR->AWUWR = *ticks;
	PWR->AWUCSR |= PWR_AWUCSR_AWUEN;

	return nominal;
}

static void pwr_awu_disarm(void)
{
	PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN;
	EXTI->EVENR &= ~EXTI_Line9;
	EXTI->FTENR &= ~EXTI_Line9;
}

/*
 * The peripherals are deliberately left alone.  RM 2.3.2 is right that a sleep
 * costs more with every unused peripheral clock still running, but turning off
 * a module the host configured (SPI, the timer, the USART) would mean
 * reconfiguring it after the wake, and there is no way to *measure* the power it
 * saves on this bench - the board is powered over USB and there is no current
 * meter.  Trading a silent behaviour change for an unmeasurable gain is the
 * wrong way round; a host that wants a quiet device disables the modules it is
 * not using.  What does have to be handled is that the clocks are back on HSI
 * after standby, which SystemInit() and the SysTick re-arm below do.
 */
/*
 * There is no way for the firmware to measure a sleep: the core clock stops and
 * takes SysTick with it, and the peripheral timers stop as well (measured on this
 * part: TIM2 advanced by the 10 ms before the WFE and not at all during a 500 ms
 * sleep).  What a host can do instead is time the answer itself, which is what
 * scripts/pwr_test.py does.
 */

/* One sleep.  Never returns early: the AWU backstop is always armed. */
static void pwr_sleep(void)
{
	u8 ticks = 0;
	u32 nominal;
	u32 pending;
#if V003_MODULE_UART
	u32 uart_mark;
#endif
	u8 reason;

	nominal = pwr_awu_arm(pwr_req.duration_ms, &ticks);

	if (pwr_req.wake & V003_PWR_WAKE_BUTTON)
		pwr_button_arm();

#if V003_MODULE_UART
	/* any enabled interrupt ends a sleep, so what happened on the port is what
	 * gets compared - see vendor/pwr.h */
	uart_mark = uart_activity_count();
#endif

	if (pwr_req.detach) {
		/* a host that is not expecting a sleeping device is happier with
		 * a clean disconnect: the pull-up goes away and the device
		 * re-enumerates when it comes back */
		v003_usb_detach();
		/* D- is not a wake source while we are off the bus: a host that
		 * keeps sending SOFs to the port would otherwise wake us every
		 * millisecond, which is not sleeping at all */
		EXTI->INTENR &= ~(1u << USB_PIN_DM);
	}

	if (pwr_req.mode == V003_PWR_MODE_STANDBY) {
		PWR->CTLR |= PWR_CTLR_PDDS;
		PFIC->SCTLR |= (1u << 2); /* SLEEPDEEP */
	} else {
		PWR->CTLR &= ~PWR_CTLR_PDDS;
		PFIC->SCTLR &= ~(1u << 2);
	}

	/* WFE, not WFI: the AWU is an EXTI *event* and events wake WFE (RM 2.3.1).
	 * A stale event would return immediately, so the pending bits are cleared
	 * first - and if the wake reason comes back as "other" the host can see
	 * that it happened. */
	EXTI->INTFR = EXTI->INTFR & (PWR_BTN_LINE | (1u << USB_PIN_DM));

	__WFE();

	/* back: in standby the clock is HSI now, in sleep it never changed */
	SystemInit();

	pending = EXTI->INTFR;
	EXTI->INTFR = pending;

	if (pwr_button_woke)
		reason = V003_PWR_WOKE_BUTTON;
#if V003_MODULE_UART
	else if (uart_activity_count() != uart_mark)
		reason = V003_PWR_WOKE_UART;
#endif
	else
		reason = V003_PWR_WOKE_AWU;

	pwr_button_disarm();
	pwr_awu_disarm();

	/* SysTick lost its configuration with the core clock: it is set up in
	 * handle_reset(), which does not run again after an AWU wake, and the
	 * entire firmware measures time with SysTick->CNT */
	SysTick->CTLR = 5;

	/* back on the bus: the pull-up, a fresh USB setup and endpoint toggles at
	 * DATA0/1 - the host has re-enumerated if we detached, and if we did not
	 * it resets the device after the silence */
	v003_usb_attach();

	pwr_state.sleep_count++;
	pwr_state.awu_ticks = ticks;
	pwr_state.nominal_ms = nominal;
	pwr_state.last_mode = pwr_req.mode;
	pwr_state.last_wake = reason;
	pwr_state.wake_reason = reason;
	pwr_state.last_detached = pwr_req.detach;
	pwr_state.armed = 0;
	pwr_pending = 0;
}

void pwr_poll(void)
{
	if (!pwr_pending)
		return;

	if ((SysTick->CNT - pwr_arm_at) <
	    (u32)(pwr_req.delay_ms ? pwr_req.delay_ms
				   : V003_PWR_DEFAULT_DELAY_MS) *
		    48000u)
		return;

	pwr_sleep();
}

void pwr_handle_control_data(u16 cmd, const u8 *data, int len)
{
	struct v003_pwr_arm req;

	if (cmd != V003_PWR_ARM)
		return;

	if (len < (int)sizeof(req)) {
		LogUEvent(0xdead0020, len, 0, 0);
		return;
	}

	memcpy(&req, data, sizeof(req));

	/* one at a time: a host that arms twice without reading the state gets a
	 * refusal it can see instead of a queue it cannot */
	if (pwr_pending) {
		pwr_state.last_refused = V003_PWR_REF_BUSY;
		return;
	}

	if (req.mode > V003_PWR_MODE_STANDBY) {
		pwr_state.last_refused = V003_PWR_REF_BAD_MODE;
		return;
	}

	if (req.wake & ~V003_PWR_WAKE_BUTTON) {
		pwr_state.last_refused = V003_PWR_REF_BAD_MODE;
		return;
	}

	if (req.duration_ms < V003_PWR_MIN_MS ||
	    req.duration_ms > V003_PWR_MAX_MS) {
		pwr_state.last_refused = V003_PWR_REF_RANGE;
		return;
	}

#if V003_MODULE_WDG
	/* the IWDG keeps counting while the core is asleep, so a sleep that
	 * outlasts it is a reset in the middle of the night rather than a nap */
	if (wdg_is_running() && req.duration_ms + 100 >= wdg_timeout_get()) {
		pwr_state.last_refused = V003_PWR_REF_WDG;
		return;
	}
#endif

	pwr_req = req;
	pwr_state.last_refused = V003_PWR_OK;
	pwr_state.requested_ms = req.duration_ms;
	pwr_state.armed = 1;
	pwr_arm_at = SysTick->CNT;
	pwr_pending = 1;
}

u32 handle_pwr_in_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_PWR_GET_INFO:
		return ((u32)V003_PWR_MAX_MS << 16) | V003_PWR_MIN_MS;
	default:
		return 0;
	}
}

/* called once at boot: fill in what this build can do, so a host can decide
 * before it asks for anything */
void pwr_init(void)
{
	u32 caps = V003_PWR_CAP_SLEEP | V003_PWR_CAP_STANDBY |
		   V003_PWR_CAP_BUTTON | V003_PWR_CAP_DETACH;

	pwr_state.caps = caps;
	pwr_state.last_refused = V003_PWR_OK;
}
