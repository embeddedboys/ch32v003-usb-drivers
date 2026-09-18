#ifndef __I2C_H
#define __I2C_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_I2C_MODULE_ID 0x03
#define V003_I2C_CMD(cmd)  V003_CMD(cmd, V003_I2C_MODULE_ID)

/* Bit banged I2C master pins (open drain, external pull-ups required). */
#define V003_I2C_SDA PC1
#define V003_I2C_SCL PC2

/* OUT, wValue = half period in units of 100 ns (0 keeps the current value, the
 * default is 12 = 1.2 us, which measures ~280 kHz on the wire because a bit
 * costs about three half periods): configures SDA/SCL as open drain outputs and
 * releases the bus.  Raise it (25 = ~140 kHz, 50 = ~70 kHz) if the pull-ups or
 * the wiring are poor. */
#define V003_I2C_CONFIG V003_I2C_CMD(0x50)

/* OUT with a data stage: [address byte][bytes...]; address byte already
 * contains the R/W bit (addr << 1).  Result in GET_STATUS. */
#define V003_I2C_WRITE V003_I2C_CMD(0x51)

/* OUT with a data stage: [address byte with R/W = 1][count]; the bytes read are
 * available through GET_RX. */
#define V003_I2C_READ V003_I2C_CMD(0x52)

/* IN -> half period in units of 100 ns */
#define V003_I2C_GET_CFG V003_I2C_CMD(0x59)

/* The clock tracer exists to debug the bus from the host; it costs 128 bytes of
 * RAM (64 edges) and is the first thing to drop when a build needs the room:
 * `make TRACE=0`.  Its commands disappear with it. */
#ifndef V003_I2C_TRACER
#define V003_I2C_TRACER 1
#endif

/* Clock tracer: OUT with wValue != 0 arms it (resets the buffer and starts
 * recording), 0 stops it.  It samples the SDA level at every rising SCL edge -
 * exactly what a slave latches - together with the time since the previous
 * edge, which is what makes a byte/clock boundary bug visible from the host. */
#define V003_I2C_TRACE V003_I2C_CMD(0x5a)

/* IN, wValue = byte offset into the trace, wLength = up to 64: the raw entries,
 * two bytes each: [ticks since the previous edge (unit: 8 ticks)][SDA level] */
#define V003_I2C_GET_TRACE V003_I2C_CMD(0x5b)

/* IN -> count | (overflow << 8) | (trace unit in ticks << 16) */
#define V003_I2C_GET_TRACE_INFO V003_I2C_CMD(0x5c)

/* OUT with a data stage [address byte with R/W = 0], wValue = timeout in ms
 * (0 = 100): poll the device address until it acknowledges instead of waiting a
 * fixed tWR.  Memory devices NACK everything while their internal write cycle
 * runs, so an ACK means the write finished - the AT24C256 datasheet calls this
 * ACK polling and it typically saves several milliseconds per page. */
#define V003_I2C_WAIT_READY V003_I2C_CMD(0x5d)

/* IN -> measured duration of the last WAIT_READY in microseconds */
#define V003_I2C_GET_WAIT_US V003_I2C_CMD(0x5e)

/* One request for the whole memory cycle: OUT with a data stage
 *   [address byte with R/W = 0][word address (2 bytes, or 1 with bit 8)][data...]
 * and wValue = (bytes to read back & 0xff)
 *            | (1 byte word address << 8)
 *            | (ACK poll before reading << 9)
 * The firmware writes the data, optionally ACK polls the device address until
 * its write cycle finished, then reads the given number of bytes back from the
 * same word address (Random Read) into GET_RX.
 *
 * This exists for speed: one control transfer costs ~3 ms on this link, and the
 * three separate requests it replaces need about fourteen of them. */
#define V003_I2C_MEM_WRITE_READ V003_I2C_CMD(0x5f)

