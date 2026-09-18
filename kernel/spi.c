// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI child driver for the CH32V003 USB multi function device.
 *
 * The firmware owns a hardware SPI1 master (SCK = PC5, MOSI = PC6,
 * MISO = PC7) and clocks bytes in and out from its main loop:
 *
 *   V003_SPI_ENABLE    turn the path on
 *   V003_SPI_CONFIG    (prescaler << 8) | mode, mode = CPOL/CPHA bits
 *   V003_SPI_TRANSFER  control OUT data stage, up to 64 bytes at a time
 *   V003_SPI_GET_RX    the bytes sampled on MISO during that transfer
 *
 * Chip select is the interesting part.  The firmware can drive one pin around
 * its own transfer, but a Linux message is delimited by CS, not by a transfer:
 * several transfers can sit between two CS edges, and that is exactly how
 * register access is expressed (`spi_write_then_read()`, SPI-NOR commands...).
 * So the controller takes a pin number as a module parameter and drives it
 * through the GPIO module instead - which is also what the firmware documents
 * as the intended way ("chip select is not driven by the firmware: the host
 * toggles any GPIO pin through the GPIO module").
 *
 * Modelled on drivers/spi/spi-dln2.c.
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#include "usb-mfd.h"

#define DRV_NAME "v003-spi"

/* SPI1 hangs off APB2, which the vendor firmware keeps at the system clock. */
#define V003_SPI_APB2_HZ 48000000

struct v003_spi {
	struct v003_dev *v003;
	struct spi_controller *ctlr;

	int cs_pin; /* firmware pin number, negative = not driven */

	/* what the firmware is configured with, so a message only pays for a
	 * V003_SPI_CONFIG when the settings actually change */
	u8 mode;
	u8 prescaler;
	bool configured;

	/* both are handed to usb_control_msg(), so they must not be on the
	 * stack (the core bounces, but keeping them here is cheaper) */
	u8 tx[V003_SPI_MAX_XFER];
	u8 rx[V003_SPI_MAX_XFER];
};

static int v003_spi_set_cs_pin(struct v003_spi *spi, int pin)
{
	struct v003_dev *v003 = spi->v003;
	int ret;

	if (pin < 0)
		return 0;

	/* claim the pin and drive it high (chip select is active low) */
	ret = v003_cmd_out(v003, V003_GPIO_REQUEST, V003_GPIO_VAL(pin, 0));
	if (ret)
		return ret;

	return v003_cmd_out(v003, V003_GPIO_DIRECTION_OUTPUT,
			    V003_GPIO_VAL(pin, 1));
}

static void v003_spi_set_cs(struct spi_device *spi_dev, bool enable)
{
	struct v003_spi *spi = spi_controller_get_devdata(spi_dev->controller);

	if (spi->cs_pin < 0)
		return;

	/* the firmware's V003_GPIO_SET takes (pin << 8) | level, and an enabled
	 * chip select is a low line */
	v003_cmd_out(spi->v003, V003_GPIO_SET,
		     V003_GPIO_VAL(spi->cs_pin, enable ? 0 : 1));
}

/* BR field of SPI_CTLR1: 0 = /2, 1 = /4, ... 7 = /256 */
static u8 v003_spi_prescaler_for(u32 hz)
{
	u32 div = 1;
	u8 br;

	if (!hz || hz >= V003_SPI_APB2_HZ / 2)
		return 0;

	for (br = 0; br < 7; br++) {
		div <<= 1;
		if (V003_SPI_APB2_HZ / div <= hz)
			return br;
	}

	return 7;
}

static int v003_spi_configure(struct v003_spi *spi, struct spi_device *spi_dev)
{
	u8 mode = spi_dev->mode & (SPI_CPOL | SPI_CPHA);
	u8 prescaler = v003_spi_prescaler_for(spi_dev->max_speed_hz);
	int ret;

	mode = (mode & SPI_CPHA ? 1 : 0) | (mode & SPI_CPOL ? 2 : 0);

	if (spi->configured && mode == spi->mode && prescaler == spi->prescaler)
		return 0;

	ret = v003_cmd_out(spi->v003, V003_SPI_ENABLE, 1);
	if (ret)
		return ret;

	ret = v003_cmd_out(spi->v003, V003_SPI_CONFIG,
			   ((u16)prescaler << 8) | mode);
	if (ret)
		return ret;

	/* never let the firmware drive a chip select: this driver owns it */
	ret = v003_cmd_out(spi->v003, V003_SPI_SET_CS, 0xffff);
	if (ret)
		return ret;

	spi->mode = mode;
	spi->prescaler = prescaler;
	spi->configured = true;

	dev_dbg(&spi_dev->dev, "mode %u, prescaler %u (%u Hz)\n", mode,
		prescaler, V003_SPI_APB2_HZ >> (prescaler + 1));

	return 0;
}

static int v003_spi_setup(struct spi_device *spi_dev)
{
	struct v003_spi *spi = spi_controller_get_devdata(spi_dev->controller);

	if (spi_dev->bits_per_word != 8)
		return -EINVAL;

	return v003_spi_configure(spi, spi_dev);
}

static int v003_spi_transfer_one(struct spi_controller *ctlr,
				 struct spi_device *spi_dev,
				 struct spi_transfer *xfer)
{
	struct v003_spi *spi = spi_controller_get_devdata(ctlr);
	unsigned int done = 0;
	int ret;

	ret = v003_spi_configure(spi, spi_dev);
	if (ret)
		return ret;

	while (done < xfer->len) {
		unsigned int len = min_t(unsigned int, xfer->len - done,
					 V003_SPI_MAX_XFER);
		const u8 *tx = xfer->tx_buf ? xfer->tx_buf + done : NULL;
		u8 *rx = xfer->rx_buf ? xfer->rx_buf + done : NULL;

		/* the firmware always clocks something out, so a receive only
		 * transfer sends zeros */
		if (!tx) {
			memset(spi->tx, 0, len);
			tx = spi->tx;
		}

		ret = v003_data_out(spi->v003, V003_SPI_TRANSFER, 0, tx, len);
		if (ret < 0)
			return ret;

		if (rx) {
			ret = v003_data_in(spi->v003, V003_SPI_GET_RX, 0,
					   spi->rx, len);
			if (ret < 0)
				return ret;

			if (ret != len) {
				dev_err(&spi_dev->dev,
					"clocked %u bytes, got %d back\n", len,
					ret);
				return -EIO;
			}

			memcpy(rx, spi->rx, len);
		}

		done += len;
	}

	return 0;
}

static unsigned int v003_spi_default_speed = V003_SPI_APB2_HZ / 2;
module_param_named(speed_hz, v003_spi_default_speed, uint, 0644);
MODULE_PARM_DESC(speed_hz, "clock used when a device asks for none (default 24 MHz)");

/*
 * Probe time self test: clock `selftest` bytes with MOSI jumped to MISO and
 * compare what came back.  With the PC6<->PC7 jumper the device test board
 * carries, every byte has to return unchanged, which verifies enable/config,
 * chip select handling, the chunking of transfers longer than 64 bytes and the
 * GET_RX path in one go.  It runs through the SPI core (spi_new_device +
 * spi_sync) so it exercises exactly what a real client driver would.  0
 * disables it.
 */
static unsigned int v003_spi_selftest;
module_param_named(selftest, v003_spi_selftest, uint, 0644);
MODULE_PARM_DESC(selftest, "bytes to loop back at probe for a self test (0 = off)");

static unsigned int v003_spi_selftest_speed = 1000000;
module_param_named(selftest_speed, v003_spi_selftest_speed, uint, 0644);
MODULE_PARM_DESC(selftest_speed, "clock for the self test (default 1 MHz)");

static int v003_spi_cs_pin = -1;
module_param_named(cs_pin, v003_spi_cs_pin, int, 0644);
MODULE_PARM_DESC(cs_pin, "firmware pin number driven as chip select (-1 = none)");

static void v003_spi_selftest_run(struct spi_controller *ctlr)
{
	struct spi_board_info info = {
		.modalias = "v003-spi-selftest",
		.bus_num = ctlr->bus_num,
		.chip_select = 0,
		.max_speed_hz = v003_spi_selftest_speed,
		.mode = SPI_MODE_0,
	};
	struct spi_device *spi_dev;
	struct spi_transfer xfer = { };
	struct spi_message msg;
	u8 *tx, *rx;
	unsigned int i;
	int ret;

	if (!v003_spi_selftest)
		return;

	/* DMA mapped by the SPI core, so it cannot be a stack buffer */
	tx = kmalloc(v003_spi_selftest, GFP_KERNEL);
	rx = kmalloc(v003_spi_selftest, GFP_KERNEL);
	if (!tx || !rx)
		goto out;

	for (i = 0; i < v003_spi_selftest; i++)
		tx[i] = (u8)(0xc3 + i * 7);

	spi_dev = spi_new_device(ctlr, &info);
	if (!spi_dev) {
		dev_warn(ctlr->dev.parent, "self test: no device\n");
		goto out;
	}

	xfer.tx_buf = tx;
	xfer.rx_buf = rx;
	xfer.len = v003_spi_selftest;
	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(spi_dev, &msg);
	spi_unregister_device(spi_dev);

	if (ret) {
		dev_warn(ctlr->dev.parent, "self test: transfer failed: %d\n",
			 ret);
		goto out;
	}

	for (i = 0; i < v003_spi_selftest; i++) {
		if (rx[i] != tx[i])
			break;
	}

	if (i == v003_spi_selftest)
		dev_info(ctlr->dev.parent,
			 "self test: %u bytes looped back unchanged\n",
			 v003_spi_selftest);
	else
		dev_warn(ctlr->dev.parent,
			 "self test: byte %u is %#x, expected %#x (is MOSI jumped to MISO?)\n",
			 i, rx[i], tx[i]);

out:
	kfree(tx);
	kfree(rx);
}

static int v003_spi_probe(struct platform_device *pdev)
{
	struct v003_dev *v003 = v003_get_dev(pdev);
	struct spi_controller *ctlr;
	struct v003_spi *spi;
	int ret;

	if (!v003)
		return -EPROBE_DEFER;

	ret = v003_cmd_out(v003, V003_SPI_ENABLE, 0);
	if (ret) {
		dev_err(&pdev->dev, "failed to reach the SPI module: %d\n", ret);
		return ret;
	}

	ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*spi));
	if (!ctlr)
		return -ENOMEM;

	spi = spi_controller_get_devdata(ctlr);
	spi->v003 = v003;
	spi->cs_pin = v003_spi_cs_pin;

	ret = v003_spi_set_cs_pin(spi, spi->cs_pin);
	if (ret) {
		dev_err(&pdev->dev, "failed to set up chip select pin %d: %d\n",
			spi->cs_pin, ret);
		return ret;
	}

	ctlr->dev.parent = &pdev->dev;
	ctlr->dev.of_node = pdev->dev.of_node;
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->max_speed_hz = V003_SPI_APB2_HZ / 2;
	ctlr->min_speed_hz = V003_SPI_APB2_HZ / 256;
	if (spi->cs_pin >= 0)
		ctlr->set_cs = v003_spi_set_cs;
	ctlr->setup = v003_spi_setup;
	ctlr->transfer_one = v003_spi_transfer_one;

	platform_set_drvdata(pdev, spi);

	ret = devm_spi_register_controller(&pdev->dev, ctlr);
	if (ret) {
		dev_err(&pdev->dev, "failed to register the controller: %d\n",
			ret);
		return ret;
	}

	spi->ctlr = ctlr;

	dev_info(&pdev->dev,
		 "spi%u registered, max %u Hz, chip select %s%d\n",
		 ctlr->bus_num, ctlr->max_speed_hz,
		 spi->cs_pin < 0 ? "none" : "pin ", spi->cs_pin);

	v003_spi_selftest_run(ctlr);

	return 0;
}

static struct platform_driver v003_spi_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = v003_spi_probe,
};
module_platform_driver(v003_spi_driver);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to SPI controller");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
