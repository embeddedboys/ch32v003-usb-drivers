#include "ch32fun.h"
#include "rv003usb.h"
#include <stdio.h>
#include <string.h>

#include "vendor.h"
#include "v003_usb_ids.h" /* V003_DEVICE_UID_SIZE, V003_SERIAL_DESC_SIZE */
#include "gpio.h"
#if V003_MODULE_SPI
#include "spi.h"
#endif
#if V003_MODULE_I2C
#include "i2c.h"
#endif
#if V003_MODULE_WDG
#include "wdg.h"
#endif
#if V003_MODULE_PWM
#include "pwm.h"
#endif
#if V003_MODULE_ADC
#include "adc.h"
#endif
#if V003_MODULE_UART
#include "uart.h"
#endif
#include "frame.h"

#define V003_HW_ID	 0x200
#define V003_USB_TIMEOUT 200

/* Printing the UEvent ring goes out over the bit-banged debug channel, which
 * costs milliseconds per event and blocks the main loop (the channel only
 * drains while something is reading it on the other side).  Anything the main
 * loop still owes the host then stalls with it - a zero length control OUT such
 * as V003_SET_FRAME_MODE is queued for main, so the framed path silently never
 * gets enabled.  Off by default: build with `make DEBUG_EVENTS=1` to bring the
 * spam back for a bring-up session. */
#ifndef VENDOR_DEBUG_EVENTS
#define VENDOR_DEBUG_EVENTS 0
#endif

#define SIMPLE_USB_REQUEST_SIZE	     V003_NUM_SIMPLE_REQUESTS
#define SIMPLE_USB_REQUEST_MASK	     (SIMPLE_USB_REQUEST_SIZE - 1)
#define SIMPLE_USB_REQUEST_NEXT(idx) ((idx + 1) & SIMPLE_USB_REQUEST_MASK)

/* Single producer / single consumer ring of vendor OUT requests without a
 * data stage: usb_handle_other_control_message() pushes from the USB
 * interrupt, main() pops.  Only the interrupt writes head and the ring
 * slots, only main() writes tail, so no locking is needed - but a full ring
 * must drop the new request instead of moving tail, which belongs to main(). */
static struct usb_urb simple_usb_requests[SIMPLE_USB_REQUEST_SIZE];
static volatile uint8_t head = 0, tail = 0;
static volatile u32 simple_usb_requests_dropped;

static void simple_usb_request_push(struct usb_urb urb)
{
	uint8_t next = SIMPLE_USB_REQUEST_NEXT(head);

	if (next == tail) {
		/* full: the main loop has not caught up, drop this request */
		simple_usb_requests_dropped++;
		return;
	}

	simple_usb_requests[head] = urb;
	head = next;
}

static struct usb_urb *simple_usb_request_pop(void)
{
	int event = tail;

	/* empty */
	if (head == tail)
		return NULL;

	tail = SIMPLE_USB_REQUEST_NEXT(tail);
	return &simple_usb_requests[event];
}

#define V003_DEVICE_VER 0x1010

static volatile u32 ep_rx_count[4];
static volatile u32 ep_tx_count[4];
static volatile u32 ep3_tx_bytes;

/* advanced by the main loop each time a vendor OUT request has been handled, so
 * the host can tell when the work done outside the USB interrupt (control-OUT
 * data stages for SPI/I2C) has completed - see V003_GET_CTRL_OUT_SEQ */
static volatile u32 ctrl_out_seq;

/* ------------------------------------------------------------------ */
/* EP3 IN (device -> host) transmit FIFO                              */
/*                                                                    */
/* Producers: any OUT endpoint callback (currently EP1/EP2 OUT echo    */
/* whatever the host wrote, which makes the data path testable without */
/* a protocol on top).  Consumer: usb_handle_user_in_request() for     */
/* endp == 3, at most one 8 byte packet per IN token.                  */
/* Both run from the USB interrupt, so no locking is needed.           */
/* ------------------------------------------------------------------ */

#define EP3_TX_FIFO_SIZE 64
#define EP3_TX_FIFO_MASK (EP3_TX_FIFO_SIZE - 1)

static u8 ep3_tx_fifo[EP3_TX_FIFO_SIZE];
/* usb_send_data() bit-bangs synchronously, so one staging packet is enough */
static u8 ep3_tx_packet[8];
static volatile u16 ep3_tx_head;
static volatile u16 ep3_tx_tail;
static volatile u32 ep3_tx_dropped;

