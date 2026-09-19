#ifndef __PWR_H
#define __PWR_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_PWR_MODULE_ID 0x08
#define V003_PWR_CMD(cmd)  V003_CMD(cmd, V003_PWR_MODULE_ID)

/*
 * Low power modes.  This is the one module that changes whether the device
 * exists as far as the host is concerned: CH32V003 low speed USB is bit banged,
 * so a sleeping core cannot answer anything (RM 2.3).
 *
 * Two modes, and the difference matters to a host:
 *
 *   SLEEP    the core stops, every peripheral clock and every IO keeps running,
 *            wake is the fastest, and the device stays *attached* but silent:
 *            the host sees control transfers time out for the duration.  For
 *            short pauses (tens of milliseconds) that is what you want.
 *   STANDBY  all high frequency clocks stop, SRAM and register contents and the
 *            IO states are kept, only the IWDG and the LSI keep running.  Wake
 *            is by an EXTI line, the AWU, NRST or an IWDG reset, and an AWU wake
 *            does **not** reset the chip - the firmware continues after the WFE
 *            with the clock back on HSI (which SystemInit() then takes back to
 *            48 MHz).  This driver releases the USB pull-up first, so the host
 *            sees a disconnect and a clean re-enumeration instead of a device
 *            that ignores it; the serial number comes from the factory id, so it
 *            is the same device coming back.
 *
 * Rules this module follows, all of them learned from what breaks otherwise:
 *
 *  - **Nothing sleeps unless a host asks.**  A firmware that sleeps by itself
 *    cannot be reprogrammed over SWIO while it does (the debug interface is off
 *    in standby), which is exactly the trap WCH's own examples warn about.
 *  - **Every sleep has an AWU backstop**, even when a wake pin is configured, so
 *    no request can park the device forever with no way back.
 *  - **The sleep starts a few milliseconds after the control transfer that asked
 *    for it**, from the main loop, or the host would never see its own reply.
 *  - **A sleep longer than the watchdog timeout is refused**, not silently
 *    executed: the IWDG keeps counting in standby and would reset the chip
 *    mid-sleep (the reset cause is readable afterwards through the WDG module).
 */

#define V003_PWR_MODE_SLEEP   0
#define V003_PWR_MODE_STANDBY 1

/* wake sources a request may ask for; the AWU is always armed as well */
#define V003_PWR_WAKE_AWU    0x00 /* backstop only: sleep for the whole duration */
#define V003_PWR_WAKE_BUTTON 0x01 /* the board's boot button, PD6, on a press */

/*
 * Why the last sleep ended.
 *
 * Three reasons are produced, and only three, because two attempts at naming a
 * fourth one were measured and did not work:
 *
 *  - **the button** is an EXTI interrupt on line 6, routed around the USB stack
 *    through the hook upstream provides for exactly this: rv003usb's EXTI7_0
 *    handler covers lines 0..7 and, without `RV003_ADD_EXTI_MASK`, would feed a
 *    button press to the USB state machine as if it were bus traffic.  With the
 *    hook (set in vendor/usb_config.h) the handler checks the pending bits, and
 *    when the USB line is not the cause it calls `pwr_button_edge()` instead and
 *    clears only the bits it owns.  An *event* line would avoid the handler
 *    altogether, and was the first attempt - it wakes the core, but an event-only
 *    line does not set INTFR, so the firmware could not say who had woken it
 *    (measured: a 20 s standby sleep ended early when the button was pressed and
 *    still reported AWU).
 *  - **the AWU** is the backstop, and it is what comes back when the wake was
 *    neither the button nor an incoming byte.
 *  - **UART activity** is what is left: in sleep mode every peripheral clock keeps
 *    running and *any* enabled interrupt ends the sleep, so a byte arriving on the
 *    receive pin and the transmitter draining its own ring both come out here.
 *    The distinction is not available (the receive interrupt has already cleared
 *    RXNE by the time the core runs again), and with no second UART peer on this
 *    bench the receive half cannot be produced on its own anyway.
 *
 * "The host talked to us during the sleep" cannot be told apart from these: the
 * USB handler consumes the EXTI pending bit before this code can read it (the
 * first attempt), and the USB stack's own activity marker (se0_windup) moves on
 * *any* bus traffic - including the SOFs a host sends to a device that is merely
 * attached - so comparing it before and after reports "USB" for every sleep, not
 * just the poked ones (the second attempt, measured: every quiet sleep came back
 * as USB activity).  A host that pokes a sleeping device still ends the sleep
 * early, and can see that by how quickly the device answered; what the firmware
 * cannot do is name it, and a field that is always wrong is worse than no field.
 */
