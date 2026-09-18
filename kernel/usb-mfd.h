/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared definitions for the CH32V003 USB MFD core and its child drivers.
 *
 * The layout follows the Diolan DLN-2 (drivers/mfd/dln2.c): the USB driver is
 * only a transport that exposes v003_transfer*(), and every function of the
 * device (GPIO, I2C, SPI) is a platform driver instantiated from an MFD cell
 * and talking through those helpers.
 */
#ifndef __V003_MFD_H
#define __V003_MFD_H

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/usb.h>

/* Module ("handle") ids, mirroring enum dln2_handle and the firmware's
 * V003_*_MODULE_ID. */
#define V003_HANDLE_CTRL 0x00
#define V003_HANDLE_GPIO 0x01
#define V003_HANDLE_SPI	 0x02
#define V003_HANDLE_I2C	 0x03
#define V003_HANDLE_WDG	 0x04
#define V003_HANDLE_PWM	 0x05
#define V003_HANDLE_ADC	 0x06
#define V003_HANDLE_UART 0x07

/* Commands, mirrored from the firmware (vendor/vendor.h, vendor/gpio.h).
 * wIndex carries the command (module in the high byte), wValue the argument. */
#define V003_GENERIC_CMD(cmd) ((cmd) | (V003_HANDLE_CTRL << 8))
#define V003_GET_DEVICE_VER   V003_GENERIC_CMD(0x30)
/* the first word of the factory unique id */
#define V003_GET_DEVICE_SN    V003_GENERIC_CMD(0x31)
/* the capability report: which modules this firmware build contains, the
 * channel counts and the pins the device owns.  A host reads it instead of
 * assuming, which is what makes build time module selection safe. */
#define V003_GET_CAPABILITIES V003_GENERIC_CMD(0x3d)

#define V003_CAP_GPIO  BIT(0)
#define V003_CAP_SPI   BIT(1)
#define V003_CAP_I2C   BIT(2)
#define V003_CAP_ADC   BIT(3)
#define V003_CAP_PWM   BIT(4)
#define V003_CAP_UART  BIT(5)
#define V003_CAP_WDG   BIT(7)
#define V003_CAP_PWR   BIT(8)
#define V003_CAP_FRAME BIT(6)

struct v003_caps {
	u32 caps;
	u8 ngpio;
	u8 nadc;
	u8 npwm;
	u8 nuart;
	u32 reserved_lo; /* pins 0..31 the device owns */
	u32 reserved_hi; /* pins 32..63 the device owns */
};

/* the whole 96 bit ESIG unique id (V003_DEVICE_UID_SIZE bytes, see
 * lib/v003_usb_ids.h); the device reports the same value, in hex, as its USB
 * serial number string */
#define V003_GET_DEVICE_UID   V003_GENERIC_CMD(0x3c)

#define V003_GENERIC_CMD_GET_CMD(cmd) ((cmd) & 0xff)
#define V003_GENERIC_CMD_GET_ID(cmd)  ((cmd) >> 8)

#define V003_GPIO_CMD(cmd) ((cmd) | (V003_HANDLE_GPIO << 8))
#define V003_GPIO_SET		 V003_GPIO_CMD(0x06)
#define V003_GPIO_GET		 V003_GPIO_CMD(0x07)
#define V003_GPIO_REQUEST	 V003_GPIO_CMD(0x08)
#define V003_GPIO_FREE		 V003_GPIO_CMD(0x09)
#define V003_GPIO_GET_DIRECTION	 V003_GPIO_CMD(0x0a)
#define V003_GPIO_DIRECTION_INPUT  V003_GPIO_CMD(0x0b)
#define V003_GPIO_DIRECTION_OUTPUT V003_GPIO_CMD(0x0c)
/* pin index in the high byte, value in the low byte */
#define V003_GPIO_VAL(pin, val) (((pin) << 8) | ((val) & 0xff))

/* PWM module (vendor/pwm.h).  A period in nanoseconds and a duty in permille,
 * which is what the Linux PWM API hands a driver; GET reads the same structure
 * back with the period the prescaler and reload work out to. */