static int ep3_tx_level(void)
{
	return (int)(u16)(ep3_tx_head - ep3_tx_tail);
}

static void ep3_tx_push(const u8 *data, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (ep3_tx_level() >= EP3_TX_FIFO_SIZE) {
			ep3_tx_dropped++;
			continue;
		}

		ep3_tx_fifo[ep3_tx_head & EP3_TX_FIFO_MASK] = data[i];
		ep3_tx_head++;
	}
}

static int ep3_tx_pop(u8 *dst, int max)
{
	int len = 0;

	while (len < max && ep3_tx_tail != ep3_tx_head) {
		dst[len++] = ep3_tx_fifo[ep3_tx_tail & EP3_TX_FIFO_MASK];
		ep3_tx_tail++;
	}

	return len;
}

/* used by the other modules (e.g. spi and frame) to queue data for the host */
void v003_ep3_in_push(const u8 *data, int len)
{
	ep3_tx_push(data, len);
}

int v003_ep3_in_pop(u8 *dst, int max)
{
	return ep3_tx_pop(dst, max);
}

int v003_ep3_in_level(void)
{
	return ep3_tx_level();
}

/* Used when switching the meaning of EP3 IN (raw echo stream <-> framed
 * responses): whatever is queued belongs to the old mode and would otherwise
 * be handed to the host as a bogus first response. */
void v003_ep3_in_reset(void)
{
	ep3_tx_head = 0;
	ep3_tx_tail = 0;
}

/* Control-OUT data stages.
 *
 * The USB interrupt receives them (rv003usb writes a completion marker holding
 * the expected length into the first word of the buffer) while the main loop
 * processes them - and processing one can take milliseconds (the I2C bit
 * banging), so a second request can arrive in the meantime.  A single buffer
 * therefore raced: the interrupt overwrote the payload the main loop was still
 * working from, which put the *previous* request's bytes on the I2C bus.
 *
 * Now the interrupt claims a free slot from a tiny ring and records the
 * request, and the main loop only consumes slots whose marker is complete.
 * Two slots are plenty because the host waits for each request, and anything
 * beyond that is counted instead of corrupting an in-flight transfer.
 *
 * slot layout: [0..3] completion marker written by rv003usb, [4..] payload
 */
#define CTRL_OUT_SLOTS 2
/* 72 = one device address byte + a two byte word address + a full 64 byte
 * EEPROM page, which is what V003_I2C_MEM_WRITE_READ needs to carry */
#define CTRL_OUT_DATA_SIZE 72

struct ctrl_out_req {
	u8 slot;
	u16 len;
	struct usb_urb urb;
};

static u8 ctrl_out_slots[CTRL_OUT_SLOTS][4 + CTRL_OUT_DATA_SIZE];
static u8 ctrl_out_scratch[4 + CTRL_OUT_DATA_SIZE]; /* dropped requests land here */
static volatile struct ctrl_out_req ctrl_out_q[CTRL_OUT_SLOTS];
static volatile u8 ctrl_out_head, ctrl_out_tail;
static volatile u8 ctrl_out_slot_used[CTRL_OUT_SLOTS];
static volatile u32 ctrl_out_dropped;
/* last data stage the main loop finished, for V003_GET_CTRL_OUT_DATA */
static volatile u8 ctrl_out_done_slot;
static volatile u16 ctrl_out_done_len;

/* ------------------------------------------------------------------ */
/* capability report                                                  */
/*                                                                    */
/* A constant in flash: the data stage points straight at it, which   */
/* costs no RAM and no code (the same trick the descriptors use).      */
/* ------------------------------------------------------------------ */

static const struct v003_caps device_caps = {
	.caps = 0
#if V003_MODULE_GPIO
		| V003_CAP_GPIO
#endif
#if V003_MODULE_SPI
		| V003_CAP_SPI
#endif
#if V003_MODULE_I2C
		| V003_CAP_I2C
#endif
#if V003_MODULE_ADC
		| V003_CAP_ADC
#endif
#if V003_MODULE_PWM
		| V003_CAP_PWM
#endif
#if V003_MODULE_UART
		| V003_CAP_UART
#endif
#if V003_MODULE_WDG
		| V003_CAP_WDG
#endif
#if V003_MODULE_PWR
		| V003_CAP_PWR
#endif
		| V003_CAP_FRAME,
	.ngpio = V003_NGPIO,
	#if V003_MODULE_ADC
	.nadc = V003_ADC_CHANNELS,
#else
	.nadc = 0,
#endif
#if V003_MODULE_PWM
	.npwm = V003_PWM_CHANNELS,
#else
	.npwm = 0,
#endif
#if V003_MODULE_UART
	.nuart = V003_UART_COUNT,
#else
	.nuart = 0,
#endif
	.reserved_lo = V003_DEVICE_RESERVED_LO,
	.reserved_hi = V003_DEVICE_RESERVED_HI,
};

