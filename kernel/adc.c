// SPDX-License-Identifier: GPL-2.0-only
/*
 * ADC child driver for the CH32V003 USB multi function device.
 *
 * The firmware converts on demand: V003_ADC_START is an OUT request carrying the
 * channel and the main loop runs the conversion (42 us, three orders of
 * magnitude over the USB interrupt's budget), V003_ADC_GET hands the result back
 * with a completion tag.  That maps onto IIO's direct mode directly - one
 * conversion per read_raw(), no buffers, no triggers, because the device has no
 * way to convert anything by itself.
 *
 * Ten channels: 0..7 are the external inputs, 8 is the internal 1.2 V reference
 * and 9 the internal calibration voltage (RM 9.2.1).  The converter is 10 bit
 * (RM 9.1), so a raw value is 0..1023 and the voltage is raw * AVDD / 1024:
 * the scale attribute returns exactly that fraction, with AVDD in millivolts
 * from the `avdd_mv` parameter because the driver has no way to measure the
 * board's supply.  The two internal channels are what makes the device
 * verifiable without wiring anything up: Vref reads about 1.18 V on this board,
 * which is the 1.2 V it should be to within the part's tolerance.
 *
 * The channel count comes from the device's capability report, so a firmware
 * built without the module gets no cell and this driver never probes.
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/iio/iio.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#include "usb-mfd.h"

#define DRV_NAME "v003-adc"

struct v003_adc {
	struct v003_dev *v003;
	struct device *dev;
	struct mutex lock; /* one conversion at a time */
	u8 last_tag;	   /* completion tag of the last conversion accepted */
	u16 avdd_mv;	   /* what the scale is computed from */
};

/* The driver cannot measure the board's supply, so the scale is computed from
 * this; 3.3 V is what the bench board runs at. */
static unsigned int v003_adc_avdd_mv = 3300;
module_param_named(avdd_mv, v003_adc_avdd_mv, uint, 0444);
MODULE_PARM_DESC(avdd_mv, "board supply in millivolts, used for the scale (default 3300)");

/*
 * A conversion result is (tag << 16) | (valid << 15) | value.  Reading the tag
 * before the request and waiting for it to change is what tells a fresh result
 * from the previous one: the firmware executes the request from its main loop,
 * so a reply can arrive before the conversion it asks for has happened (the same
 * completion tag lesson as V003_I2C_GET_RESULT).  In practice one read is
 * enough - a control IN takes ~3 ms and a conversion 42 us - but the driver
 * waits rather than assumes, and reports what it saw when it gives up.
 */
static int v003_adc_convert(struct v003_adc *adc, unsigned int channel,
			    u16 *raw)
{
	struct v003_dev *v003 = adc->v003;
	u32 value = 0;
	int ret;
	int tries = 20;

	mutex_lock(&adc->lock);

	ret = v003_cmd_in(v003, V003_ADC_GET, 0, &value);
	if (ret)
		goto out;

	adc->last_tag = V003_ADC_GET_TAG(value);

	ret = v003_cmd_out(v003, V003_ADC_START, channel);
	if (ret)
		goto out;

	for (;;) {
		ret = v003_cmd_in(v003, V003_ADC_GET, 0, &value);
		if (ret)
			goto out;

		if (V003_ADC_GET_TAG(value) != adc->last_tag)
			break;

		if (--tries == 0) {
			dev_err(adc->dev,
				"channel %u: the firmware did not convert (tag %u)\n",
				channel, adc->last_tag);
			ret = -ETIMEDOUT;
			goto out;
		}
	}

	adc->last_tag = V003_ADC_GET_TAG(value);

	if (!V003_ADC_GET_VALID(value)) {
		dev_err(adc->dev,
			"channel %u: the conversion timed out in the firmware\n",
			channel);
		ret = -EIO;
		goto out;
	}

	*raw = V003_ADC_GET_VALUE(value);
	ret = 0;
out:
	mutex_unlock(&adc->lock);

	return ret;
}

/*
 * Labels: the two internal channels are the interesting ones and userspace has
 * no way to know which index is which from the raw attribute alone.  The
 * external ones are named after the pin the firmware converts on - channels 1
 * and 2 are known (measured on this board, and from WCH's own ADC example), the
 * rest are the datasheet's package table, which this project does not have.
 */
