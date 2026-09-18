// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C child driver for the CH32V003 USB multi function device.
 *
 * The firmware runs a bit banged I2C master (PC1 = SDA, PC2 = SCL) and exposes
 * it one transaction at a time:
 *
 *   write            control OUT data stage [addr << 1][bytes...]
 *   read             control OUT data stage [addr << 1 | 1][count]
 *   write then read  control OUT data stage [addr << 1][bytes...] with
 *                    wValue = count, which emits a repeated START
 *   result           IN V003_I2C_GET_RX (up to 64 bytes), plus GET_STATUS
 *
 * That maps onto i2c_algorithm.master_xfer() directly: a message without
 * I2C_M_RD is a write, a read message with a write message right before it is
 * the repeated START case (which is exactly how register/EEPROM reads are
 * expressed), and a read on its own is a current address read.
 *
 * Modelled on drivers/i2c/busses/i2c-dln2.c.
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "usb-mfd.h"

#define DRV_NAME "v003-i2c"

/* The firmware reads at most I2C_RX_BUF_SIZE bytes in one transaction. */
#define V003_I2C_RX_MAX 64

struct v003_i2c {
	struct v003_dev *v003;
	struct i2c_adapter adap;

	/* completion tag of the last result this driver accepted */
	u8 last_tag;

	/* control OUT data stage: the address byte plus the payload */
	u8 tx[V003_CTRL_DATA_MAX];
	/* bytes the last transaction read back */
	u8 rx[V003_I2C_RX_MAX];
};

/*
 * The firmware reports what happened on the wire, which is the only way to tell
 * a missing device from a device that NACKed a data byte.  Always ask: a
 * silently ignored write is much worse than one extra control transfer.
 *
 * A data stage is executed by the firmware's main loop, so the answer has to be
 * tagged: V003_I2C_GET_RESULT carries the main loop's request counter in its
 * top byte and this polls until that tag differs from the last one it accepted.
 * Normally the firmware has long finished by the time the read arrives, so it
 * costs nothing; a plain status read was measurably returning the previous
 * request's status often enough to matter.
 */
static int v003_i2c_check_status(struct v003_i2c *i2c)
{
	struct v003_dev *v003 = i2c->v003;
	u32 value = 0;
	u32 status;
	int ret;
	int tries = 20;

	for (;;) {
		ret = v003_cmd_in(v003, V003_I2C_GET_RESULT, 0, &value);
		if (ret)
			return ret;

		if ((value >> V003_I2C_TAG_SHIFT) != i2c->last_tag)
			break;

		if (--tries == 0) {
			dev_err(i2c->adap.dev.parent,
				"firmware did not finish the request (tag %u)\n",
				i2c->last_tag);
			return -ETIMEDOUT;
		}
	}

	i2c->last_tag = value >> V003_I2C_TAG_SHIFT;
	status = value & V003_I2C_STATUS_MASK;

	if (status & V003_I2C_ST_NOT_CONFIGURED)
		return -ENODEV;

	if (status & V003_I2C_ST_ADDR_NACK)
		return -ENXIO; /* nobody at that address */

	if (status & V003_I2C_ST_READ_NACK)
		return -EREMOTEIO; /* the repeated START was not acknowledged */

	if (status & V003_I2C_ST_DATA_NACK)
		return -EIO; /* the slave refused byte %u */
	if (status & V003_I2C_ST_STRETCH)
		return -ETIMEDOUT; /* a slave held SCL down */
	if (status & V003_I2C_ST_TIMEOUT)
		return -ETIMEDOUT;

	if (!(status & V003_I2C_ST_OK))
		return -EIO;

	return 0;
}

static int v003_i2c_get_rx(struct v003_i2c *i2c, u8 *buf, u16 count)
{
	int ret;

	/* always land in our own buffer first: an in kernel client is free to
	 * hand us a buffer on its stack, and that cannot be DMA mapped */
	ret = v003_data_in(i2c->v003, V003_I2C_GET_RX, 0, i2c->rx,
			   min_t(u16, count, sizeof(i2c->rx)));
	if (ret < 0)
		return ret;

	if (ret != count) {
		dev_err(i2c->adap.dev.parent, "asked for %u bytes, got %d\n",
			count, ret);
		return -EIO;
	}

	memcpy(buf, i2c->rx, count);

	return 0;
}