/* ------------------------------------------------------------------ */
/* serial number                                                      */
/*                                                                    */
/* The descriptor is built at boot out of the factory ESIG unique id, */
/* so every board reports its own serial number instead of a fixed     */
/* placeholder.  Only the string lives in RAM (see usb_config.h).      */
/* ------------------------------------------------------------------ */

uint8_t v003_serial_descriptor[V003_SERIAL_DESC_SIZE];

/* the unique id, copied out of the system memory area once at boot: reading it
 * from the USB interrupt breaks the bit banged timing (the ISR has a few
 * microseconds and the ESIG is a slow read), which showed up as the host
 * failing the transfer with EIO */
static u8 device_uid[V003_DEVICE_UID_SIZE];

static void serial_from_esig(void)
{
	static const char hex[] = "0123456789abcdef";
	const u8 *uid = (const u8 *)V003_DEVICE_UID_ADDR;
	int i;

	for (i = 0; i < V003_DEVICE_UID_SIZE; i++)
		device_uid[i] = uid[i];

	v003_serial_descriptor[0] = V003_SERIAL_DESC_SIZE;
	v003_serial_descriptor[1] = 3; /* bDescriptorType: string */
	for (i = 0; i < V003_DEVICE_UID_SIZE; i++) {
		/* UTF-16LE hex, high nibble first, so the host sees the same
		 * byte order it gets from V003_GET_DEVICE_UID */
		v003_serial_descriptor[2 + i * 4 + 0] = hex[uid[i] >> 4];
		v003_serial_descriptor[2 + i * 4 + 1] = 0;
		v003_serial_descriptor[2 + i * 4 + 2] = hex[uid[i] & 0x0f];
		v003_serial_descriptor[2 + i * 4 + 3] = 0;
	}
}

/* ------------------------------------------------------------------ */
/* stack canary                                                       */
/*                                                                    */
/* The CH32V003 has 2 kB of RAM: the stack starts at the top and grows */
/* straight down into the statics, so running out of stack does not    */
/* fault, it silently corrupts variables.  main() paints the free RAM  */
/* below its own frame and V003_GET_STACK_FREE reports how much of it  */
/* was still untouched at the deepest point.                          */
/* ------------------------------------------------------------------ */

#define STACK_CANARY_PATTERN 0x5a
#define STACK_PAINT_MARGIN   64

extern u32 _ebss;

static u8 *stack_paint_base;
static u8 *stack_paint_top;
static volatile u32 stack_free_cache;
static volatile u8 stack_free_refresh;

/* noinline so the address of a local really is below main()'s frame: with
 * -Os there is no frame pointer, so __builtin_frame_address(0) would be 0 and
 * the paint would run away over the whole address space */
static void __attribute__((noinline)) stack_canary_paint(void)
{
	u8 probe;
	u8 *sp = (u8 *)((u32)&probe & ~(u32)3);
	u8 *base = (u8 *)&_ebss;
	u8 *top = sp - STACK_PAINT_MARGIN;
	u8 *p;

	if (base >= top)
		return;

	for (p = base; p < top; p++)
		*p = STACK_CANARY_PATTERN;

	/* published last: a reader treats a set base as "painting is done" */
	stack_paint_top = top;
	stack_paint_base = base;
}

/* Reading the whole painted region takes thousands of cycles, which is far
 * more than the low speed USB budget for answering a control request from the
 * interrupt (the SETUP handshake has to be acknowledged within microseconds -
 * a long handler makes the host fail the transfer with EIO).  So the host
 * arms a measurement and main() refreshes the cached value: the first read
 * returns the stale value, the next one the fresh result. */
static u32 stack_free_bytes(void);

u32 v003_stack_free(void)
{
	stack_free_refresh = 1;
	return stack_free_cache;
}

