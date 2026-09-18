#ifndef __PWM_H
#define __PWM_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_PWM_MODULE_ID 0x05
#define V003_PWM_CMD(cmd)  V003_CMD(cmd, V003_PWM_MODULE_ID)

/*
 * PWM out of TIM1.  The CH32V003 has no PA8..PA11, so the channels live on the
 * pins the remap table gives them; in the default mapping (TIM1_RM=00, which is
 * what the reset value selects) that is
 *
 *     CH1 = PD2, CH2 = PA1, CH3 = PC3, CH4 = PC4
 *
 * Two of those collide with how this board is wired - PC3 is used as a spare
 * GPIO and PC4 as the SPI chip select - so this firmware drives the first two,
 * PD2 and PA1, and reports them as reserved pins like every other module pin.
 *
 * OUT with a data stage: struct v003_pwm_cfg, i.e. a period in nanoseconds and a
 * duty in permille, which is what the Linux PWM API hands a driver.  The
 * firmware picks the prescaler and the reload value itself and keeps what the
 * hardware ended up with, so a host can read it back with V003_PWM_GET.
 */
#define V003_PWM_SET	 V003_PWM_CMD(0x70)
/* IN with a data stage, wValue = channel: the configuration the hardware has */
#define V003_PWM_GET	 V003_PWM_CMD(0x71)
/* IN -> number of channels (also in the capability report) */
#define V003_PWM_GET_INFO V003_PWM_CMD(0x72)

/* The pins of the channels and the channel count live in vendor.h next to the
 * reserved pin mask, because that mask has to reference them even in builds
 * where this module is switched off (a ternary in a macro does not drop the
 * identifier, only an #if would, and that cannot be used inside an initializer).
 * See vendor.h: V003_PWM_PIN_CH1/CH2 and V003_PWM_CHANNELS. */

/*
 * period_ns is a u32, so the longest period this protocol can express is about
 * 4.29 s.  The timer itself could do ~89 s (16 bit prescaler and reload at
 * 48 MHz), but reaching it would mean 64 bit arithmetic and about 1.5 kB of
 * libgcc soft division for a corner case nobody asks a PWM bridge for; two
 * seconds is plenty for a fan, a backlight or a servo.
 */
#define V003_PWM_MAX_PERIOD_NS 4000000000u

struct v003_pwm_cfg {
	u8 channel;
	u8 enable;
	u16 duty_permille;
	u32 period_ns;	 /* requested */
	u32 actual_ns;	 /* what the timer really does, filled in by GET */
};

extern void pwm_handle_control_data(const u8 *data, int len);
extern u32 handle_pwm_in_request(u16 cmd, u16 data);
extern const struct v003_pwm_cfg *pwm_channel_state(u8 channel);

#endif /* __PWM_H */
