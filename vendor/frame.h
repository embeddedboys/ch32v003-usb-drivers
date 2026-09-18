#ifndef __FRAME_H
#define __FRAME_H

#include "ch32fun.h"
#include "vendor.h"

/*
 * Framed protocol on the endpoint data path, following the Diolan DLN-2
 * (drivers/mfd/dln2.c).
 *
 *   request   [size u16][id u16][echo u16][handle u16][payload...]
 *   response  [size u16][id u16][echo u16][handle u16][result u16][payload...]
 *
 * size   total frame length including the header
 * id     command byte (the module is in `handle`; a control-path encoded
 *        cmd|module<<8 is accepted too, only the low byte is used)
 * echo   host tag, copied into the response; 0 means "no response wanted"
 * handle module id (see V003_*_MODULE_ID)
 * result 0 on success, non zero on error
 *
 * The request payload carries the command argument.  For the commands that
 * exist on both paths that is the 16 bit wValue of the equivalent control
 * request, so a host driver can switch transports without changing the
 * per-command encoding.  Response payloads are command specific.
 *
 * Frames arrive on EP2 OUT and are answered on EP3 IN, so a host driver can
 * move payloads and keep several requests in flight instead of paying for a
 * control transfer per operation.
 */

#define V003_FRAME_HDR	8
#define V003_FRAME_MAX	72 /* header + payload, same budget as a control OUT */
/* depth of the request queue the interrupt fills and main() drains */
#define V003_FRAME_RX_SLOTS 2

#define V003_FRAME_OK	 0
#define V003_FRAME_EBADCMD 1
#define V003_FRAME_ETOOBIG 2

/* control request: OUT V003_SET_FRAME_MODE wValue 1 enables the framed path */
#define V003_SET_FRAME_MODE V003_GENERIC_CMD(0x39)
/* IN -> (frames handled << 16) | frames dropped */
#define V003_GET_FRAME_STATS V003_GENERIC_CMD(0x3a)

/* called from the EP2 OUT callback (interrupt context) */
void v003_frame_feed(const u8 *data, int len);
/* called from main(): handle one queued request, if any.  Responses are queued
 * on the shared EP3 IN FIFO through v003_ep3_in_push(). */
void v003_frame_poll(void);
void v003_frame_enable(int on);
u32 v003_frame_stats(void);
/* 1 while the host asked for the framed data path (EP1 echo and the raw EP3
 * FIFO are then idle, so the two streams cannot be mixed up) */
int v003_frame_active(void);

#endif /* __FRAME_H */
