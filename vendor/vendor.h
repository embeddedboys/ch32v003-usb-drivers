#ifndef __VENDOR_H
#define __VENDOR_H

#include "ch32fun.h"

#define __maybe_unused __attribute__((unused))

#define EP0_IN_ADDR  (USB_EP_DIR_IN | 0)
#define EP0_OUT_ADDR (USB_EP_DIR_OUT | 0)
#define EP1_OUT_ADDR (USB_EP_DIR_OUT | 1)
#define EP2_OUT_ADDR (USB_EP_DIR_OUT | 2)
#define EP2_IN_ADDR  (USB_EP_DIR_IN | 2)
#define EP3_OUT_ADDR (USB_EP_DIR_OUT | 3)
#define EP4_IN_ADDR  (USB_EP_DIR_IN | 4)

#define REQ_EP1_OUT 0x02
#define REQ_EP2_IN  0x03

#define V003_CMD(cmd, id)     ((cmd) | (id << 8))
#define V003_CMD_GET_ID(cmd)  (cmd >> 8)
#define V003_CMD_GET_CMD(cmd) (cmd & 0xFF)

/* Detach and re-attach the device on the bus.  The USB pull-up (PD5) is what
 * tells the host a device is there, so a standby sleep releases it and gets a
 * clean disconnect plus a fresh enumeration on the way back instead of a device
 * that ignores everything (vendor/vendor.c, used by the power module). */
extern void v003_usb_detach(void);
extern void v003_usb_attach(void);

/* ------------------------------------------------------------------ */
/* which modules this build contains                                  */
/*                                                                    */
/* The chip has 2 kB of RAM, so a firmware that needs a UART ring is  */
/* not the same binary as one that needs I2C: modules are selected at */
/* build time (`make MODULES=gpio,i2c,spi`) and the device reports     */
/* what it ended up with through V003_GET_CAPABILITIES, so a host      */
/* never probes a module that is not there.                           */
/* ------------------------------------------------------------------ */
#ifndef V003_MODULE_GPIO
#define V003_MODULE_GPIO 1
#endif
#ifndef V003_MODULE_SPI
#define V003_MODULE_SPI 1
#endif
#ifndef V003_MODULE_I2C
#define V003_MODULE_I2C 1
#endif
/* ADC1, one conversion per request; the internal reference and calibration
 * channels make it verifiable without wiring anything up (vendor/adc.h) */
#ifndef V003_MODULE_ADC
#define V003_MODULE_ADC 1
#endif
/* PWM out of TIM1, two channels on PD2 and PA1 (see vendor/pwm.h for why not
 * the other two: PC3 and PC4 collide with a spare GPIO and the SPI chip select) */
#ifndef V003_MODULE_PWM
#define V003_MODULE_PWM 1
#endif
/* USART1 as a byte pipe, remapped to PD0/PD1 because the default mapping is
 * the USB pull-up line and the boot button on this board (vendor/uart.h) */
#ifndef V003_MODULE_UART
#define V003_MODULE_UART 1
#endif
/* a watchdog: configure the timeout, feed it, and learn why the chip last
 * reset.  Three registers and a few bytes of state, no pin, no buffer. */
#ifndef V003_MODULE_WDG
#define V003_MODULE_WDG 0
#endif
/* power management: sleep and standby entry, wake sources and the wake reason.
 * The one module that changes whether the device exists as far as the host is
 * concerned (a sleeping core cannot run the bit banged USB), see vendor/pwr.h for
 * the rules it follows - explicit requests only, an always-armed AWU backstop,
 * and the USB pull-up released for a standby sleep. */
#ifndef V003_MODULE_PWR
#define V003_MODULE_PWR 1
#endif

/* Depth of the ring the USB interrupt uses to hand zero length vendor OUT
 * requests to main().  It only has to cover the gap until main() gets there:
 * each request occupies the wire for a whole low speed control transfer (3 ms)
 * while main() drains the ring in microseconds, so more than a couple of entries
 * is RAM spent on a case that does not happen - 8 entries cost 64 of the 2048
 * bytes.  What does not fit is counted (V003_GET_REQ_DROPS), never silently
 * moved.  Must be a power of two. */
#ifndef V003_NUM_SIMPLE_REQUESTS
#define V003_NUM_SIMPLE_REQUESTS 4
#endif