static void stack_free_poll(void)
{
	if (stack_free_refresh) {
		stack_free_cache = stack_free_bytes();
		stack_free_refresh = 0;
	}
}

static u32 stack_free_bytes(void)
{
	u8 *p;

	if (!stack_paint_base || !stack_paint_top)
		return 0;

	for (p = stack_paint_base; p < stack_paint_top; p++) {
		if (*p != STACK_CANARY_PATTERN)
			break;
	}

	return (u32)(p - stack_paint_base);
}

/* How many vendor OUT requests the main loop has finished.  Commands that are
 * carried by a control-OUT data stage are executed from the main loop, so the
 * interrupt answers a control transfer before the work is done: a host that
 * wants to read a result has to know that its own request has been processed,
 * and this counter is what tells it (the I2C module stamps it into
 * V003_I2C_GET_RESULT). */
u32 v003_ctrl_out_seq(void)
{
	return ctrl_out_seq;
}

u32 v003_generic_in_request(u16 cmd, u16 data)
{
	LogUEvent(SysTick->CNT, cmd, data, 0);
	switch (cmd) {
	case V003_GET_DEVICE_VER:
		return V003_DEVICE_VER;
	case V003_GET_DEVICE_SN:
		/* the first word of the factory unique id, so a host that only
		 * wants an identifier does not have to read 12 bytes; the full
		 * id is V003_GET_DEVICE_UID */
		return (u32)device_uid[0] | ((u32)device_uid[1] << 8) |
		       ((u32)device_uid[2] << 16) | ((u32)device_uid[3] << 24);
	case V003_GET_EP_STATS:
		switch (data) {
		case 0:
			return ep_rx_count[0];
		case 1:
			return ep_rx_count[1];
		case 2:
			return ep_rx_count[2];
		case 3:
			return ep_tx_count[3];
		case 4:
			return ep3_tx_bytes;
		default:
			return 0;
		}
	case V003_GET_FIFO_LEVEL:
		return ep3_tx_level();
	case V003_GET_FIFO_DROPS:
		return ep3_tx_dropped;
	case V003_GET_REQ_DROPS:
		return simple_usb_requests_dropped;
	case V003_GET_CTRL_OUT_SEQ:
		return ctrl_out_seq;
	case V003_GET_CTRL_OUT_DROPS:
		return ctrl_out_dropped;
	case V003_GET_FRAME_STATS:
		return v003_frame_stats();
	case V003_GET_STACK_FREE:
		return v003_stack_free();
	case V003_GET_TIMING: { /* EXP: spin wValue iterations, report ticks */
		volatile u32 i;
		u32 t0 = SysTick->CNT;

		for (i = 0; i < data; i++)
			;
		return SysTick->CNT - t0;
	}
	default:
		return 0;
	}
}

/* generic commands that change state instead of answering; returns 1 when the
 * command was one of them */
int v003_generic_out_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_SET_FRAME_MODE:
		v003_frame_enable(data ? 1 : 0);
		return 1;
	case V003_GET_EP_STATS:
		/* OUT V003_GET_EP_STATS with wValue == 0xff resets the counters */
		if (data == 0xff) {
			memset((void *)ep_rx_count, 0, sizeof(ep_rx_count));
			memset((void *)ep_tx_count, 0, sizeof(ep_tx_count));
			ep3_tx_bytes = 0;
			ep3_tx_dropped = 0;
			simple_usb_requests_dropped = 0;
		}
		return 1;
	default:
		return 0;
	}
}


static void usb_handle_control_out_request(struct usb_urb *urb)
{
	LogUEvent(urb->wRequestTypeLSBRequestMSB, urb->wIndex, urb->wValue,
		  urb->wLength);

	if (v003_generic_out_request(urb->wIndex, urb->wValue))
		return;

	switch (V003_CMD_GET_ID(urb->wIndex)) {
	case V003_GPIO_MODULE_ID:
		handle_gpio_out_request(urb->wIndex, urb->wValue);
		break;
#if V003_MODULE_SPI
	case V003_SPI_MODULE_ID:
		handle_spi_out_request(urb->wIndex, urb->wValue);
		break;
#endif
#if V003_MODULE_I2C
	case V003_I2C_MODULE_ID:
		handle_i2c_out_request(urb->wIndex, urb->wValue);
		break;
#endif
#if V003_MODULE_WDG
	case V003_WDG_MODULE_ID:
		handle_wdg_out_request(urb->wIndex, urb->wValue);
		break;
#endif
#if V003_MODULE_ADC
	case V003_ADC_MODULE_ID:
		/* a conversion is too slow for the interrupt, so the request
		 * only records what to convert and main() runs it */
		handle_adc_out_request(urb->wIndex, urb->wValue);
		break;
#endif
#if V003_MODULE_UART
	case V003_UART_MODULE_ID:
		uart_handle_out_request(urb->wIndex, urb->wValue);
		break;
#endif
	default:
		/* unsupported module request */
		break;
	}
}

