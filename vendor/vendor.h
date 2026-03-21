#ifndef __VENDOR_H
#define __VENDOR_H

#include "ch32fun.h"

#define __maybe_unused __attribute__((unused))

#define EP0_IN_ADDR  (USB_EP_DIR_IN | 0)
#define EP0_OUT_ADDR (USB_EP_DIR_OUT | 0)
#define EP1_OUT_ADDR (USB_EP_DIR_OUT | 1)
#define EP2_IN_ADDR  (USB_EP_DIR_IN | 2)
#define EP3_OUT_ADDR (USB_EP_DIR_OUT | 3)
#define EP4_IN_ADDR  (USB_EP_DIR_IN | 4)

#define REQ_EP1_OUT 0x02
#define REQ_EP2_IN  0x03

#define V003_CMD(cmd, id)     ((cmd) | (id << 8))
#define V003_CMD_GET_ID(cmd)  (cmd >> 8)
#define V003_CMD_GET_CMD(cmd) (cmd & 0xFF)

struct usb_ctrl_msg_ctx {
	u8 msg_len; /* current ctrl msg length */

	u8 byte_left;
	u8 byte_pos;
};

struct usbd_ep_ctx {
	u8 ep_addr;
	u8 *buf;
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

#endif /* __VENDOR_H */