/* what the device can do, as reported by V003_GET_CAPABILITIES */
#define V003_CAP_GPIO  (1u << 0)
#define V003_CAP_SPI   (1u << 1)
#define V003_CAP_I2C   (1u << 2)
#define V003_CAP_ADC   (1u << 3)
#define V003_CAP_PWM   (1u << 4)
#define V003_CAP_UART  (1u << 5)
#define V003_CAP_WDG   (1u << 7)
#define V003_CAP_PWR   (1u << 8)
/* the framed protocol on the endpoint data path (V003_SET_FRAME_MODE) */
#define V003_CAP_FRAME (1u << 6)

/* pins the device itself owns, as flat pin numbers (port * 16 + pin), the same
 * numbering the GPIO module uses: PC1 = 33, PC5..PC7 = 37..39, PD3..PD6 = 51..54 */
#define V003_PIN_USB_DP	  51
#define V003_PIN_USB_DM	  52
#define V003_PIN_USB_DPU  53
#define V003_PIN_BOOT_BTN 54
#define V003_PIN_I2C_SDA  33
#define V003_PIN_I2C_SCL  34
#define V003_PIN_SPI_SCK  37
#define V003_PIN_SPI_MOSI 38
#define V003_PIN_SPI_MISO 39

/* the 64 bit mask of them, split the way the structure carries it */
/* PWM: TIM1 channels 1 and 2.  The CH32V003 has no PA8..PA11, so the channels
 * sit on the pins the default TIM1 remap gives them (CH1 = PD2, CH2 = PA1,
 * CH3 = PC3, CH4 = PC4); the last two collide with this board's spare GPIO and
 * the SPI chip select, so only the first two are offered.  Defined even when the
 * module is off, because the reserved mask below references them. */
#define V003_PWM_PIN_CH1 50 /* PD2 */
#define V003_PWM_PIN_CH2 1  /* PA1 */
#define V003_PWM_CHANNELS 2

/* UART: USART1 with remap 01 (AFIO_PCFR1 bit 21), TX on PD0 and RX on PD1.
 * Defined even when the module is off, because the reserved mask below
 * references them - the same reason the PWM pins are here. */
#define V003_UART_TX_PIN 48 /* PD0 */
#define V003_UART_RX_PIN 49 /* PD1 */

/* the flat pin space the GPIO module accepts: ports A (0..15), C (32..47) and
 * D (48..63) of the CH32V003; 16..31 has no port behind it and is reserved so
 * userspace cannot write into an address that is not a GPIO at all */
#define V003_NGPIO 56

#define V003_DEVICE_RESERVED_LO						\
	(0xffff0000u /* 16..31: no port there */ |				\
	 (V003_MODULE_PWM ? (1u << V003_PWM_PIN_CH2) : 0u))
#define V003_DEVICE_RESERVED_HI						\
	((1u << (V003_PIN_USB_DP - 32)) |				\
	 (1u << (V003_PIN_USB_DM - 32)) |				\
	 (1u << (V003_PIN_USB_DPU - 32)) |				\
	 (1u << (V003_PIN_BOOT_BTN - 32)) |				\
	 (V003_MODULE_I2C ? ((1u << (V003_PIN_I2C_SDA - 32)) |		\
			     (1u << (V003_PIN_I2C_SCL - 32))) : 0u) |	\
	 (V003_MODULE_SPI ? ((1u << (V003_PIN_SPI_SCK - 32)) |		\
			     (1u << (V003_PIN_SPI_MOSI - 32)) |		\
			     (1u << (V003_PIN_SPI_MISO - 32))) : 0u) |	\
	 (V003_MODULE_PWM ? (1u << (V003_PWM_PIN_CH1 - 32)) : 0u) |		\
	 (V003_MODULE_UART ? ((1u << (V003_UART_TX_PIN - 32)) |			\
			      (1u << (V003_UART_RX_PIN - 32))) : 0u))

/* what a host gets from V003_GET_CAPABILITIES: fixed size, append only, so a
 * host that knows a shorter version keeps working */
struct v003_caps {
	u32 caps;	 /* V003_CAP_* */
	u8 ngpio;	 /* lines the GPIO module drives */
	u8 nadc;	 /* ADC channels */
	u8 npwm;	 /* PWM channels */
	u8 nuart;	 /* UARTs */
	u32 reserved_lo; /* pins 0..31 the device owns */
	u32 reserved_hi; /* pins 32..63 the device owns */
};

/*
 * These names and their capability bits are reserved so a host can be written
 * against them, but a build cannot turn one on until there is code behind it -
 * the same macro gates the implementation *and* the bit the device reports, so
 * enabling it silently would advertise a feature that does not exist.
 */
/* every module named in vendor.h now has an implementation; the guard that used
 * to refuse the reserved names is gone with the last one (uart, then pwr) */

