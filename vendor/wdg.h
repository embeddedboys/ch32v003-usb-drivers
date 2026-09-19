#ifndef __WDG_H
#define __WDG_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_WDG_MODULE_ID 0x04
#define V003_WDG_CMD(cmd)  V003_CMD(cmd, V003_WDG_MODULE_ID)

/*
 * The independent watchdog, in three useful pieces: a timeout, a kick, and the
 * answer to "why did I just reboot".
 *
 * OUT, wValue = timeout in milliseconds: pick the smallest prescaler that fits
 * in the 12 bit reload register, set both, and start the counter.  This is a one
 * way door - the IWDG cannot be stopped once started (WCH hardware, not a
 * firmware choice) - so from here on something has to feed it or the chip
 * resets.  The actual timeout is reported by GET_STATE because the LSI is only
 * accurate to about +-20 % (32..40 kHz), which the requested value does not
 * reflect.
 */
#define V003_WDG_START V003_WDG_CMD(0x60)

/* OUT: reload the counter, wValue = 0xaaaa (the key) - a wrong key feeds
 * nothing, so a host cannot kick the watchdog by accident */
#define V003_WDG_FEED V003_WDG_CMD(0x61)
#define V003_WDG_FEED_KEY 0xaaaa

/* IN -> (actual timeout in ms << 8) | (running ? 1 : 0).  Running is what the
 * firmware knows: after a reset the IWDG is off again, so this is not a lie. */
#define V003_WDG_GET_STATE V003_WDG_CMD(0x62)

/* IN -> reset cause flags (V003_WDG_RST_*), and clears them so the next read
 * answers for the next reset */
#define V003_WDG_GET_RESET_CAUSE V003_WDG_CMD(0x63)

#define V003_WDG_RST_PIN  (1u << 0)
#define V003_WDG_RST_POR  (1u << 1)
#define V003_WDG_RST_SOFT (1u << 2)
#define V003_WDG_RST_IWDG (1u << 3)
#define V003_WDG_RST_WWDG (1u << 4)
#define V003_WDG_RST_LPWR (1u << 5)

/* the LSI the IWDG counts, nominally 40 kHz */
#define V003_WDG_LSI_HZ 40000

extern void handle_wdg_out_request(u16 cmd, u16 data);
extern u32 handle_wdg_in_request(u16 cmd, u16 data);
/* call once at boot, before anything can look at the reset flags */
extern void wdg_reset_cause_capture(void);
extern u32 wdg_reset_cause(void);
/* what the low power module needs to know: the IWDG keeps counting while the
 * core sleeps, so a sleep that outlasts it is a reset in the middle of the night */
extern u8 wdg_is_running(void);
extern u16 wdg_timeout_get(void);

#endif /* __WDG_H */