static const char *const v003_adc_labels[V003_ADC_CHANNELS] = {
	"ADC_IN0", "ADC_IN1 (PA1)", "ADC_IN2 (PC4)", "ADC_IN3", "ADC_IN4",
	"ADC_IN5", "ADC_IN6", "ADC_IN7", "Vref (1.2 V internal)",
	"Vcal (internal calibration voltage)",
};

static int v003_adc_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int *val,
			     int *val2, long mask)
{
	struct v003_adc *adc = iio_priv(indio_dev);
	u16 raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = v003_adc_convert(adc, chan->channel, &raw);
		if (ret)
			return ret;

		*val = raw;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/* millivolts per count: AVDD / 1024, as a fraction so nothing
		 * is lost to integer division */
		*val = adc->avdd_mv;
		*val2 = 1024;
		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static int v003_adc_read_label(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan, char *label)
{
	if (chan->channel >= V003_ADC_CHANNELS)
		return -EINVAL;

	return sprintf(label, "%s\n", v003_adc_labels[chan->channel]);
}

static const struct iio_info v003_adc_info = {
	.read_raw = v003_adc_read_raw,
	.read_label = v003_adc_read_label,
};

#define V003_ADC_CHANNEL(_index)					\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = (_index),					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
	}

static const struct iio_chan_spec v003_adc_channels[V003_ADC_CHANNELS] = {
	V003_ADC_CHANNEL(0),
	V003_ADC_CHANNEL(1),
	V003_ADC_CHANNEL(2),
	V003_ADC_CHANNEL(3),
	V003_ADC_CHANNEL(4),
	V003_ADC_CHANNEL(5),
	V003_ADC_CHANNEL(6),
	V003_ADC_CHANNEL(7),
	V003_ADC_CHANNEL(8),
	V003_ADC_CHANNEL(9),
};

static unsigned int v003_adc_selftest;
module_param_named(selftest, v003_adc_selftest, uint, 0644);
MODULE_PARM_DESC(selftest,
		 "read the internal reference and calibration channels at probe time (0 = off)");

/* Report what the internal channels say, in millivolts: the reference is the
 * only absolute truth the device has, and the calibration voltage has two
 * selectable levels, so both are worth seeing in the log. */
static void v003_adc_selftest_run(struct v003_adc *adc)
{
	static const u8 channels[] = { V003_ADC_VREF_CHANNEL,
				       V003_ADC_VCAL_CHANNEL };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(channels); i++) {
		u16 raw;
		int ret;

		ret = v003_adc_convert(adc, channels[i], &raw);
		if (ret) {
			dev_warn(adc->dev, "channel %u read failed: %d\n",
				 channels[i], ret);
			continue;
		}

		dev_info(adc->dev, "channel %u: %u counts = %u mV\n",
			 channels[i], raw, raw * adc->avdd_mv / 1024);
	}
}

static int v003_adc_probe(struct platform_device *pdev)
{
	struct v003_dev *v003 = v003_get_dev(pdev);
	const struct v003_caps *caps = v003_capabilities(v003);
	struct v003_adc *adc;
	struct iio_dev *indio;
	unsigned int channels = V003_ADC_CHANNELS;
	u32 value = 0;
	int ret;

	if (caps && caps->nadc)
		channels = min_t(unsigned int, caps->nadc, V003_ADC_CHANNELS);

	indio = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!indio)
		return -ENOMEM;

	adc = iio_priv(indio);
	adc->v003 = v003;
	adc->dev = &pdev->dev;
	adc->avdd_mv = clamp_t(unsigned int, v003_adc_avdd_mv, 1000, 5000);
	mutex_init(&adc->lock);

	/* the resolution is reported by the device and is worth logging once:
	 * a reading means nothing without it, and this one is 10 bit, not 12 */
	ret = v003_cmd_in(v003, V003_ADC_GET_INFO, 0, &value);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "%u bit, %u channels, scale %u/%u mV\n",
		 value >> 16, value & 0xffff, adc->avdd_mv, 1024);

	indio->name = "v003-adc";
	indio->info = &v003_adc_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = v003_adc_channels;
	indio->num_channels = channels;

	platform_set_drvdata(pdev, indio);

	ret = devm_iio_device_register(&pdev->dev, indio);
	if (ret)
		return ret;

	if (v003_adc_selftest)
		v003_adc_selftest_run(adc);

	return 0;
}

static struct platform_driver v003_adc_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = v003_adc_probe,
};
module_platform_driver(v003_adc_driver);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to ADC (IIO)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