/* Module 0x00: device-wide / generic requests. */
#define V003_GENERIC_MODULE_ID 0x00
#define V003_GENERIC_CMD(cmd)  V003_CMD(cmd, V003_GENERIC_MODULE_ID)

#define V003_GET_DEVICE_VER V003_GENERIC_CMD(0x30)
#define V003_GET_DEVICE_SN  V003_GENERIC_CMD(0x31)
#define V003_GET_EP_STATS   V003_GENERIC_CMD(0x32)
/* Bytes currently queued in the EP3 IN (device -> host) FIFO. */
#define V003_GET_FIFO_LEVEL V003_GENERIC_CMD(0x33)
/* Bytes dropped because the EP3 IN FIFO was full. */
#define V003_GET_FIFO_DROPS V003_GENERIC_CMD(0x34)
/* Vendor OUT requests dropped because the request ring was full. */
#define V003_GET_REQ_DROPS V003_GENERIC_CMD(0x35)
/* IN: the data stage of the last vendor control-OUT (max 64 bytes) */
#define V003_GET_CTRL_OUT_DATA V003_GENERIC_CMD(0x36)
/* IN: number of vendor OUT requests (with or without a data stage) the main
 * loop has finished processing.  Control-OUT data stage work (SPI/I2C
 * transfers) happens in main() context, so a host that reads a result right
 * after sending a request must wait for this counter to advance - otherwise it
 * can still read the previous result. */
#define V003_GET_CTRL_OUT_SEQ V003_GENERIC_CMD(0x37)
/* IN: control-OUT data stages dropped because both slots were busy */
#define V003_GET_CTRL_OUT_DROPS V003_GENERIC_CMD(0x38)
/* IN: bytes of stack that were still untouched at the deepest point reached
 * since main() started (0 = the canary was never painted).  The stack grows
 * down from 0x20000800 straight into the statics, so this is the only way to
 * see how much headroom is really left. */
#define V003_GET_STACK_FREE V003_GENERIC_CMD(0x3b)
/* IN: wValue = number of loop iterations, returns the SysTick delta.  Used to
 * find out how long a vendor handler may run before the low speed USB host
 * gives up on the transfer. */
#define V003_GET_TIMING V003_GENERIC_CMD(0x3f)
/* IN with a data stage: the factory ESIG unique id, 96 bits (12 bytes), read
 * straight out of the system memory area at 0x1FFFF7E8 - see chapter 15 of the
 * reference manual.  The device reports the same value as its USB serial
 * number string (in hex), so a host can identify a specific board. */
#define V003_GET_DEVICE_UID V003_GENERIC_CMD(0x3c)
/* IN with a data stage: struct v003_caps - which modules this build has, how
 * many channels each offers, and which pins the device owns.  A host uses it
 * to decide what to talk to instead of assuming; see notes/firmware.md. */
#define V003_GET_CAPABILITIES V003_GENERIC_CMD(0x3d)
/* where the unique id lives (ch32fun's ESIG_TypeDef): 0x1FFFF7E8 */
#define V003_DEVICE_UID_ADDR (&ESIG->UNIID1)

struct usb_ctrl_msg_ctx {
	u8 msg_len; /* current ctrl msg length */

	u8 byte_left;
	u8 byte_pos;
};

struct usbd_ep_ctx {
	u8 ep_addr;
	u8 *buf;
	u16 bufsize; /* size of the receive buffer */
	u16 byte_pos; /* current write position in buf */
	u16 byte_left; /* remaining bytes of the expected transfer, 0 = per-packet delivery */
	void (*ep_cb)(u8 *data, int len);
};

/* a simple usb request only use ep0, requires one single ctrl transfer */
struct simple_usb_request {
	u16 wValue;

	union {
		struct {
			u8 cmd;
			u8 id;
		};

		u16 wIndex;
	};
} __attribute__((packed));

extern void hexdump(const void *data, uint32_t size);

/* the EP3 IN (device -> host) byte FIFO: queue, drain and inspect it.  Frame
 * responses and the raw echo/SPI stream share it because the two modes are
 * mutually exclusive. */
extern void v003_ep3_in_push(const u8 *data, int len);
extern int v003_ep3_in_pop(u8 *dst, int max);
extern int v003_ep3_in_level(void);
extern void v003_ep3_in_reset(void);

/* number of vendor OUT requests the main loop has finished (see vendor.c) */
extern u32 v003_ctrl_out_seq(void);

/* module 0x00 dispatch, shared by the control path and the framed EP path */
extern u32 v003_generic_in_request(u16 cmd, u16 data);
/* returns 1 when the command was an action that answers with nothing */
extern int v003_generic_out_request(u16 cmd, u16 data);

#endif /* __VENDOR_H */
