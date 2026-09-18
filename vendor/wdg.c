#include "ch32fun.h"
#include "rv003usb.h"

#include "wdg.h"

/*
 * Independent watchdog (IWDG), see wdg.h for the protocol and chapter 9 of the
 * reference manual for the hardware.  Sequence, exactly as WCH's own example
 * does it: 0x5555 enables write access to the prescaler and reload registers,
 * 0xaaaa reloads the counter (and is also the feed), 0xcccc starts it.
 *
 * Nothing here needs a pin or a buffer: the whole module is three registers and
 * a handful of bytes of state.
 */

static const u16 wdg_presc[7] = { 4, 8, 16, 32, 64, 128, 256 };

/* what the device is running with, for V003_WDG_GET_STATE */
static volatile u16 wdg_timeout_ms;
static volatile u8 wdg_running;

/* the reset flags, captured once at boot: reading them later would report the
 * flags of whatever happened in between */
static volatile u32 wdg_rst_flags;

#define WDG_RELOAD_MAX 0x0fff

void wdg_reset_cause_capture(void)
{
	u32 rsts = RCC->RSTSCKR;
	u32 flags = 0;

	if (rsts & RCC_PINRSTF)
		flags |= V003_WDG_RST_PIN;
	if (rsts & RCC_PORRSTF)
		flags |= V003_WDG_RST_POR;
	if (rsts & RCC_SFTRSTF)
		flags |= V003_WDG_RST_SOFT;
	if (rsts & RCC_IWDGRSTF)
		flags |= V003_WDG_RST_IWDG;
#ifdef RCC_WWDGRSTF
	if (rsts & RCC_WWDGRSTF)
		flags |= V003_WDG_RST_WWDG;
#endif
	if (rsts & RCC_LPWRRSTF)
		flags |= V003_WDG_RST_LPWR;

	wdg_rst_flags = flags;

	/* clear, so that a host asking later gets the cause of the *last* reset
	 * rather than a mixture */
	RCC->RSTSCKR |= RSTSCKR_RMVF_Set;
}

u32 wdg_reset_cause(void)
{
	return wdg_rst_flags;
}

void handle_wdg_out_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_WDG_START: {
		/* counter ticks = timeout * LSI / prescaler, in units of 1000 */
		u32 want = (u32)data * V003_WDG_LSI_HZ;
		u8 p;

		for (p = 0; p < 7; p++) {
			u32 ticks = want / (1000u * wdg_presc[p]);

			if (ticks <= WDG_RELOAD_MAX)
				break;
		}

		if (p == 7) {
			/* longer than the hardware can do: use the maximum */
			p = 6;
		}

		{
			u32 ticks = want / (1000u * wdg_presc[p]);
			u32 actual;

			if (ticks < 1)
				ticks = 1;
			if (ticks > WDG_RELOAD_MAX)
				ticks = WDG_RELOAD_MAX;

			IWDG->CTLR = 0x5555; /* write access to PSCR/RLDR */
			IWDG->PSCR = p;
			IWDG->RLDR = ticks;
			IWDG->CTLR = 0xaaaa; /* reload */
			IWDG->CTLR = 0xcccc; /* start */

			/* report what the hardware will really do */
			actual = (ticks * wdg_presc[p] * 1000u) /
				 V003_WDG_LSI_HZ;
			wdg_timeout_ms = actual > 0xffff ? 0xffff
							 : (u16)actual;
			wdg_running = 1;
		}
		break;
	}
	case V003_WDG_FEED:
		if (data == V003_WDG_FEED_KEY && wdg_running)
			IWDG->CTLR = 0xaaaa;
		break;
	default:
		break;
	}
}

u32 handle_wdg_in_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_WDG_GET_STATE:
		return ((u32)wdg_timeout_ms << 8) | (wdg_running ? 1u : 0u);
	case V003_WDG_GET_RESET_CAUSE: {
		u32 flags = wdg_rst_flags;

		wdg_rst_flags = 0; /* answered once */
		return flags;
	}
	default:
		return 0;
	}
}
