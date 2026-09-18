#include "ch32fun.h"
#include "rv003usb.h"

#include "frame.h"
#include "gpio.h"

/* ------------------------------------------------------------------ */
/* receive side: the interrupt accumulates bytes, main() handles them  */
/* ------------------------------------------------------------------ */

struct frame_rx_slot {
	u16 len;   /* bytes accumulated */
	u16 size;  /* total length once the header has arrived */
	u8 data[V003_FRAME_MAX];
};

static struct frame_rx_slot frame_rx[V003_FRAME_RX_SLOTS];
static volatile u8 frame_rx_head, frame_rx_tail;
static volatile u8 frame_rx_full; /* the queue is full: drop instead of mixing */
static volatile u8 frame_rx_resync; /* ignore the rest of this packet */
static volatile u32 frame_rx_dropped;
static volatile u32 frame_handled;

/* Responses are queued on the EP3 IN byte FIFO (v003_ep3_in_push), which the
 * interrupt drains - the raw echo/SPI stream uses the same FIFO, and frame
 * mode is mutually exclusive with it. */

static volatile u8 frame_on;

static u16 frame_get16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static void frame_put16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xff);
	p[1] = (u8)(v >> 8);
}

void v003_frame_enable(int on)
{
	u8 i;

	frame_on = on ? 1 : 0;

	/* Both the accumulating slots and the EP3 IN FIFO can hold leftovers
	 * from the previous mode.  Without clearing them the first request
	 * after re-enabling gets appended to a stale partial frame (so its
	 * header is parsed at the wrong offset and the frame is dropped) and
	 * the first read can return bytes of the old echo stream. */
	for (i = 0; i < V003_FRAME_RX_SLOTS; i++) {
		frame_rx[i].len = 0;
		frame_rx[i].size = 0;
	}

	v003_ep3_in_reset();

	frame_rx_head = frame_rx_tail = 0;
	frame_rx_full = 0;
	frame_rx_resync = 0;
	frame_rx_dropped = 0;
	frame_handled = 0;
}

u32 v003_frame_stats(void)
{
	return (frame_handled << 16) | (frame_rx_dropped & 0xffff);
}

int v003_frame_active(void)
{
	return frame_on;
}

/* Interrupt context: feed the bytes of one EP2 OUT packet. */
void v003_frame_feed(const u8 *data, int len)
{
	struct frame_rx_slot *slot;
	int i;

	if (!frame_on || len <= 0)
		return;

	slot = &frame_rx[frame_rx_head];
	/* a bad length desynchronises the byte stream, but the packet boundary
	 * is a known good place to start over */
	frame_rx_resync = 0;

	for (i = 0; i < len; i++) {
		if (frame_rx_full || frame_rx_resync) {
			frame_rx_dropped++;
			continue;
		}

		if (slot->len < V003_FRAME_MAX)
			slot->data[slot->len] = data[i];
		slot->len++;

		if (slot->len == V003_FRAME_HDR) {
			slot->size = frame_get16(slot->data);
			if (slot->size < V003_FRAME_HDR ||
			    slot->size > V003_FRAME_MAX) {
				/* desynced: drop the rest of this packet */
				frame_rx_dropped++;
				slot->len = 0;
				slot->size = 0;
				frame_rx_resync = 1;
				continue;
			}
		}

		if (slot->size && slot->len >= slot->size) {
			u8 next = (u8)((frame_rx_head + 1) %
				       V003_FRAME_RX_SLOTS);

			if (next == frame_rx_tail) {
				/* main is behind: keep this frame until it is
				 * consumed, drop anything after it */
				frame_rx_full = 1;
				frame_rx_dropped++;
				continue;
			}

			frame_rx_head = next;
			slot = &frame_rx[frame_rx_head];
			slot->len = 0;
			slot->size = 0;
		}
	}
}

/* main() context: answer one request */
static void frame_respond(const u8 *req, u16 result, const u8 *payload,
			  int payload_len)
{
	u8 out[V003_FRAME_HDR + 2 + V003_FRAME_MAX];
	u16 echo = frame_get16(req + 4);

	if (!echo)
		return; /* fire and forget */

	if (payload_len > V003_FRAME_MAX - V003_FRAME_HDR - 2)
		payload_len = V003_FRAME_MAX - V003_FRAME_HDR - 2;

	frame_put16(out, (u16)(V003_FRAME_HDR + 2 + payload_len));
	out[2] = req[2];
	out[3] = req[3];
	out[4] = req[4];
	out[5] = req[5];
	out[6] = req[6];
	out[7] = req[7];
	frame_put16(out + 8, result);

	if (payload_len > 0)
		memcpy(out + V003_FRAME_HDR + 2, payload, payload_len);

	v003_ep3_in_push(out, V003_FRAME_HDR + 2 + payload_len);
}

/* main() context: run the command carried by a frame */
static u16 frame_execute(const u8 *req, int len, u8 *payload, int *payload_len)
{
	/* The module always comes from `handle`, so the id is used as a plain
	 * command byte.  The high byte is masked off rather than rejected so a
	 * host can carry its control-path encoding (wIndex = cmd|module<<8)
	 * straight over to the framed path. */
	u16 id = V003_CMD_GET_CMD(frame_get16(req + 2));
	u16 handle = frame_get16(req + 6);
	u16 arg = 0;
	u32 val;

	if (len >= V003_FRAME_HDR + 2)
		arg = frame_get16(req + V003_FRAME_HDR);

	*payload_len = 0;
	val = 0;

	switch (handle) {
	case V003_GPIO_MODULE_ID:
		switch (id) {
		case 0x06: /* SET */
		case 0x08: /* REQUEST */
		case 0x09: /* FREE */
		case 0x0b: /* DIRECTION_INPUT */
		case 0x0c: /* DIRECTION_OUTPUT */
			handle_gpio_out_request(V003_GPIO_CMD(id), arg);
			return V003_FRAME_OK;
		case 0x07: /* GET */
		case 0x0a: /* GET_DIRECTION */
			val = handle_gpio_in_request(V003_GPIO_CMD(id), arg);
			frame_put16(payload, (u16)(val & 0xffff));
			frame_put16(payload + 2, (u16)(val >> 16));
			*payload_len = 4;
			return V003_FRAME_OK;
		default:
			return V003_FRAME_EBADCMD;
		}
	case V003_GENERIC_MODULE_ID:
		/* some generic commands are actions that answer with nothing
		 * (e.g. enabling frame mode), the rest are queries */
		if (v003_generic_out_request(V003_GENERIC_CMD(id), arg))
			return V003_FRAME_OK;

		val = v003_generic_in_request(V003_GENERIC_CMD(id), arg);
		frame_put16(payload, (u16)(val & 0xffff));
		frame_put16(payload + 2, (u16)(val >> 16));
		*payload_len = 4;
		return V003_FRAME_OK;
	default:
		return V003_FRAME_EBADCMD;
	}
}

/* main() context: handle queued requests */
void v003_frame_poll(void)
{
	u8 payload[8];
	int payload_len = 0;
	u16 result;

	while (frame_rx_tail != frame_rx_head) {
		struct frame_rx_slot *slot = &frame_rx[frame_rx_tail];

		if (slot->len < V003_FRAME_HDR || slot->len < slot->size)
			break; /* not complete yet */

		result = frame_execute(slot->data, slot->len, payload,
				       &payload_len);
		frame_respond(slot->data, result, payload, payload_len);

		slot->len = 0;
		slot->size = 0;
		frame_rx_tail = (u8)((frame_rx_tail + 1) % V003_FRAME_RX_SLOTS);
		frame_rx_full = 0;
		frame_handled++;
	}
}
