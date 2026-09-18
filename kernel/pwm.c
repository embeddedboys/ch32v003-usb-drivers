// SPDX-License-Identifier: GPL-2.0-only
/*
 * PWM child driver for the CH32V003 USB multi function device.
 *
 * The firmware drives TIM1 channels 1 and 2 (PD2 and PA1, the two of the four
 * that this board has free) and speaks in nanoseconds and permille, which is
 * what the Linux PWM API hands a driver: apply_state() gives a period and a duty
 * cycle in ns and this translates the duty, sends one vendor request, and reads
 * the result back so get_state() reports what the timer actually does rather
 * than what was asked for.
 *
 * The channel count comes from the device's capability report, so a firmware
 * built without the module gets no chip at all (and this driver would not probe
 * either, since the core only adds a cell for a capability the device has).
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#include "usb-mfd.h"

#define DRV_NAME "v003-pwm"

struct v003_pwm {
	struct v003_dev *v003;
};

static int v003_pwm_send(struct v003_pwm *priv, unsigned int channel,
			 const struct pwm_state *state)
{
	int ret;

	struct v003_pwm_cfg cfg = {
		.channel = channel,
		.enable = state->enabled ? 1 : 0,
		.period_ns = state->period,
	};

	/* the firmware takes permille, the API counts nanoseconds */
	if (state->period && state->duty_cycle >= state->period)
		cfg.duty_permille = 1000;
	else if (state->period)
		cfg.duty_permille = div_u64((u64)state->duty_cycle * 1000,
					    state->period);

	ret = v003_data_out(priv->v003, V003_PWM_SET, 0, &cfg, sizeof(cfg));

	/* v003_data_out() returns the number of bytes it sent on success, but
	 * .apply() has to return 0: passing 12 up made the PWM core report a
	 * failure for a write that worked */
	return ret < 0 ? ret : 0;
}

static int v003_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			  const struct pwm_state *state)
{
	struct v003_pwm *priv = pwmchip_get_drvdata(chip);
	struct pwm_state now;

	/* polarity is not something the firmware can change (it would mean a
	 * second compare register convention), so refuse rather than lie */
	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EOPNOTSUPP;

	if (!state->enabled) {
		/* parking the channel means duty 0, and the firmware keeps the
		 * configured period while it does so */
		now = *state;
		now.duty_cycle = 0;
		return v003_pwm_send(priv, pwm->hwpwm, &now);
	}

	return v003_pwm_send(priv, pwm->hwpwm, state);
}

static int v003_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			      struct pwm_state *state)
{
	struct v003_pwm *priv = pwmchip_get_drvdata(chip);
	struct v003_pwm_cfg cfg;
	int ret;

	ret = v003_data_in(priv->v003, V003_PWM_GET, pwm->hwpwm, &cfg,
			   sizeof(cfg));
	if (ret < 0)
		return ret;

	if (ret < (int)sizeof(cfg))
		return -EIO;

	state->period = cfg.actual_ns ? cfg.actual_ns : cfg.period_ns;
	state->duty_cycle = div_u64((u64)state->period * cfg.duty_permille,
				    1000);
	state->polarity = PWM_POLARITY_NORMAL;
	state->enabled = cfg.enable ? true : false;

	return 0;
}

static void v003_pwm_selftest_run(struct pwm_chip *chip);

static const struct pwm_ops v003_pwm_ops = {
	.apply = v003_pwm_apply,
	.get_state = v003_pwm_get_state,
};

static int v003_pwm_probe(struct platform_device *pdev)
{
	struct v003_dev *v003 = v003_get_dev(pdev);
	const struct v003_caps *caps;
	struct v003_pwm *priv;
	struct pwm_chip *chip;
	int ret;

	if (!v003)
		return -EPROBE_DEFER;

	caps = v003_capabilities(v003);
	if (!caps || !caps->npwm) {
		dev_err(&pdev->dev, "the device reports no PWM channels\n");
		return -ENODEV;
	}

	chip = devm_pwmchip_alloc(&pdev->dev, caps->npwm, sizeof(*priv));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	priv = pwmchip_get_drvdata(chip);
	priv->v003 = v003;

	/* devm_pwmchip_alloc() already set chip->dev from &pdev->dev; there is
	 * no separate parent field in this kernel's struct pwm_chip */
	chip->ops = &v003_pwm_ops;

	platform_set_drvdata(pdev, priv);

	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret) {
		dev_err(&pdev->dev, "failed to add the pwm chip: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "pwmchip0 registered with %u channels\n",
		 caps->npwm);

	v003_pwm_selftest_run(chip);

	return 0;
}

/*
 * Probe time self test: applying a state needs /sys/class/pwm/... which is root
 * only, so this drives the same callbacks the sysfs path would and checks that
 * what comes back is what went in.  It is what verifies the ns/permille
 * translation; the firmware side is covered by scripts/pwm_test.py.
 */
static unsigned int v003_pwm_selftest;
module_param_named(selftest, v003_pwm_selftest, uint, 0644);
MODULE_PARM_DESC(selftest, "run a probe time self test (0 = off)");

static void v003_pwm_selftest_run(struct pwm_chip *chip)
{
	struct pwm_state want = {
		.period = 1000000,	/* 1 kHz */
		.duty_cycle = 250000,	/* 25 % */
		.polarity = PWM_POLARITY_NORMAL,
		.enabled = true,
	};
	struct pwm_device *pwm = &chip->pwms[0];
	struct pwm_state got;
	int ret;

	if (!v003_pwm_selftest)
		return;

	ret = v003_pwm_apply(chip, pwm, &want);
	if (ret) {
		dev_warn(&chip->dev, "self test: apply failed: %d\n", ret);
		return;
	}

	ret = v003_pwm_get_state(chip, pwm, &got);
	if (ret) {
		dev_warn(&chip->dev, "self test: get_state failed: %d\n", ret);
		return;
	}

	if (!got.enabled || abs((long)got.period - (long)want.period) > 1000 ||
	    abs((long)got.duty_cycle - (long)want.duty_cycle) > 1000)
		dev_warn(&chip->dev,
			 "self test: applied %llu ns / %llu ns, read back %llu ns / %llu ns (enabled %d)\n",
			 (unsigned long long)want.period,
			 (unsigned long long)want.duty_cycle,
			 (unsigned long long)got.period,
			 (unsigned long long)got.duty_cycle, got.enabled);
	else
		dev_info(&chip->dev,
			 "self test: %llu ns period, %llu ns duty round tripped\n",
			 (unsigned long long)got.period,
			 (unsigned long long)got.duty_cycle);

	/* park the channel low again */
	want.duty_cycle = 0;
	want.enabled = false;
	v003_pwm_apply(chip, pwm, &want);
}

static struct platform_driver v003_pwm_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = v003_pwm_probe,
};
module_platform_driver(v003_pwm_driver);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to PWM");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
