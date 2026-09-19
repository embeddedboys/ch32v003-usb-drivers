#ifndef __UART_H
#define __UART_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_UART_MODULE_ID 0x07
#define V003_UART_CMD(cmd)  V003_CMD(cmd, V003_UART_MODULE_ID)

/*
 * USART1 as a byte pipe.
 *
 * Pins: the default mapping is CK/PD4, TX/PD5, RX/PD6 (RM 7.3.2.1), and on this
 * board PD4 is the USB D- line, PD5 drives the USB pull-up and PD6 is the boot
 * button - using the default mapping would break the link this module is talked
 * to over.  The module therefore takes remap 01 (AFIO_PCFR1 bit 21 = 1, bit 2 =
 * 0), which puts TX on **PD0** and RX on **PD1**, both free here (PD2 is PWM
 * channel 1, PD3/PD4 are the USB pair).  Both pins are reported as reserved
 * through V003_GET_CAPABILITIES.
 *
 * RX is interrupt driven, TX is not:
 *
 *  - bytes arrive when they want to, so a 64 byte ring is filled by
 *    USART1_IRQHandler (which only moves the byte and counts what it could not
 *    keep) and drained by the main loop.  Polling in main() instead would lose
 *    bytes whenever main is busy, and it is busy for milliseconds at a time: a
 *    64 byte SPI transfer takes ~6 ms and a 64 byte I2C block read ~15 ms
 *    (notes/frame-protocol.md, notes/i2c.md).  At 115200 baud a byte arrives
 *    every 87 us, so the ring buys about 5.5 ms of cover - which is honest
 *    about what this module can do while another module is transferring.
 *  - a write is queued into a small transmit ring that the same interrupt
 *    drains (TXE), so no command handler blocks main() for the length of a
 *    frame: 64 bytes at 9600 baud is 66 ms, which is longer than anything else
 *    main does.  A host that queues faster than the line drains reads the
 *    backlog from V003_UART_GET_STATE and the losses from tx_dropped.
 *
 * Verification: a full duplex round trip through a **PD0 <-> PD1 jumper** is
 * what proves both directions, and it needs that jumper because PD1 is the
 * chip's SWIO debug pin - the programmer holds it, so the receive pin is not
 * usable without tying it to the transmit pin (notes/uart.md has the
 * measurements, including why HDSEL half duplex does not substitute for it).
 */

#define V003_UART_COUNT	 1

/* Ring sizes, both powers of two (the indices wrap with a mask).  Every byte
 * here is paid for out of the stack margin the build reports through
 * V003_GET_STACK_FREE.  The receive ring covers the longest stall main() can
 * have while another module transfers: a byte arrives every 87 us at 115200
 * baud, so 64 bytes is about 5.5 ms of cover. */
#ifndef V003_UART_RX_RING_SIZE
#define V003_UART_RX_RING_SIZE 64
#endif
#ifndef V003_UART_TX_RING_SIZE
#define V003_UART_TX_RING_SIZE 32
#endif

/* RM 12.3: baud = HCLK / (16 * USARTDIV) with USARTDIV = BRR/16, so the
 * register holds HCLK/baud directly and a 12 bit mantissa plus a 4 bit fraction
 * bound the range.  Values outside it are refused rather than rounded into
 * something that cannot work. */
#define V003_UART_BAUD_MIN 733	 /* BRR would not fit 16 bits below this */
#define V003_UART_BAUD_MAX 3000000 /* RM 12.1: 3 Mbps */

#define V003_UART_DATA_BITS_MIN 8
#define V003_UART_DATA_BITS_MAX 9
#define V003_UART_PARITY_NONE	0
#define V003_UART_PARITY_EVEN	1
#define V003_UART_PARITY_ODD	2
#define V003_UART_STOP_MIN	1
#define V003_UART_STOP_MAX	2

/* what a host sends to configure and what it reads back (actual_baud is only
 * filled in by the GET, and is what the prescaler really divides to - the read
 * back is the same idea as V003_PWM_GET) */
struct v003_uart_cfg {
	u32 baud;
	u8 data_bits;
	u8 parity;
	u8 stop_bits;
	u8 enable;
	u32 actual_baud;
};

/* OUT with a data stage (sizeof(struct v003_uart_cfg)): configure, and enable or
 * disable the port.  Refused as a whole - nothing is applied - if a field is out
 * of range. */
#define V003_UART_CONFIG V003_UART_CMD(0x90)
/* IN with a data stage: the configuration as the device holds it, actual_baud
 * included */
#define V003_UART_GET_CFG V003_UART_CMD(0x91)
/* OUT with a data stage: send these bytes (bounded, synchronous) */
#define V003_UART_WRITE V003_UART_CMD(0x92)
/* IN, wValue = maximum bytes: hand back what the receiver has.  The reply is
 * the contiguous run at the ring tail, so a host that gets a short answer asks
 * again; exactly what was published is consumed. */
#define V003_UART_READ V003_UART_CMD(0x93)
/* IN -> (tx_queued << 24) | (rx_available << 16) | (tx_dropped << 8) |
 * rx_dropped.  The four fields are 8 bit (the rings are smaller than 256), and
 * "enabled" comes from V003_UART_GET_CFG. */
#define V003_UART_GET_STATE V003_UART_CMD(0x94)
/* IN -> ((tx_bytes & 0xffff) << 16) | (rx_bytes & 0xffff) */
#define V003_UART_GET_COUNTS V003_UART_CMD(0x95)
/* IN -> (isr_entries << 24) | (overrun << 16) | (framing << 8) | parity: what
 * the receiver saw and what it rejected */
#define V003_UART_GET_ERRORS V003_UART_CMD(0x96)
/* OUT: drop whatever the receiver is holding.  A deliberate loss is still a
 * loss, so it is counted as dropped. */
#define V003_UART_FLUSH V003_UART_CMD(0x97)
/* OUT: zero the drop and error counters.  They are diagnostics a host measures
 * against (tx_dropped, rx_dropped, and the receiver's isr/overrun/framing/parity
 * counters), and an 8 bit counter that keeps its value across sessions makes a
 * difference measurement impossible once it has saturated.  The rings and the
 * byte totals are untouched. */
#define V003_UART_CLEAR_STATS V003_UART_CMD(0x9a)
/* IN -> (ring size << 16) | number of ports */
#define V003_UART_GET_INFO V003_UART_CMD(0x99)

extern void uart_handle_control_data(u16 cmd, const u8 *data, int len);
extern u32 handle_uart_in_request(u16 cmd, u16 data);
extern void uart_handle_out_request(u16 cmd, u16 data);
/* Bytes the port has moved in total, both directions.  The power module
 * snapshots this around a sleep: the receive interrupt clears RXNE before the
 * core can look at the USART after waking, and in sleep mode it is *any* enabled
 * interrupt that ends the sleep - including the transmitter draining its ring -
 * so "the UART was busy" is the honest thing to report, not "a byte arrived". */
extern u32 uart_activity_count(void);
extern u16 uart_rx_take(u16 want, u8 **out);
extern const struct v003_uart_cfg *uart_cfg_state(void);

#endif /* __UART_H */