#define V003_PWM_CMD(cmd) ((cmd) | (V003_HANDLE_PWM << 8))

#define V003_PWM_SET	  V003_PWM_CMD(0x70)
#define V003_PWM_GET	  V003_PWM_CMD(0x71)
#define V003_PWM_GET_INFO V003_PWM_CMD(0x72)

struct v003_pwm_cfg {
	u8 channel;
	u8 enable;
	u16 duty_permille;
	u32 period_ns;	/* requested */
	u32 actual_ns;	/* what the timer does, filled in by GET */
};

/* ADC module (vendor/adc.h).  The converter is 10 bit and unbuffered: one
 * conversion per request, the result read back with a completion tag.  A
 * conversion takes ~42 us (241 + 11 ADCCLK at 6 MHz), which is why START is an
 * OUT request the main loop runs instead of work in the USB interrupt. */
#define V003_ADC_CMD(cmd) ((cmd) | (V003_HANDLE_ADC << 8))

#define V003_ADC_START	    V003_ADC_CMD(0x80) /* wValue: channel */
#define V003_ADC_GET	    V003_ADC_CMD(0x81) /* (seq << 16) | (valid << 15) | value */
#define V003_ADC_GET_INFO   V003_ADC_CMD(0x82) /* (bits << 16) | channels */
#define V003_ADC_GET_SEQ    V003_ADC_CMD(0x83)
#define V003_ADC_GET_STATUS V003_ADC_CMD(0x84)
#define V003_ADC_SET_CALVOL V003_ADC_CMD(0x85)

#define V003_ADC_CHANNELS     10
#define V003_ADC_BITS	      10
#define V003_ADC_MAX	      0x3ff
#define V003_ADC_VREF_CHANNEL 8	 /* internal 1.2 V reference */
#define V003_ADC_VCAL_CHANNEL 9	 /* internal calibration voltage */
#define V003_ADC_VREF_MV      1200
#define V003_ADC_CALVOL_HALF  0	 /* 2/4 AVDD */
#define V003_ADC_CALVOL_3Q    1	 /* 3/4 AVDD */

/* UART module (vendor/uart.h): USART1 remapped to PD0 (TX) and PD1 (RX), a
 * receive ring filled by the interrupt and a transmit ring it drains.  The
 * default pin mapping is the USB pull-up line and the boot button on this board,
 * and PD1 is the chip's SWIO debug pin, so the receive path needs a jumper to
 * PD0 to be usable at all (notes/uart.md). */
#define V003_UART_CMD(cmd) ((cmd) | (V003_HANDLE_UART << 8))

#define V003_UART_CONFIG      V003_UART_CMD(0x90) /* OUT data: struct v003_uart_cfg */
#define V003_UART_GET_CFG     V003_UART_CMD(0x91) /* IN data: the same, actual_baud filled */
#define V003_UART_WRITE       V003_UART_CMD(0x92) /* OUT data: bytes to queue */
#define V003_UART_READ        V003_UART_CMD(0x93) /* IN data, wValue = max bytes */
#define V003_UART_GET_STATE   V003_UART_CMD(0x94) /* (tx_queued << 24) | (rx_available << 16) | (tx_dropped << 8) | rx_dropped */
#define V003_UART_GET_COUNTS  V003_UART_CMD(0x95) /* (tx_bytes << 16) | rx_bytes, 16 bit */
#define V003_UART_GET_ERRORS  V003_UART_CMD(0x96) /* (isr_entries << 24) | (overrun << 16) | (framing << 8) | parity */
#define V003_UART_FLUSH       V003_UART_CMD(0x97)
#define V003_UART_GET_INFO    V003_UART_CMD(0x99) /* (rx ring size << 16) | ports */
#define V003_UART_CLEAR_STATS V003_UART_CMD(0x9a)

#define V003_UART_TX_PIN 48 /* PD0 */
#define V003_UART_RX_PIN 49 /* PD1, also SWIO */
#define V003_UART_COUNT	 1

