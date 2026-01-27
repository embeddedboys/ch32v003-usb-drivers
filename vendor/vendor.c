#include "ch32fun.h"
#include "rv003usb.h"
#include <stdio.h>
#include <string.h>

#include "vendor.h"

#define SIMPLE_USB_REQUEST_SIZE 8
#define SIMPLE_USB_REQUEST_MASK (SIMPLE_USB_REQUEST_SIZE - 1)
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

struct usb_urb *simple_usb_request_pop(void)
{
	int event = tail;

	/* empty */
	if (head == tail)
		return NULL;

	tail = SIMPLE_USB_REQUEST_NEXT(tail);
	return &simple_usb_requests[event];
}

void handle_gpio_out_request(u16 cmd, u16 data)
{
	u8 state = data & 0xff;
	u8 idx = data >> 8;

	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_GPIO_SET:
		funDigitalWrite(idx, state);
		break;
	default:
		break;
	}
}

u32 handle_gpio_in_request(u16 cmd, u16 data)
{
	u8 idx = data >> 8;
	u32 state = 0;

	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_GPIO_GET:
		state = funDigitalRead(idx);
		break;
	default:
		break;
	}

	return state;
}

void usb_handle_control_out_request(struct usb_urb *urb)
{
	LogUEvent(urb->wRequestTypeLSBRequestMSB, urb->wIndex, urb->wValue,
		  urb->wLength);

	switch (V003_CMD_GET_ID(urb->wIndex)) {
	case V003_GPIO_MODULE_ID:
		handle_gpio_out_request(urb->wIndex, urb->wValue);
		break;
	default:
		/* unsupported module request */
		break;
	}
}

void usb_handle_control_in_request(struct usb_endpoint *e, struct usb_urb *s)
{
	static u32 val = 0;

	switch (V003_CMD_GET_ID(s->wIndex)) {
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

int main()
{
	SystemInit();

	funGpioInitAll();
	funPinMode(PC0, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);

	usb_setup();

	for (;;) {
		uint32_t *ue = GetUEvent();

		if (ue)
			printf("0x%lx 0x%lx 0x%lx 0x%lx\n", ue[0], ue[1], ue[2],
			       ue[3]);

		// struct usb_urb *req = simple_usb_request_pop();

		// if (req)
		// 	usb_handle_control_out_request(req);
	}

	return 0;
}

void usb_handle_user_in_request(struct usb_endpoint *e, uint8_t *scratchpad,
				int endp, uint32_t sendtok,
				struct rv003usb_internal *ist)
{
	LogUEvent(SysTick->CNT, endp, sendtok, 0);

	if (endp == 3) {
		// usb_send_data( (uint8_t*)"Hello!~~", 8, 0, sendtok );
		usb_send_empty(sendtok);
	} else if (endp == 1) {
		usb_send_empty(sendtok);
	} else if (endp == 2) {
		usb_send_empty(sendtok);
	} else {
		// If it's a control transfer, don't send anything.
		usb_send_empty(sendtok);
	}
}

// uint8_t request;
// uint8_t ctrl_msg_len;
// uint8_t byte_left;
// uint8_t byte_pos;

void usb_handle_other_control_message(struct usb_endpoint *e, struct usb_urb *s,
				      struct rv003usb_internal *ist)
{
	// request = s->bRequest;

	LogUEvent(s->wRequestTypeLSBRequestMSB, s->wIndex, s->wValue,
		  s->wLength);
	// LogUEvent(SysTick->CNT, s->wRequestTypeLSBRequestMSB,
	// 	  s->lValueLSBIndexMSB, s->wLength);

	// ctrl_msg_len = s->wValue;
	// byte_left = s->wValue;

	/* request type is not vendor */
	if (!(s->wRequestTypeLSBRequestMSB & 0x40))
		return;

	if (s->wRequestTypeLSBRequestMSB & USB_CONTROL_IN_EP0)
		usb_handle_control_in_request(e, s);
	else
		usb_handle_control_out_request(s);
}

uint8_t ep0_read_buffer[64];
uint8_t ep1_read_buffer[64];

/* control transfer buffer handler */
static void usb_vendor_ep0_int_out(uint8_t *data, int len)
{
	// hexdump(data, len);
}

static void usb_vendor_ep1_int_out(uint8_t *data, int len)
{
	// hexdump(data, len);
}

struct usbd_ep_ctx eps_ctxs[] = {
	{
		.ep_addr = EP0_OUT_ADDR,
		.buf = ep0_read_buffer,
		.ep_cb = usb_vendor_ep0_int_out,
	},
	{
		.ep_addr = EP1_OUT_ADDR,
		.buf = ep1_read_buffer,
		.ep_cb = usb_vendor_ep1_int_out,
	},
};

/* will be called when more than 8 bytes on ep0 or any length of bytes on other endpoints */
void usb_handle_user_data(struct usb_endpoint *e, int current_endpoint,
			  uint8_t *data, int len, struct rv003usb_internal *ist)
{
	// struct usbd_ep_ctx *ctx;

	LogUEvent(SysTick->CNT, 0xffffffff, current_endpoint, len);

	// ctx = &eps_ctxs[current_endpoint];

	// memcpy(ctx->buf + byte_pos, data, len);
	// byte_pos += len;
	// byte_left -= len;

	// if (byte_left == 0) {
	// 	ctx->ep_cb(ctx->buf, byte_pos);
	// 	byte_pos = 0;
	// }
}