static void usb_handle_control_in_request(struct usb_endpoint *e,
					  struct usb_urb *s)
{
	static u32 val = 0;
	u8 *data = (u8 *)&val;
	u16 len = s->wLength;

	if (s->wIndex == V003_GET_CTRL_OUT_DATA) {
		/* hand back the data stage of the last vendor control-OUT; the
		 * completion marker holds the length rv003usb received, so the
		 * host never sees stale bytes beyond it */
		u16 have = ctrl_out_done_len;

		if (have > CTRL_OUT_DATA_SIZE)
			have = 0;

		data = ctrl_out_slots[ctrl_out_done_slot] + 4;
		len = have < s->wLength ? have : s->wLength;
#if V003_MODULE_SPI
	} else if (s->wIndex == V003_SPI_GET_RX) {
		/* the MISO bytes of the last SPI transfer */
		u16 have = spi_rx_length();

		data = (u8 *)spi_rx_data();
		len = have < s->wLength ? have : s->wLength;
#endif
	} else if (s->wIndex == V003_GET_CAPABILITIES) {
		/* straight out of flash, see device_caps */
		data = (u8 *)&device_caps;
		len = s->wLength < sizeof(device_caps) ? s->wLength
						      : sizeof(device_caps);
	} else if (s->wIndex == V003_GET_DEVICE_UID) {
		/* the RAM copy serial_from_esig() took at boot - never the ESIG
		 * area itself, see there */
		data = device_uid;
		len = s->wLength < V003_DEVICE_UID_SIZE ? s->wLength
						       : V003_DEVICE_UID_SIZE;
#if V003_MODULE_UART
	} else if (s->wIndex == V003_UART_GET_CFG) {
		/* the configuration as the device holds it, actual_baud included */
		const struct v003_uart_cfg *cfg = uart_cfg_state();

		data = (u8 *)cfg;
		len = s->wLength > sizeof(*cfg) ? sizeof(*cfg) : s->wLength;
	} else if (s->wIndex == V003_UART_READ) {
		/* the contiguous run the receiver is holding; a short reply just
		 * means the ring wrapped and the host asks again */
		u16 want = s->wValue < s->wLength ? s->wValue : s->wLength;

		len = uart_rx_take(want, &data);
#endif
#if V003_MODULE_PWM
	} else if (s->wIndex == V003_PWM_GET) {
		/* what the hardware ended up with, for the channel in wValue */
		const struct v003_pwm_cfg *cfg = pwm_channel_state(s->wValue);

		data = (u8 *)cfg;
		len = cfg && s->wLength > sizeof(*cfg) ? sizeof(*cfg) : s->wLength;
		if (!cfg)
			len = 0;
#endif
#if V003_MODULE_I2C && V003_I2C_TRACER
	} else if (s->wIndex == V003_I2C_GET_TRACE) {
		/* raw clock trace, two bytes per rising SCL edge */
		u16 total = i2c_trace_bytes();
		u16 off = s->wValue;

		if (off > total)
			off = total;

		data = (u8 *)i2c_trace_ptr() + off;
		len = total - off;
		if (len > s->wLength)
			len = s->wLength;
#endif
#if V003_MODULE_I2C
	} else if (s->wIndex == V003_I2C_GET_RX) {
		/* the bytes read by the last I2C transfer */
		u16 have = i2c_rx_length();

		data = (u8 *)i2c_rx_data();
		len = have < s->wLength ? have : s->wLength;
#endif
	} else {
		switch (V003_CMD_GET_ID(s->wIndex)) {
		case V003_GENERIC_MODULE_ID:
			val = v003_generic_in_request(s->wIndex, s->wValue);
			break;
		case V003_GPIO_MODULE_ID:
			val = handle_gpio_in_request(s->wIndex, s->wValue);
			break;
#if V003_MODULE_SPI
		case V003_SPI_MODULE_ID:
			val = handle_spi_in_request(s->wIndex, s->wValue);
			break;
#endif
#if V003_MODULE_I2C
		case V003_I2C_MODULE_ID:
			val = handle_i2c_in_request(s->wIndex, s->wValue);
			break;
#endif
#if V003_MODULE_WDG
		case V003_WDG_MODULE_ID:
			val = handle_wdg_in_request(s->wIndex, s->wValue);
			break;
#endif
#if V003_MODULE_PWM
		case V003_PWM_MODULE_ID:
			val = handle_pwm_in_request(s->wIndex, s->wValue);
			break;
#endif
#if V003_MODULE_ADC
		case V003_ADC_MODULE_ID:
			/* only the result is read here; the conversion itself is
			 * an OUT request the main loop runs (see vendor/adc.h) */
			val = handle_adc_in_request(s->wIndex, s->wValue);
			break;
#endif
#if V003_MODULE_UART
		case V003_UART_MODULE_ID:
			val = handle_uart_in_request(s->wIndex, s->wValue);
			break;
#endif
		default:
			/* unsupported module request */
			break;
		}
	}

