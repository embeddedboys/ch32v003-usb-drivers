// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watchdog child driver for the CH32V003 USB multi function device.
 *
 * The firmware owns the chip's independent watchdog (IWDG): it picks a
 * prescaler and reload value for a requested timeout, feeds the counter, and
 * reports which reset fired.  This driver exposes it as a real
 * watchdog_device, so /dev/watchdog and the usual tooling work.
 *
 * Two hardware facts shape the driver:
 *
 *   - the IWDG cannot be stopped once started, so there is no .stop callback.
 *     The framework then keeps the device running and the magic close does not
 *     disarm anything - which is why nowayout is the honest default here.
 *   - after a watchdog reset the IWDG is off again, so the firmware reports
 *     "running" truthfully and the driver has to re-arm it if the user asks.
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#include "usb-mfd.h"

#define DRV_NAME "v003-wdt"

struct v003_wdt {
	struct v003_dev *v003;
	struct watchdog_device wdd;
	/* a copy, because the reset cause has to be added to the options */
	struct watchdog_info info;
};

static int v003_wdt_start(struct watchdog_device *wdd)
{
	struct v003_wdt *wdt = watchdog_get_drvdata(wdd);

	return v003_cmd_out(wdt->v003, V003_WDG_START, wdd->timeout * 1000);
}

static int v003_wdt_ping(struct watchdog_device *wdd)
{
	struct v003_wdt *wdt = watchdog_get_drvdata(wdd);

	return v003_cmd_out(wdt->v003, V003_WDG_FEED, V003_WDG_FEED_KEY);
}

static int v003_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	struct v003_wdt *wdt = watchdog_get_drvdata(wdd);
	int ret;

	wdd->timeout = timeout;

	if (!watchdog_active(wdd))
		return 0;

	/* re-arm with the new timeout, otherwise the change would only show up
	 * after the next reset (which the old timeout decides) */
	ret = v003_wdt_start(wdd);
	if (ret)
		return ret;

	/* the LSI is only good to about +-20 %, so report what the firmware
	 * says it configured rather than what was asked for */
	return v003_cmd_in(wdt->v003, V003_WDG_GET_STATE, 0, NULL);
}

static const struct watchdog_ops v003_wdt_ops = {
	.owner = THIS_MODULE,
	.start = v003_wdt_start,
	.ping = v003_wdt_ping,
	.set_timeout = v003_wdt_set_timeout,
	/* no .stop: the IWDG cannot be stopped once started */
};

static int v003_wdt_probe(struct platform_device *pdev)
{
	struct v003_dev *v003 = v003_get_dev(pdev);
	struct v003_wdt *wdt;
	u32 state = 0;
	u32 cause = 0;
	int ret;

	if (!v003)
		return -EPROBE_DEFER;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->v003 = v003;

	/* reading the cause also clears it on the device, so do it once */
	ret = v003_cmd_in(v003, V003_WDG_GET_RESET_CAUSE, 0, &cause);
	if (ret) {
		dev_err(&pdev->dev, "failed to read the reset cause: %d\n", ret);
		return ret;
	}

	wdt->info.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING;
	if (cause & V003_WDG_RST_IWDG) {
		dev_warn(&pdev->dev, "the last reset was the watchdog\n");
		wdt->info.options |= WDIOF_CARDRESET;
	}
	/* identity is a char array in the structure, not a pointer */
	strscpy(wdt->info.identity, "CH32V003 IWDG", sizeof(wdt->info.identity));
	wdt->info.firmware_version = 0;

	/* The watchdog API counts in SECONDS (not milliseconds), and the limits
	 * are what the firmware can reach with the 12 bit reload and a nominal
	 * 40 kHz LSI: about 26 s at the slowest prescaler.  It can go below a
	 * second, but a whole-second API cannot express that, so the floor is
	 * one. */
	wdt->wdd.info = &wdt->info;
	wdt->wdd.ops = &v003_wdt_ops;
	wdt->wdd.parent = &pdev->dev;
	wdt->wdd.timeout = 10;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_timeout = 26;
	/* The watchdog core refuses to register a device that has no .stop
	 * callback *unless* it declares a hardware heartbeat limit, which is why
	 * this is here and why max_timeout alone was not enough: without a stop
	 * the watchdog can only be left running, so the core wants to know the
	 * longest it can possibly run.  Both attempts to register without it came
	 * back as -EINVAL. */
	wdt->wdd.max_hw_heartbeat_ms = wdt->wdd.max_timeout * 1000;

	watchdog_set_drvdata(&wdt->wdd, wdt);
	platform_set_drvdata(pdev, wdt);

	ret = devm_watchdog_register_device(&pdev->dev, &wdt->wdd);
	if (ret) {
		dev_err(&pdev->dev, "failed to register the watchdog: %d\n", ret);
		return ret;
	}

	/* ask the device whether it is already running (it is not, right after a
	 * reset, but a host that armed it through the vendor protocol would have
	 * left it that way) */
	if (!v003_cmd_in(v003, V003_WDG_GET_STATE, 0, &state))
		dev_info(&pdev->dev,
			 "registered, timeout %u s, device reports running=%u, %u ms configured\n",
			 wdt->wdd.timeout, state & 1, state >> 8);

	return 0;
}

static struct platform_driver v003_wdt_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = v003_wdt_probe,
};
module_platform_driver(v003_wdt_driver);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to watchdog");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
