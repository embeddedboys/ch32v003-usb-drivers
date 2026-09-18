#include "ch32fun.h"
#include "rv003usb.h"
#include <stdio.h>
#include <string.h>

#include "vendor.h"
#include "gpio.h"

#define V003_GENERIC_MODULE_ID 0x00
#define V003_GENERIC_CMD(cmd)  V003_CMD(cmd, V003_GENERIC_MODULE_ID)
#define V003_GET_DEVICE_VER    V003_GENERIC_CMD(0x30)
#define V003_GET_DEVICE_SN     V003_GENERIC_CMD(0x31)
#define V003_GET_EP_STATS      V003_GENERIC_CMD(0x32)

#define V003_HW_ID	 0x200
#define V003_USB_TIMEOUT 200

#define SIMPLE_USB_REQUEST_SIZE	     8
#define SIMPLE_USB_REQUEST_MASK	     (SIMPLE_USB_REQUEST_SIZE - 1)
#define SIMPLE_USB_REQUEST_NEXT(idx) ((idx + 1) & SIMPLE_USB_REQUEST_MASK)

static struct usb_urb simple_usb_requests[8];
static volatile uint8_t head = 0, tail = 0;

static void simple_usb_request_push(struct usb_urb urb)
{
	simple_usb_requests[head] = urb;
	head = SIMPLE_USB_REQUEST_NEXT(head);

	if (head == tail)
		tail = SIMPLE_USB_REQUEST_NEXT(tail);
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
#define V003_DEVICE_SN	0x12345678

static volatile u32 ep_rx_count[4];
static volatile u32 ep_tx_count[4];

static u32 handle_generic_in_request(u16 cmd, u16 data)
{
	LogUEvent(SysTick->CNT, cmd, data, 0);
	switch (cmd) {
	case V003_GET_DEVICE_VER:
		return V003_DEVICE_VER;
	case V003_GET_DEVICE_SN:
		return V003_DEVICE_SN;
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
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static void usb_handle_control_out_request(struct usb_urb *urb)
{
	LogUEvent(urb->wRequestTypeLSBRequestMSB, urb->wIndex, urb->wValue,
		  urb->wLength);

	switch (V003_CMD_GET_ID(urb->wIndex)) {
	case V003_GENERIC_MODULE_ID:
		/* reset ep stats: OUT V003_GET_EP_STATS with wValue == 0xff */
		if (urb->wIndex == V003_GET_EP_STATS && urb->wValue == 0xff) {
			memset((void *)ep_rx_count, 0, sizeof(ep_rx_count));
			memset((void *)ep_tx_count, 0, sizeof(ep_tx_count));
		}
		break;
	case V003_GPIO_MODULE_ID:
		handle_gpio_out_request(urb->wIndex, urb->wValue);
		break;
	default:
		/* unsupported module request */
		break;
	}
}

static void usb_handle_control_in_request(struct usb_endpoint *e,
					  struct usb_urb *s)
{
	static u32 val = 0;

	switch (V003_CMD_GET_ID(s->wIndex)) {
	case V003_GENERIC_MODULE_ID:
		val = handle_generic_in_request(s->wIndex, s->wValue);
		break;
	case V003_GPIO_MODULE_ID:
		val = handle_gpio_in_request(s->wIndex, s->wValue);
		break;
	default:
		/* unsupported module request */
		break;
	}

	e->opaque = (u8 *)&val;
	e->max_len = s->wLength;
}

/* ------------------------------------------------------------------ */
/* endpoint data path                                                 */
/* ------------------------------------------------------------------ */

static u8 ep0_read_buffer[64];
static u8 ep1_read_buffer[64];
static u8 ep2_read_buffer[64];

/* buffer receiving control-OUT data stages, layout:
 * [0..3]   completion marker written by rv003usb (== ctrl_out_len)
 * [4..]    received data
 */
static u8 ctrl_out_buf[4 + 64];
static volatile u16 ctrl_out_len;
static struct usb_urb ctrl_out_urb;

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
	LogUEvent(0xeeee0000 | len, data[0], data[1], 0);
}

static void ep1_out_cb(u8 *data, int len)
{
	LogUEvent(0x11110000 | len, data[0], data[1], 0);
}

static void ep2_out_cb(u8 *data, int len)
{
	LogUEvent(0x22220000 | len, data[0], data[1], 0);
}

/* called from the main loop when a control-OUT data stage completed */
static void usb_handle_control_out_data(struct usb_urb *urb, u8 *data, int len)
{
	LogUEvent(0xcccc0000 | len, urb->wIndex, urb->wValue, 0);

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
		/* EP3 IN: demo payload */
		usb_send_data((uint8_t *)"Hello!~~", 8, 0, sendtok);
	} else {
		usb_send_empty(sendtok);
	}
}

void usb_handle_other_control_message(struct usb_endpoint *e, struct usb_urb *s,
				      struct rv003usb_internal *ist)
{
	LogUEvent(s->wRequestTypeLSBRequestMSB, s->wIndex, s->wValue,
		  s->wLength);

	/* request type is not vendor */
	if (!(s->wRequestTypeLSBRequestMSB & 0x40))
		return;

	if (s->wRequestTypeLSBRequestMSB & USB_CONTROL_IN_EP0) {
		usb_handle_control_in_request(e, s);
		return;
	}

	/* vendor OUT */
	if (s->wLength > 0) {
		/* control-OUT with a data stage: record it into ctrl_out_buf.
		 * rv003usb will write the packets into e->opaque and mark
		 * completion by storing the expected length at opaque[0]. */
		u16 len = s->wLength;

		if (len > sizeof(ctrl_out_buf) - 4)
			len = sizeof(ctrl_out_buf) - 4;

		ctrl_out_len = len;
		ctrl_out_urb = *s;
		e->opaque = (u8 *)ctrl_out_buf;
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

	funGpioInitAll();

	usb_setup();

	for (;;) {
		uint32_t *ue = GetUEvent();

		if (ue)
			printf("0x%lx 0x%lx 0x%lx 0x%lx\n", ue[0], ue[1], ue[2],
			       ue[3]);

		/* process a completed control-OUT data stage */
		if (ctrl_out_len &&
		    *(volatile u32 *)ctrl_out_buf == (u32)ctrl_out_len) {
			u16 len = ctrl_out_len;

			ctrl_out_len = 0;
			usb_handle_control_out_data(&ctrl_out_urb,
						    ctrl_out_buf + 4, len);
		}

		/* process queued vendor OUT requests */
		struct usb_urb *req = simple_usb_request_pop();

		if (req)
			usb_handle_control_out_request(req);
	}

	return 0;
}