	e->opaque = data;
	e->max_len = len;
}

/* ------------------------------------------------------------------ */
/* endpoint data path                                                 */
/* ------------------------------------------------------------------ */

/* EP0 packets are 8 bytes (USB 1.1 low speed), EP1/EP2 are 8 byte interrupt
 * OUT endpoints and deliver per packet, so a 16 byte staging buffer is plenty.
 * These used to be 64 bytes each: with 2 kB of RAM in total that memory is
 * better spent on the frame queues than on headroom nothing can ever use. */
static u8 ep0_read_buffer[16];
static u8 ep1_read_buffer[16];
static u8 ep2_read_buffer[16];

static void ctrl_out_cb(u8 *data, int len);
static void ep1_out_cb(u8 *data, int len);
static void ep2_out_cb(u8 *data, int len);

static struct usbd_ep_ctx eps_ctxs[] = {
	{
		.ep_addr = EP0_OUT_ADDR,
		.buf = ep0_read_buffer,
		.bufsize = sizeof(ep0_read_buffer),
		.ep_cb = ctrl_out_cb,
	},
	{
		.ep_addr = EP1_OUT_ADDR,
		.buf = ep1_read_buffer,
		.bufsize = sizeof(ep1_read_buffer),
		.ep_cb = ep1_out_cb,
	},
	{
		.ep_addr = EP2_OUT_ADDR,
		.buf = ep2_read_buffer,
		.bufsize = sizeof(ep2_read_buffer),
		.ep_cb = ep2_out_cb,
	},
};

static void ctrl_out_cb(u8 *data, int len)
{
	/* data received on EP0 that is not part of a control-OUT data stage */
	LogUEvent(0xeeee0000 | len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
		  0);
}

/* EP1 OUT: the host wrote a packet, echo it back through EP3 IN so the
 * endpoint data path can be exercised end to end. */
static void ep1_out_cb(u8 *data, int len)
{
	LogUEvent(0x11110000 | len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
		  0);

	/* in frame mode EP3 IN carries responses only: echoing here would
	 * interleave host traffic with the response stream */
	if (v003_frame_active())
		return;

	ep3_tx_push(data, len);
}

/* EP2 OUT: the frame stream, or the SPI MOSI stream, or the echo channel */
static void ep2_out_cb(u8 *data, int len)
{
	LogUEvent(0x22220000 | len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
		  0);

	if (v003_frame_active()) {
		v003_frame_feed(data, len);
		return;
	}

#if V003_MODULE_SPI
	if (spi_enabled()) {
		spi_out_bytes(data, len);
		return;
	}
#endif

	ep3_tx_push(data, len);
}

