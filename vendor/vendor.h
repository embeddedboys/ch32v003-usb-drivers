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