#define V003_UART_PARITY_NONE 0
#define V003_UART_PARITY_EVEN 1
#define V003_UART_PARITY_ODD  2
#define V003_UART_BAUD_MIN    733
#define V003_UART_BAUD_MAX    3000000
#define V003_UART_STOP_MIN    1
#define V003_UART_STOP_MAX    2
#define V003_UART_DATA_BITS_MIN 8
#define V003_UART_DATA_BITS_MAX 9

/* what a host writes to configure and reads back; actual_baud is what the
 * prescaler really divides to (baud = HCLK / BRR, RM 12.3) */
struct v003_uart_cfg {
	u32 baud;
	u8 data_bits;
	u8 parity;
	u8 stop_bits;
	u8 enable;
	u32 actual_baud;
};

#define V003_UART_GET_TX_QUEUED(val)  ((val) >> 24)
#define V003_UART_GET_RX_AVAIL(val)   (((val) >> 16) & 0xff)
#define V003_UART_GET_TX_DROPPED(val) (((val) >> 8) & 0xff)
#define V003_UART_GET_RX_DROPPED(val) ((val) & 0xff)

#define V003_ADC_GET_TAG(val)   (((val) >> 16) & 0xff)
#define V003_ADC_GET_VALID(val) (((val) >> 15) & 1)
#define V003_ADC_GET_VALUE(val) ((val) & V003_ADC_MAX)

/* Watchdog module (vendor/wdg.h): the chip's IWDG.  The timeout is a one way
 * door (the hardware cannot be stopped once started), and GET_RESET_CAUSE
 * answers once, with the flags of the last reset. */
#define V003_WDG_CMD(cmd) ((cmd) | (V003_HANDLE_WDG << 8))

#define V003_WDG_START		 V003_WDG_CMD(0x60) /* wValue: timeout in ms */
#define V003_WDG_FEED		 V003_WDG_CMD(0x61) /* wValue: V003_WDG_FEED_KEY */
#define V003_WDG_FEED_KEY	 0xaaaa
#define V003_WDG_GET_STATE	 V003_WDG_CMD(0x62) /* (timeout ms << 8) | running */
#define V003_WDG_GET_RESET_CAUSE V003_WDG_CMD(0x63)

#define V003_WDG_RST_PIN  BIT(0)
#define V003_WDG_RST_POR  BIT(1)
#define V003_WDG_RST_SOFT BIT(2)
#define V003_WDG_RST_IWDG BIT(3)
#define V003_WDG_RST_WWDG BIT(4)
#define V003_WDG_RST_LPWR BIT(5)

/* I2C module (vendor/i2c.h).  The firmware is a bit banged master: a request
 * carries one transaction, and the bytes it read are fetched afterwards. */
#define V003_I2C_CMD(cmd) ((cmd) | (V003_HANDLE_I2C << 8))

/* OUT, wValue = half period in 100 ns units (0 keeps the current value, the
 * firmware default of 12 measures ~280 kHz on the wire) */
#define V003_I2C_CONFIG	    V003_I2C_CMD(0x50)
/* OUT with a data stage [address byte with the R/W bit][bytes...] */
#define V003_I2C_WRITE	    V003_I2C_CMD(0x51)
/* OUT with a data stage [address byte with R/W = 1][count] */
#define V003_I2C_READ	    V003_I2C_CMD(0x52)
/* IN: the bytes read by the last transfer (data stage, up to 64) */
#define V003_I2C_GET_RX	    V003_I2C_CMD(0x53)
/* IN: status of the last transaction */
#define V003_I2C_GET_STATUS V003_I2C_CMD(0x54)
/* IN: status with a completion tag, (main loop request counter << 24) | status.
 * A control OUT data stage is executed by the firmware's main loop, so the
 * interrupt answers the transfer *before* the work is done and a plain status
 * read can still be the previous request's (measured: 0x2 = address NACK from
 * the request before, re-read 0x1201 = OK, 18 bytes).  The tag changes for
 * every request, so polling it until it changes is what makes the result
 * trustworthy - and it costs an extra control transfer only when the firmware
 * has not caught up yet. */