/* called from the main loop when a control-OUT data stage completed */
static void usb_handle_control_out_data(struct usb_urb *urb, u8 *data, int len)
{
	LogUEvent(0xcccc0000 | len, urb->wIndex, urb->wValue, 0);

#if V003_MODULE_SPI
	/* an SPI transfer uses the payload as MOSI data instead of a request
	 * extension, and keeps the sampled bytes for V003_SPI_GET_RX */
	if (urb->wIndex == V003_SPI_TRANSFER) {
		spi_transfer_bytes(data, len);
		return;
	}
#endif

#if V003_MODULE_I2C
	/* I2C write/read carry their address and payload in the data stage too */
	if (i2c_handle_control_data(urb->wIndex, urb->wValue, data, len))
		return;
#endif

#if V003_MODULE_PWM
	if (urb->wIndex == V003_PWM_SET) {
		pwm_handle_control_data(data, len);
		return;
	}
#endif

#if V003_MODULE_UART
	if (V003_CMD_GET_ID(urb->wIndex) == V003_UART_MODULE_ID) {
		uart_handle_control_data(urb->wIndex, data, len);
		return;
	}
#endif

	/* data stage is currently only used for simple request extensions;
	 * dispatch the request itself as well */
	usb_handle_control_out_request(urb);
}

/* will be called when more than 8 bytes on ep0 or any length of bytes on other endpoints */
void usb_handle_user_data(struct usb_endpoint *e, int current_endpoint,
			  uint8_t *data, int len, struct rv003usb_internal *ist)
{
	LogUEvent(SysTick->CNT, 0xffffffff, current_endpoint, len);

	if (current_endpoint < 0 || current_endpoint >= (int)ARRAY_SIZE(eps_ctxs))
		return;

	struct usbd_ep_ctx *ctx = &eps_ctxs[current_endpoint];

	if (!ctx->buf || len <= 0)
		return;

	if (len > ctx->bufsize - ctx->byte_pos)
		len = ctx->bufsize - ctx->byte_pos;

	memcpy(ctx->buf + ctx->byte_pos, data, len);
	ctx->byte_pos += len;
	ep_rx_count[current_endpoint] += len;

	if (ctx->byte_left > 0) {
		/* a transfer with a known expected length (set by a control
		 * request): deliver once the whole message arrived */
		ctx->byte_left -= len;
		if (ctx->byte_left == 0) {
			ctx->ep_cb(ctx->buf, ctx->byte_pos);
			ctx->byte_pos = 0;
		}
	} else {
		/* no expected length: deliver per packet */
		ctx->ep_cb(ctx->buf, ctx->byte_pos);
		ctx->byte_pos = 0;
	}
}

void usb_handle_user_in_request(struct usb_endpoint *e, uint8_t *scratchpad,
				int endp, uint32_t sendtok,
				struct rv003usb_internal *ist)
{
	LogUEvent(SysTick->CNT, endp, sendtok, 0);

	ep_tx_count[endp]++;

	if (endp == 3) {
		/* EP3 IN: send up to one packet of queued data, an empty packet
		 * means "nothing queued" to the host.  Frame responses go into
		 * the same FIFO - frame mode and the raw echo/SPI stream are
		 * mutually exclusive, so one FIFO is all that is needed */
		int len = ep3_tx_pop(ep3_tx_packet, sizeof(ep3_tx_packet));

		if (len > 0) {
			ep3_tx_bytes += len;
			usb_send_data(ep3_tx_packet, len, 0, sendtok);
			return;
		}
	}

	usb_send_empty(sendtok);
}

/* SET_CONFIGURATION (bRequest 0x09) reaches us because rv003usb does not
 * handle it.  The USB spec says every endpoint's data toggle goes back to
 * DATA0 when the configuration is set, but rv003usb only resets the toggle of
 * the endpoint that received a SETUP.  A host that reopens the endpoints
 * without a bus reset - a userspace libusb program, or the kernel driver after
 * the device was already used by something else - starts at DATA0 while the
 * device still expects the parity left over from the previous session.  A
 * mismatched OUT packet is acknowledged and then thrown away, and because the
 * device's toggle does not advance on a mismatch, that endpoint stays dead
 * (the host sees successful transfers while the firmware never sees the data)
 * until the device is reset. */
static void usb_reset_endpoint_toggles(struct rv003usb_internal *ist)
{
	int i;

	for (i = 0; i < ENDPOINTS; i++) {
		ist->eps[i].count = 0;
		ist->eps[i].toggle_out = 0;
		/* EP0 data stages start at DATA1, the rest at DATA0 */
		ist->eps[i].toggle_in = (i == 0) ? 1 : 0;
	}
}

void usb_handle_other_control_message(struct usb_endpoint *e, struct usb_urb *s,
				      struct rv003usb_internal *ist)
{
	LogUEvent(s->wRequestTypeLSBRequestMSB, s->wIndex, s->wValue,
		  s->wLength);