static int v003_i2c_write_msg(struct v003_i2c *i2c, struct i2c_msg *msg,
			      u16 read_count)
{
	u16 cmd = read_count ? V003_I2C_WRITE_READ : V003_I2C_WRITE;
	unsigned int len = msg->len + 1;
	int ret;

	if (len > sizeof(i2c->tx))
		return -EINVAL;

	i2c->tx[0] = (msg->addr << 1) & 0xfe; /* R/W = 0 */
	memcpy(i2c->tx + 1, msg->buf, msg->len);

	ret = v003_data_out(i2c->v003, cmd, read_count, i2c->tx, len);
	if (ret < 0)
		return ret;

	return v003_i2c_check_status(i2c);
}

/* a read on its own: current address read, the slave keeps its pointer */
static int v003_i2c_read_msg(struct v003_i2c *i2c, struct i2c_msg *msg)
{
	u8 setup[2] = { (msg->addr << 1) | 1, msg->len };
	int ret;

	ret = v003_data_out(i2c->v003, V003_I2C_READ, 0, setup, sizeof(setup));
	if (ret < 0)
		return ret;

	ret = v003_i2c_check_status(i2c);
	if (ret)
		return ret;

	return v003_i2c_get_rx(i2c, msg->buf, msg->len);
}

static int v003_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			 int num)
{
	struct v003_i2c *i2c = i2c_get_adapdata(adap);
	int i = 0;
	int ret;

	while (i < num) {
		struct i2c_msg *msg = &msgs[i];

		if (msg->flags & I2C_M_TEN) {
			dev_err(&adap->dev, "10 bit addresses are not supported\n");
			return -EOPNOTSUPP;
		}

		if (msg->flags & I2C_M_NOSTART) {
			dev_err(&adap->dev, "I2C_M_NOSTART is not supported\n");
			return -EOPNOTSUPP;
		}

		if (msg->len > V003_I2C_RX_MAX) {
			dev_err(&adap->dev, "%u byte message exceeds the %u byte limit\n",
				msg->len, V003_I2C_RX_MAX);
			return -EOPNOTSUPP;
		}

		if (msg->flags & I2C_M_RD) {
			ret = v003_i2c_read_msg(i2c, msg);
			i++;
		} else if (i + 1 < num && (msgs[i + 1].flags & I2C_M_RD)) {
			/* write followed by a read in the same transfer: the
			 * firmware turns this into a repeated START, which is
			 * how register and EEPROM reads are expressed */
			struct i2c_msg *rd = &msgs[i + 1];

			if (rd->len > V003_I2C_RX_MAX) {
				dev_err(&adap->dev,
					"%u byte read exceeds the %u byte limit\n",
					rd->len, V003_I2C_RX_MAX);
				return -EOPNOTSUPP;
			}

			ret = v003_i2c_write_msg(i2c, msg, rd->len);
			if (!ret)
				ret = v003_i2c_get_rx(i2c, rd->buf, rd->len);
			i += 2;
		} else {
			ret = v003_i2c_write_msg(i2c, msg, 0);
			i++;
		}

		if (ret) {
			dev_dbg(&adap->dev, "message %d/%d (addr %#x) failed: %d\n",
				i, num, msg->addr, ret);
			return ret;
		}
	}

	return num;
}