#define V003_I2C_GET_RESULT V003_I2C_CMD(0x58)
#define V003_I2C_STATUS_MASK 0x00ffffffu
#define V003_I2C_TAG_SHIFT   24
/* OUT with a data stage [address byte with R/W = 0][bytes...] and
 * wValue = bytes to read back afterwards, with a repeated START in between */
#define V003_I2C_WRITE_READ V003_I2C_CMD(0x57)

/* SPI module (vendor/spi.h): hardware SPI1 master, SCK = PC5, MOSI = PC6,
 * MISO = PC7.  One V003_SPI_TRANSFER clocks up to 64 bytes out on MOSI and
 * keeps the bytes sampled on MISO, which V003_SPI_GET_RX hands back.  The
 * firmware can drive one chip select pin around its transfer
 * (V003_SPI_SET_CS), but a host that owns the CS line itself - as a Linux
 * spi_controller does, because a message can hold several transfers between two
 * CS edges - drives it through the GPIO module instead. */
#define V003_SPI_CMD(cmd) ((cmd) | (V003_HANDLE_SPI << 8))

/* OUT, wValue = 0/1: the SPI data path has to be enabled before a transfer */
#define V003_SPI_ENABLE	    V003_SPI_CMD(0x40)
/* OUT, wValue = (prescaler << 8) | mode, mode bit0 = CPHA, bit1 = CPOL, the
 * prescaler is the SPI_CTLR1 BR field (0 = /2 ... 7 = /256) */
#define V003_SPI_CONFIG	    V003_SPI_CMD(0x41)
/* IN -> (prescaler << 16) | (mode << 8) | enabled */
#define V003_SPI_GET_STATE  V003_SPI_CMD(0x42)
/* IN -> bytes clocked out on MOSI since reset */
#define V003_SPI_GET_STATS  V003_SPI_CMD(0x43)
/* OUT, wValue = pin index or 0xffff for none */
#define V003_SPI_SET_CS	    V003_SPI_CMD(0x44)
/* OUT with a data stage: up to 64 bytes, one atomic transfer */
#define V003_SPI_TRANSFER   V003_SPI_CMD(0x45)
/* IN: the MISO bytes of the last transfer (data stage) */
#define V003_SPI_GET_RX	    V003_SPI_CMD(0x46)
/* IN -> the chip select pin, 0xffff when none is driven */
#define V003_SPI_GET_CS	    V003_SPI_CMD(0x47)

/* largest transfer the firmware does in one go (spi_rx_buf) */
#define V003_SPI_MAX_XFER 64

/* Largest payload a control OUT data stage can carry on this firmware
 * (CTRL_OUT_DATA_SIZE in vendor/vendor.c). */
#define V003_CTRL_DATA_MAX 72

/* status bits */
#define V003_I2C_ST_OK		   BIT(0)
#define V003_I2C_ST_ADDR_NACK	   BIT(1)
#define V003_I2C_ST_DATA_NACK	   BIT(2)
#define V003_I2C_ST_STRETCH	   BIT(3)
#define V003_I2C_ST_NOT_CONFIGURED BIT(4)
#define V003_I2C_ST_READ_NACK	   BIT(5)
#define V003_I2C_ST_TIMEOUT	   BIT(6)
#define V003_I2C_ST_BYTES(s)	   (((s) >> 8) & 0xff)
#define V003_I2C_ST_FAILED_BYTE(s) (((s) >> 16) & 0xff)

/* Framed endpoint protocol (vendor/frame.h).
 *
 * Requests go out on EP2 OUT, responses come back on EP3 IN:
 *
 *   request   [size][id][echo][handle][arg...]
 *   response  [size][id][echo][handle][result][payload...]
 *
 * There is no SETUP or status stage, so a command that needs no answer costs
 * two 8 byte packets instead of the three frames a control transfer takes on a
 * low speed device (see the measurements in TODO.md).  `echo` tags a request so
 * a response can be matched to it; the firmware answers every IN token, with a
 * zero length packet while it has nothing, which is why the receive side has to
 * keep asking until the tag comes back. */