/* IN -> status of the last transaction with a completion tag:
 *   (main loop request counter << 24) | status
 * The status bits alone are ambiguous for a host that has to poll: a control
 * transfer that carries a data stage is answered by the interrupt before the
 * main loop has executed it, so reading GET_STATUS right after sending a
 * request can return the *previous* request's status - and if two requests
 * happen to fail the same way, no amount of re-reading tells them apart.  The
 * counter (see v003_ctrl_out_seq()) changes for every request, so a host polls
 * this until the tag changes: one extra transfer to read a result it cannot
 * otherwise trust.  Use GET_STATUS when the host has its own way of knowing the
 * request was processed. */
#define V003_I2C_GET_RESULT V003_I2C_CMD(0x58)

/* IN -> bytes read by the last READ, clamped to its length */
#define V003_I2C_GET_RX V003_I2C_CMD(0x53)

/* IN -> status of the last transaction */
#define V003_I2C_GET_STATUS V003_I2C_CMD(0x54)

/* OUT, wValue = first 7 bit address: probes 32 addresses and stores the ACK
 * bitmap, read it with GET_SCAN.  Runs in main loop context. */
#define V003_I2C_SCAN V003_I2C_CMD(0x55)

/* IN -> bitmap of the last SCAN, bit 0 = the first probed address */
#define V003_I2C_GET_SCAN V003_I2C_CMD(0x56)

/* OUT with a data stage [address byte with R/W = 0][bytes to write...] and
 * wValue = number of bytes to read afterwards: START, address+W, the payload,
 * a repeated START (no STOP in between), address+R, then the read bytes, which
 * land in GET_RX.  This is the idiom register based devices (EEPROMs, most
 * sensors) need to read a register back. */
#define V003_I2C_WRITE_READ V003_I2C_CMD(0x57)

/* status bits */
#define V003_I2C_ST_OK		  (1u << 0)
#define V003_I2C_ST_ADDR_NACK	  (1u << 1)
#define V003_I2C_ST_DATA_NACK	  (1u << 2)
#define V003_I2C_ST_STRETCH	  (1u << 3) /* a slave held SCL low too long */
#define V003_I2C_ST_NOT_CONFIGURED (1u << 4)
/* the repeated START read phase was not acknowledged */
#define V003_I2C_ST_READ_NACK	  (1u << 5)
/* WAIT_READY gave up waiting for the device */
#define V003_I2C_ST_TIMEOUT	  (1u << 6)
/* status fields: builders for i2c.c, extractors for the host side */
#define V003_I2C_SET_BYTES(n)	     (((n) & 0xff) << 8)
#define V003_I2C_SET_FAILED_BYTE(n)  (((n) & 0xff) << 16)
#define V003_I2C_GET_BYTES(s)	     (((s) >> 8) & 0xff)
#define V003_I2C_GET_FAILED_BYTE(s)  (((s) >> 16) & 0xff)

/* buffer for the bytes read by V003_I2C_READ */
#define I2C_RX_BUF_SIZE 64

/* address used for the throwaway probe that absorbs the glitch of a pin mode
 * reconfiguration inside V003_I2C_CONFIG (0x08 is a reserved address and must
 * never be assigned to a real device) */
#define V003_I2C_PROBE_ADDR 0x08

extern int i2c_configured(void);
extern void handle_i2c_out_request(u16 cmd, u16 data);
extern u32 handle_i2c_in_request(u16 cmd, u16 data);
/* returns nonzero when the command carried an I2C payload; `val` is the
 * request's wValue (the read count for V003_I2C_WRITE_READ) */
extern int i2c_handle_control_data(u16 idx, u16 val, const u8 *data, int len);
extern const u8 *i2c_rx_data(void);
extern u16 i2c_rx_length(void);

/* clock trace accessors */
extern const u8 *i2c_trace_ptr(void);
extern u16 i2c_trace_bytes(void);

#endif /* __I2C_H */