	if (s->wRequestTypeLSBRequestMSB == 0x0900) { /* SET_CONFIGURATION */
		usb_reset_endpoint_toggles(ist);
		return;
	}

	/* request type is not vendor */
	if (!(s->wRequestTypeLSBRequestMSB & 0x40))
		return;

	if (s->wRequestTypeLSBRequestMSB & USB_CONTROL_IN_EP0) {
		usb_handle_control_in_request(e, s);
		return;
	}

	/* vendor OUT */
	if (s->wLength > 0) {
		/* control-OUT with a data stage: claim a free slot, tell rv003usb
		 * to fill it (it stores the expected length into the first word
		 * when the transfer completes) and queue it for the main loop. */
		u16 len = s->wLength;
		u8 slot = CTRL_OUT_SLOTS, next;
		u8 i;

		if (len > CTRL_OUT_DATA_SIZE)
			len = CTRL_OUT_DATA_SIZE;

		for (i = 0; i < CTRL_OUT_SLOTS; i++) {
			if (!ctrl_out_slot_used[i]) {
				slot = i;
				break;
			}
		}

		next = (u8)((ctrl_out_head + 1) % CTRL_OUT_SLOTS);

		if (slot >= CTRL_OUT_SLOTS || next == ctrl_out_tail) {
			/* both slots busy or the ring is full: receive it into
			 * scratch so rv003usb has somewhere to write, and drop
			 * it instead of corrupting an in-flight transfer */
			ctrl_out_dropped++;
			e->opaque = (u8 *)ctrl_out_scratch;
			e->max_len = len;
			ist->setup_request = 2;
			return;
		}

		ctrl_out_slot_used[slot] = 1;
		/* Clear the completion marker *before* rv003usb fills the slot.
		 * The marker is the expected length, so a slot reused for a
		 * request of the same length still carried the previous marker
		 * and the main loop consumed the slot before the new payload had
		 * arrived - which put the *previous* request's bytes on the I2C
		 * bus even though the host had sent the new ones. */
		*(volatile u32 *)ctrl_out_slots[slot] = 0;
		ctrl_out_q[ctrl_out_head].slot = slot;
		ctrl_out_q[ctrl_out_head].len = len;
		ctrl_out_q[ctrl_out_head].urb = *s;
		ctrl_out_head = next;

		e->opaque = (u8 *)ctrl_out_slots[slot];
		e->max_len = len;
		ist->setup_request = 2;
	} else {
		/* zero-length vendor OUT: defer processing to the main loop */
		simple_usb_request_push(*s);
	}
}

int main()
{
	SystemInit();

	/* before anything can look at the reset flags, and before usb_setup():
	 * the serial descriptor has to be ready for enumeration */
#if V003_MODULE_WDG
	wdg_reset_cause_capture();
#endif
	serial_from_esig();

	stack_canary_paint();

	funGpioInitAll();

	usb_setup();

	for (;;) {
		uint32_t *ue = GetUEvent();

#if VENDOR_DEBUG_EVENTS
		if (ue)
			printf("0x%lx 0x%lx 0x%lx 0x%lx\n", ue[0], ue[1], ue[2],
			       ue[3]);
#else
		(void)ue; /* still pop the ring so the interrupt keeps room */
#endif

		/* consume control-OUT data stages whose marker is complete */
		while (ctrl_out_tail != ctrl_out_head) {
			struct ctrl_out_req *r = (struct ctrl_out_req *)
						 &ctrl_out_q[ctrl_out_tail];
			u8 *buf = ctrl_out_slots[r->slot];

			if (*(volatile u32 *)buf != (u32)r->len)
				break; /* still being received */

			ctrl_out_done_slot = r->slot;
			ctrl_out_done_len = r->len;
			usb_handle_control_out_data(&r->urb, buf + 4, r->len);

			ctrl_out_slot_used[r->slot] = 0;
			ctrl_out_tail = (u8)((ctrl_out_tail + 1) % CTRL_OUT_SLOTS);
			ctrl_out_seq++;
		}

		/* process queued vendor OUT requests */
		struct usb_urb *req = simple_usb_request_pop();

		if (req) {
			usb_handle_control_out_request(req);
			ctrl_out_seq++;
		}

		/* answer framed requests that arrived on EP2 OUT */
		v003_frame_poll();

		/* refresh the stack measurement outside the USB interrupt */
		stack_free_poll();
	}

	return 0;
}