static u32 v003_i2c_func(struct i2c_adapter *adap)
{
	/* no SMBus emulation on purpose: the emulated block read expects to
	 * continue a transfer with I2C_M_NOSTART, which a request per
	 * transaction firmware cannot do */
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm v003_i2c_algorithm = {
	.master_xfer = v003_i2c_xfer,
	.functionality = v003_i2c_func,
};

/* Half period of the bit banged clock in 100 ns units, as the firmware wants
 * it.  12 measures ~280 kHz; raise it (25 = ~140 kHz, 50 = ~70 kHz) when the
 * pull-ups or the wiring are poor. */
static unsigned int half_period = 12;
module_param(half_period, uint, 0644);
MODULE_PARM_DESC(half_period,
		 "I2C half period in 100 ns units (default 12, ~280 kHz)");

/*
 * Probe time self test: read `selftest_len` bytes from word address
 * `selftest_off` of the 7 bit address `selftest` and log them.  It exists
 * because the adapter can only be exercised from userspace with root (/dev/i2c-N
 * belongs to the i2c group), while the values a real device holds are usually
 * known - an AT24C256 with a marker at 0 is the case this was written for.  It
 * uses the same two message write+read transfer (repeated START) that the at24
 * driver uses, so a passing self test is the whole adapter path verified.
 * 0 disables it.
 */
static unsigned int selftest;
module_param(selftest, uint, 0644);
MODULE_PARM_DESC(selftest, "7 bit address to read at probe for a self test (0 = off)");

static unsigned int selftest_off;
module_param_named(selftest_off, selftest_off, uint, 0644);
MODULE_PARM_DESC(selftest_off, "word address the self test reads from");

static unsigned int selftest_len = 8;
module_param_named(selftest_len, selftest_len, uint, 0644);
MODULE_PARM_DESC(selftest_len, "bytes the self test reads (max 16)");

static void v003_i2c_selftest(struct v003_i2c *i2c)
{
	struct i2c_msg msgs[2];
	u8 word[2] = { (u8)(selftest_off >> 8), (u8)selftest_off };
	u8 *buf = i2c->rx; /* never a stack buffer: this one is DMA mapped */
	int ret;

	if (!selftest)
		return;

	if (selftest_len > sizeof(i2c->rx))
		selftest_len = sizeof(i2c->rx);

	msgs[0].addr = selftest;
	msgs[0].flags = 0;
	msgs[0].len = sizeof(word);
	msgs[0].buf = word;

	msgs[1].addr = selftest;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = selftest_len;
	msgs[1].buf = buf;

	ret = i2c_transfer(&i2c->adap, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		dev_warn(i2c->adap.dev.parent,
			 "self test read of %#04x at %#x failed: %d\n",
			 selftest_off, selftest, ret);
		return;
	}

	dev_info(i2c->adap.dev.parent, "self test %#x[%#04x]: %*ph\n", selftest,
		 selftest_off, selftest_len, buf);
}

static int v003_i2c_probe(struct platform_device *pdev)
{
	struct v003_dev *v003 = v003_get_dev(pdev);
	struct v003_i2c *i2c;
	int ret;

	if (!v003)
		return -EPROBE_DEFER;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->v003 = v003;

	/* configures SDA/SCL as open drain and releases the bus */
	ret = v003_cmd_out(v003, V003_I2C_CONFIG, half_period);
	if (ret) {
		dev_err(&pdev->dev, "failed to configure the bus: %d\n", ret);
		return ret;
	}

	i2c->adap.owner = THIS_MODULE;
	i2c->adap.class = I2C_CLASS_HWMON;
	i2c->adap.algo = &v003_i2c_algorithm;
	i2c->adap.retries = 2;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.dev.of_node = pdev->dev.of_node;
	snprintf(i2c->adap.name, sizeof(i2c->adap.name), "CH32V003 I2C (%s)",
		 dev_name(&pdev->dev));
	i2c_set_adapdata(&i2c->adap, i2c);

	platform_set_drvdata(pdev, i2c);

	ret = i2c_add_adapter(&i2c->adap);
	if (ret) {
		dev_err(&pdev->dev, "failed to register the adapter: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "bus %d registered, half period %u x 100 ns\n",
		 i2c->adap.nr, half_period);

	v003_i2c_selftest(i2c);

	return 0;
}

static void v003_i2c_remove(struct platform_device *pdev)
{
	struct v003_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
}

static struct platform_driver v003_i2c_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = v003_i2c_probe,
	.remove = v003_i2c_remove,
};
module_platform_driver(v003_i2c_driver);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to I2C adapter");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