#define V003_EP_OUT	   0x02
#define V003_EP_IN	   0x83
#define V003_FRAME_HDR	   8
#define V003_FRAME_MAX	   72 /* request ceiling the firmware accepts */
#define V003_FRAME_RESP	   80 /* response buffer: header + result + payload */
#define V003_FRAME_TIMEOUT 1000 /* ms for a whole request/response exchange */

#define V003_SET_FRAME_MODE  V003_GENERIC_CMD(0x39)
#define V003_GET_FRAME_STATS V003_GENERIC_CMD(0x3a)

/* How commands are carried.  Control is the default: it is the most predictable
 * and wins single round trips, while the framed path wins the commands that
 * want no answer.  AUTO sends write-only commands as frames and reads over the
 * control path, which is the cheapest of both worlds but needs the firmware in
 * frame mode (so the raw EP3 echo stream is unavailable). */
#define V003_TRANSPORT_CONTROL 0
#define V003_TRANSPORT_FRAME   1
#define V003_TRANSPORT_AUTO    2

extern unsigned int v003_transport;

#define V003_USB_TIMEOUT 200 /* ms */
#define V003_DEVICE_VER	 0x1010
#define V003_NGPIO	 56

/* Pins the device itself uses: D+ (PD3), D- (PD4), the D- pull-up switch
 * (PD5) and the boot button (PD6).  Userspace must not drive the bus it is
 * talking over, so the child driver keeps these out of the gpiochip. */
#define V003_PIN_USB_DP	  51
#define V003_PIN_USB_DM	  52
#define V003_PIN_USB_DPU  53
#define V003_PIN_BOOT_BTN 54
/* pins 16..31 have no port behind them on this chip */
#define V003_RESERVED_PINS_FALLBACK (0xffff0000ULL | (0xfULL << V003_PIN_USB_DP))

struct v003_dev;

/* Core API used by the child drivers. */
struct v003_dev *v003_get_dev(struct platform_device *pdev);
int v003_transfer_out(struct v003_dev *v003, u16 cmd, u16 val);
int v003_transfer_in(struct v003_dev *v003, u16 cmd, u16 val, u32 *out);

/* Transport aware wrappers: children describe the command, the core decides
 * how to carry it, so switching transports is a module parameter and not a
 * change in every callback. */
/* what the firmware said it is; NULL when it predates the capability report */
const struct v003_caps *v003_capabilities(struct v003_dev *v003);
/* pins the device owns: the USB pins, the module pins, and the flat pin range
 * that has no port behind it.  Children must keep these out of their ranges. */
u64 v003_reserved_pins(struct v003_dev *v003);

int v003_cmd_out(struct v003_dev *v003, u16 cmd, u16 val);
int v003_cmd_in(struct v003_dev *v003, u16 cmd, u16 val, u32 *out);

/* Commands whose argument or result is a payload rather than a 32 bit word.
 * These always use control transfers: the endpoint path would need the frame
 * queues on the firmware side to be deep enough to matter first.  They return
 * the number of bytes transferred or a negative error. */
int v003_data_out(struct v003_dev *v003, u16 cmd, u16 val, const void *buf,
		  size_t len);
int v003_data_in(struct v003_dev *v003, u16 cmd, u16 val, void *buf,
		 size_t len);

/* Raw framed transfers, for payloads a control data stage cannot carry
 * comfortably (plus the mode switch itself). */
int v003_frame_mode(struct v003_dev *v003, bool on);
int v003_frame_send(struct v003_dev *v003, u16 handle, u16 id, u16 arg,
		    const void *payload, size_t payload_len);
int v003_frame_xfer(struct v003_dev *v003, u16 handle, u16 id, u16 arg,
		    const void *payload, size_t payload_len, void *rx,
		    size_t rx_len, size_t *rx_got);

#endif /* __V003_MFD_H */