#define V003_PWR_WOKE_AWU    0
#define V003_PWR_WOKE_BUTTON 1 /* the boot button was pressed */
#define V003_PWR_WOKE_UART   2 /* the port moved a byte, either direction */
#define V003_PWR_WOKE_OTHER  3 /* reserved: no case produces it today */

/* why a request was refused (0 = it was accepted) */
#define V003_PWR_OK	      0
#define V003_PWR_REF_BAD_MODE 1
#define V003_PWR_REF_RANGE    2
#define V003_PWR_REF_WDG      3
#define V003_PWR_REF_BUSY     4

/* what the firmware supports, as reported in struct v003_pwr_state.caps */
#define V003_PWR_CAP_SLEEP   (1u << 0)
#define V003_PWR_CAP_STANDBY (1u << 1)
#define V003_PWR_CAP_BUTTON  (1u << 2)
#define V003_PWR_CAP_DETACH  (1u << 3)

/* The AWU counts the internal ~128 kHz LSI through a prescaler into a six bit
 * comparison window, so a sleep is between one tick and 63 ticks of the coarsest
 * division.  The LSI is an uncalibrated RC oscillator (this project has no
 * datasheet for the part, only the reference manual), so the duration is
 * *nominal*: the host is told the ticks and measures the wall clock itself, the
 * same read-back-what-the-hardware-does rule as the PWM period and the baud rate.
 */
#define V003_PWR_LSI_HZ	      128000
#define V003_PWR_AWU_TICKS_MAX 63
#define V003_PWR_MIN_MS	      10
#define V003_PWR_MAX_MS	      30000
/* how long after the request the main loop sleeps; the control transfer's status
 * stage has to be over by then, and ~2-3 low speed frames is comfortable */
#define V003_PWR_DEFAULT_DELAY_MS 10

/* OUT with a data stage: arm a sleep.  It does not happen inside the request. */
struct v003_pwr_arm {
	u32 duration_ms; /* the requested sleep, clamped to the range above */
	u8 mode;	 /* V003_PWR_MODE_* */
	u8 wake;	 /* V003_PWR_WAKE_* */
	u8 detach;	 /* release the USB pull-up before sleeping (standby) */
	u8 delay_ms;	 /* 0 = V003_PWR_DEFAULT_DELAY_MS */
};
#define V003_PWR_ARM V003_PWR_CMD(0xa0)

/* IN with a data stage: what has happened, and what a sleep would do now */
struct v003_pwr_state {
	u32 sleep_count;     /* sleeps this boot has completed */
	u32 awu_ticks;	     /* ticks programmed for the last one */
	u32 requested_ms;    /* what the host asked for */
	u32 nominal_ms;	     /* what those ticks work out to at the nominal LSI */
	u32 caps;	     /* V003_PWR_CAP_* */
	/* Eight bytes, in this order, so the structure has no padding and a host
	 * can parse it with one format string. */
	u8 last_refused; /* V003_PWR_REF_*, 0 if the last request was accepted */
	u8 last_mode;
	u8 last_wake;	 /* V003_PWR_WOKE_* */
	u8 last_detached;
	u8 armed;	 /* a request is waiting for its start delay */
	u8 wake_reason;	 /* the same as last_wake, read after a wake */
	u8 last_div;	 /* the AWUPSC code used, so a host can work out the LSI:
			  * the ticks it slept and the time the host measured
			  * are what turn the nominal 128 kHz into a number */
	u8 pad;
};


#define V003_PWR_GET_STATE V003_PWR_CMD(0xa1)

/* IN -> (maximum ms << 16) | minimum ms, so a host can validate before asking */
#define V003_PWR_GET_INFO V003_PWR_CMD(0xa2)

/* call once at boot, before any request can be answered */
extern void pwr_init(void);
extern void pwr_handle_control_data(u16 cmd, const u8 *data, int len);
extern u32 handle_pwr_in_request(u16 cmd, u16 data);
extern const struct v003_pwr_state *pwr_state_ptr(void);
/* main loop context: runs a sleep once its start delay has passed */
extern void pwr_poll(void);

#endif /* __PWR_H */
