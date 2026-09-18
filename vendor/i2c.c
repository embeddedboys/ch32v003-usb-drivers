#include "ch32fun.h"
#include "rv003usb.h"

#include "i2c.h"

/* Software I2C master on PC1 (SDA) / PC2 (SCL).
 *
 * Both pins are open drain outputs: writing 1 releases the line to the external
 * pull-up and the level is still readable through INDR.  A software master
 * keeps the CH32V003 I2C peripheral (and its errata) out of the picture, allows
 * clock stretching from the slave, and lets the bridge report exactly which
 * byte was not acknowledged.
 *
 * Timing notes (learned the hard way):
 *  - a "set the pin high and spin N times" loop is far too coarse here (one
 *    volatile loop iteration costs ~30 cycles), so the half period is
 *    calibrated in microseconds instead and can be set by the host,
 *  - open drain lines rise slowly through the pull-ups, so wherever the master
 *    is the only driver we wait for the line to actually be high (sda_high /
 *    scl_release).  Sampling a released SDA too early reads the previous (low)
 *    level, which showed up as a bus that always returned 0x0000 once the
 *    delay was made short.
 *
 * The transactions run from main() context (control-OUT data stage completion
 * and the queued zero length requests), never from the USB interrupt, so the
 * bit banging cannot disturb the software USB timing.
 */

/* Half period in units of 100 ns: 12 ~= 400 kHz, 25 ~= 200 kHz, 100 ~= 50 kHz.
 * The unit is fine enough for the fast modes the AT24C256 allows (400 kHz from
 * 1.7 V, 1 MHz from 2.5 V) while still being an integer. */
#define I2C_HALF_100NS_DEFAULT 12
/* how long to wait for a released line to rise (or a slave to let go of SCL) */
#define I2C_RISE_TIMEOUT 200000
/* ACK polling defaults */
#define I2C_WAIT_TIMEOUT_MS_DEFAULT 100
#define I2C_WAIT_MAX_POLLS 100000

static volatile u16 i2c_half_100ns = I2C_HALF_100NS_DEFAULT;
static volatile u8 i2c_on;
static volatile u32 i2c_status;
static volatile u32 i2c_scan_result;
static u8 i2c_rx_buf[I2C_RX_BUF_SIZE];
static volatile u16 i2c_rx_len;
static volatile u32 i2c_wait_us; /* duration of the last WAIT_READY */

/* ------------------------------------------------------------------ */
/* clock tracer                                                        */
/*                                                                    */
/* Records the SDA level at every rising SCL edge (what a slave latches) */
/* plus the time since the previous edge, so the host can decode the    */
/* byte stream the device actually sees and spot a wrong clock count at */
/* a byte boundary.  Sizes are deliberately tiny: 64 edges is enough to */
/* cover the address phase of one transaction.                          */
/* ------------------------------------------------------------------ */

#define I2C_TRACE_ENTRIES 64
/* the delta is stored in units of this many SysTick ticks (48 MHz core) */
#define I2C_TRACE_UNIT 64

static u8 i2c_trace[I2C_TRACE_ENTRIES * 2]; /* [dt][sda] per rising edge */
static volatile u8 i2c_trace_n;
static volatile u8 i2c_trace_on;
static volatile u8 i2c_trace_overflow;
static volatile u32 i2c_trace_last;

const u8 *i2c_trace_ptr(void)
{
	return i2c_trace;
}

u16 i2c_trace_bytes(void)
{
	return (u16)i2c_trace_n * 2;
}

static void i2c_trace_edge(int sda)
{
	u32 now, dt;

	if (!i2c_trace_on)
		return;

	now = SysTick->CNT;
	dt = (now - i2c_trace_last) / I2C_TRACE_UNIT;
	i2c_trace_last = now;

	if (i2c_trace_n >= I2C_TRACE_ENTRIES) {
		i2c_trace_overflow = 1;
		return;
	}

	i2c_trace[i2c_trace_n * 2] = dt > 255 ? 255 : (u8)dt;
	i2c_trace[i2c_trace_n * 2 + 1] = sda ? 1 : 0;
	i2c_trace_n++;
}

int i2c_configured(void)
{
	return i2c_on;
}

const u8 *i2c_rx_data(void)
{
	return i2c_rx_buf;
}

u16 i2c_rx_length(void)
{
	return i2c_rx_len;
}

/* Calibrated half period delay.
 *
 * Measured with the clock tracer: at -Os one iteration of this loop costs
 * about ten cycles on this core (the addi/bne pair plus the loop overhead), so
 * one microsecond is FUNCONF_SYSTEM_CORE_CLOCK / 10 iterations - the earlier
 * "two cycles" guess made every delay almost five times too long.  I2C has no
 * minimum clock frequency, only a maximum one. */
#define I2C_CYCLES_PER_ITER 10

static void i2c_delay(void)
{
	/* ns -> loop iterations, all in integers: x100 ns * 100 * (ticks per us)
	 * / (1000 ns per us * ticks per iteration) */
	u32 cycles = ((u32)i2c_half_100ns * 100 *
		      (FUNCONF_SYSTEM_CORE_CLOCK / 1000000)) /
		     (1000 * I2C_CYCLES_PER_ITER);

	if (!cycles)
		cycles = 1;

	__asm__ volatile("1:\n\t"
			 "addi %0, %0, -1\n\t"
			 "bne %0, x0, 1b\n\t"
			 : "+r"(cycles));
}

static void pin_low(u16 pin)
{
	funDigitalWrite(pin, 0);
}

static void pin_release(u16 pin)
{
	funDigitalWrite(pin, 1);
}

static void sda_low(void)
{
	pin_low(V003_I2C_SDA);
}

/* release SDA without waiting: valid while a slave may be holding it low (the
 * ACK slot and the data phase of a read) */
static void sda_release(void)
{
	pin_release(V003_I2C_SDA);
}

/* release SDA and wait for the pull-up to raise it; only used where the master
 * is the only driver (idle, start/stop and the bits we send) */
static void sda_high(void)
{
	u32 guard = I2C_RISE_TIMEOUT;

	pin_release(V003_I2C_SDA);
	while (!funDigitalRead(V003_I2C_SDA) && --guard)
		;

	if (!guard)
		i2c_status |= V003_I2C_ST_STRETCH;
}

static int sda_level(void)
{
	return funDigitalRead(V003_I2C_SDA);
}

static void scl_low(void)
{
	pin_low(V003_I2C_SCL);
}

/* release SCL and wait until the line is really high (slaves may stretch it) */
static void scl_release(void)
{
	u32 guard = I2C_RISE_TIMEOUT;
	int was_low = !funDigitalRead(V003_I2C_SCL);

	pin_release(V003_I2C_SCL);
	while (!funDigitalRead(V003_I2C_SCL) && --guard)
		;

	if (!guard)
		i2c_status |= V003_I2C_ST_STRETCH;

	/* Record only *real* rising edges: when SCL is already high (entering a
	 * START from an idle bus, or a repeated START) there is no edge, and
	 * logging one would shift every decoded byte by one. */
	if (was_low)
		i2c_trace_edge(funDigitalRead(V003_I2C_SDA));
}

static void i2c_start(void)
{
	sda_high();
	scl_release();
	i2c_delay();
	sda_low();
	i2c_delay();
	scl_low();
	i2c_delay();
}

static void i2c_start(void);
static void i2c_stop(void);
static int i2c_write_byte(u8 b);

/* If a slave is still holding SDA low the bus is in the middle of a byte (an
 * aborted transfer, a glitch while the pins were reconfigured, ...).  In that
 * state a START is easy to misread, which showed up as an occasional NACK of
 * the first byte of a transaction.  Clock the slave out and issue a STOP so
 * every transaction starts from an idle bus. */
static void i2c_bus_reset(void)
{
	int i;

	/* clock the bus until the slave lets go of SDA (at most one byte), then
	 * issue a STOP so it ends up in a defined state */
	for (i = 0; i < 10 && !funDigitalRead(V003_I2C_SDA); i++) {
		scl_low();
		i2c_delay();
		scl_release();
		i2c_delay();
	}

	i2c_stop();
}

/* cheap check: only run the (slow) reset when the bus is not idle */
static void i2c_recover(void)
{
	if (!funDigitalRead(V003_I2C_SDA) || !funDigitalRead(V003_I2C_SCL))
		i2c_bus_reset();
}

/* Reconfiguring the pins glitches the bus and made the *first* transaction
 * afterwards misread its ACK slot (~1 in 200, at any clock speed, all later
 * transactions were clean).  Reset the bus and burn one throwaway probe on a
 * dead address so the host never sees that first glitch.  Called after every
 * pin (re)configuration, i.e. CONFIG and SET_PULLUP. */
static void i2c_bus_prepare(void)
{
	i2c_bus_reset();
	i2c_start();
	i2c_write_byte((u8)(V003_I2C_PROBE_ADDR << 1));
	i2c_stop();
	i2c_status = 0;
}

static void i2c_stop(void)
{
	sda_low();
	i2c_delay();
	scl_release();
	i2c_delay();
	sda_high();
	i2c_delay();
}

static int i2c_write_byte(u8 b)
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		if (b & (1 << i))
			sda_high();
		else
			sda_low();
		i2c_delay();
		scl_release();
		i2c_delay();
		scl_low();
		i2c_delay();
	}

	/* ACK: the slave pulls SDA low while we clock the ninth bit */
	sda_release();
	i2c_delay();
	scl_release();
	i2c_delay();
	ack = !sda_level();
	scl_low();
	i2c_delay();

	return ack;
}

static u8 i2c_read_byte(int ack)
{
	u8 b = 0;
	int i;

	sda_release();
	for (i = 7; i >= 0; i--) {
		i2c_delay();
		scl_release();
		/* sample late in the high phase: a slave releasing SDA for a 1
		 * bit needs the pull-up to raise the line first */
		i2c_delay();
		if (sda_level())
			b |= (1 << i);
		scl_low();
		i2c_delay();
	}

	/* acknowledge all but the last byte */
	if (ack)
		sda_low();
	else
		sda_release();
	i2c_delay();
	scl_release();
	i2c_delay();
	scl_low();
	i2c_delay();
	sda_release();

	return b;
}

static void i2c_do_write(const u8 *data, int len)
{
	int i, written = 0;

	if (len <= 0)
		return;

	i2c_recover();
	i2c_start();

	if (!i2c_write_byte(data[0])) {
		i2c_status = V003_I2C_ST_ADDR_NACK;
		i2c_stop();
		return;
	}

	for (i = 1; i < len; i++) {
		if (!i2c_write_byte(data[i])) {
			i2c_status = V003_I2C_ST_DATA_NACK |
				     V003_I2C_SET_FAILED_BYTE(i - 1);
			i2c_stop();
			return;
		}
		written++;
	}

	i2c_stop();
	i2c_status = V003_I2C_ST_OK | V003_I2C_SET_BYTES(written);
}

static void i2c_do_read(u8 addr_byte, int count)
{
	int i;

	i2c_rx_len = 0;

	if (count <= 0)
		return;

	if (count > (int)sizeof(i2c_rx_buf))
		count = sizeof(i2c_rx_buf);

	i2c_recover();
	i2c_start();

	if (!i2c_write_byte(addr_byte)) {
		i2c_status = V003_I2C_ST_ADDR_NACK;
		i2c_stop();
		return;
	}

	for (i = 0; i < count; i++)
		i2c_rx_buf[i] = i2c_read_byte(i < count - 1);

	i2c_stop();
	i2c_rx_len = count;
	i2c_status = V003_I2C_ST_OK | V003_I2C_SET_BYTES(count);
}

/* write phase, repeated START, read phase - no STOP between the two */
static void i2c_do_write_read(const u8 *data, int len, int read_count)
{
	int i, written = 0;

	i2c_rx_len = 0;

	if (len < 1)
		return;

	if (read_count > (int)sizeof(i2c_rx_buf))
		read_count = sizeof(i2c_rx_buf);

	if (read_count < 0)
		read_count = 0;

	i2c_recover();
	i2c_start();

	if (!i2c_write_byte(data[0])) {
		i2c_status = V003_I2C_ST_ADDR_NACK;
		i2c_stop();
		return;
	}

	for (i = 1; i < len; i++) {
		if (!i2c_write_byte(data[i])) {
			i2c_status = V003_I2C_ST_DATA_NACK |
				     V003_I2C_SET_FAILED_BYTE(i - 1);
			i2c_stop();
			return;
		}
		written++;
	}

	/* repeated START, same address, read direction */
	i2c_start();

	if (!i2c_write_byte(data[0] | 0x01)) {
		i2c_status = V003_I2C_ST_READ_NACK;
		i2c_stop();
		return;
	}

	for (i = 0; i < read_count; i++)
		i2c_rx_buf[i] = i2c_read_byte(i < read_count - 1);

	i2c_stop();
	i2c_rx_len = read_count;
	i2c_status = V003_I2C_ST_OK | V003_I2C_SET_BYTES(written + read_count);
}

/* Poll the device address until it acknowledges (AT24C256 "ACK polling").
 * While the internal write cycle runs the device answers nothing, so an ACK
 * means the write is committed.  Records the duration in i2c_wait_us and
 * returns 1 on success, 0 on timeout. */
static int i2c_poll_ready(u8 addr_byte, u16 timeout_ms)
{
	u32 start, elapsed_us, limit_us, polls = 0;
	int acked = 0;

	if (timeout_ms == 0)
		timeout_ms = I2C_WAIT_TIMEOUT_MS_DEFAULT;

	limit_us = (u32)timeout_ms * 1000;
	start = SysTick->CNT;

	for (;;) {
		i2c_start();
		acked = i2c_write_byte(addr_byte);
		i2c_stop();

		elapsed_us = (SysTick->CNT - start) /
			     (FUNCONF_SYSTEM_CORE_CLOCK / 1000000);

		if (acked || elapsed_us >= limit_us ||
		    ++polls >= I2C_WAIT_MAX_POLLS)
			break;
	}

	i2c_wait_us = elapsed_us;
	return acked;
}

static void i2c_do_wait_ready(u8 addr_byte, u16 timeout_ms)
{
	if (i2c_poll_ready(addr_byte, timeout_ms))
		i2c_status = V003_I2C_ST_OK | V003_I2C_SET_BYTES(1);
	else
		i2c_status = V003_I2C_ST_TIMEOUT;
}

/* The whole memory cycle in one request: page write, optional ACK poll, then a
 * random read of the same word address.  Saves the host a dozen control
 * transfers. */
static void i2c_do_mem_write_read(const u8 *data, int len, u16 opts)
{
	int addr_len = (opts & 0x100) ? 1 : 2;
	int read_count = opts & 0xff;
	int poll = opts & 0x200;
	u8 dev = data[0] & 0xfe; /* force the write direction for the ACK poll */

	if (len < 1 + addr_len)
		return;

	/* 1. page write: device address + word address + data */
	i2c_do_write(data, len);
	if (!(i2c_status & V003_I2C_ST_OK))
		return;

	/* 2. wait for the internal write cycle (the device NACKs until done) */
	if (poll && !i2c_poll_ready(dev, I2C_WAIT_TIMEOUT_MS_DEFAULT)) {
		i2c_status = V003_I2C_ST_TIMEOUT;
		return;
	}

	/* 3. read the data back from the same word address */
	if (read_count > 0)
		i2c_do_write_read(data, 1 + addr_len, read_count);
}

static void i2c_do_scan(u16 base)
{
	u32 found = 0;
	int i;

	if (base > 127)
		base = 0;

	i2c_recover();

	for (i = 0; i < 32 && (base + i) < 128; i++) {
		u8 addr = (u8)(base + i);

		/* the reserved ranges never answer, skip them to save time */
		if (addr < 0x08 || addr > 0x77)
			continue;

		i2c_start();
		if (i2c_write_byte((u8)(addr << 1)))
			found |= (1u << i);
		i2c_stop();
	}

	i2c_scan_result = found;
}

void handle_i2c_out_request(u16 cmd, u16 data)
{
	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_I2C_CONFIG:
		if (data)
			i2c_half_100ns = data;

		funPinMode(V003_I2C_SCL, GPIO_Speed_10MHz | GPIO_CNF_OUT_OD);
		funPinMode(V003_I2C_SDA, GPIO_Speed_10MHz | GPIO_CNF_OUT_OD);
		i2c_status = 0;
		i2c_on = 1;
		sda_high();
		scl_release();
		i2c_bus_prepare();
		break;
	case V003_I2C_TRACE:
		if (data) {
			i2c_trace_n = 0;
			i2c_trace_overflow = 0;
			i2c_trace_last = SysTick->CNT;
			i2c_trace_on = 1;
		} else {
			i2c_trace_on = 0;
		}
		break;
	case V003_I2C_SCAN:
		if (!i2c_on) {
			i2c_status = V003_I2C_ST_NOT_CONFIGURED;
			break;
		}
		i2c_do_scan(data);
		break;
	default:
		/* unsupported i2c request */
		break;
	}
}

int i2c_handle_control_data(u16 idx, u16 val, const u8 *data, int len)
{
	if (!i2c_on) {
		i2c_status = V003_I2C_ST_NOT_CONFIGURED;
		return 0;
	}

	if (idx == V003_I2C_WRITE) {
		i2c_do_write(data, len);
		return 1;
	}

	if (idx == V003_I2C_READ && len >= 1) {
		i2c_do_read(data[0], len > 1 ? data[1] : 1);
		return 1;
	}

	if (idx == V003_I2C_WRITE_READ) {
		i2c_do_write_read(data, len, (int)(val & 0xff));
		return 1;
	}

	if (idx == V003_I2C_WAIT_READY && len >= 1) {
		i2c_do_wait_ready(data[0], val);
		return 1;
	}

	if (idx == V003_I2C_MEM_WRITE_READ && len >= 2) {
		i2c_do_mem_write_read(data, len, val);
		return 1;
	}

	return 0;
}

u32 handle_i2c_in_request(u16 cmd, u16 data)
{
	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_I2C_GET_STATUS:
		return i2c_status;
	case V003_I2C_GET_RESULT:
		/* status plus the main loop's request counter, so the host can
		 * tell a fresh result from one it already saw */
		return (v003_ctrl_out_seq() << 24) | (i2c_status & 0x00ffffff);
	case V003_I2C_GET_SCAN:
		return i2c_scan_result;
	case V003_I2C_GET_CFG:
		return i2c_half_100ns;
	case V003_I2C_GET_WAIT_US:
		return i2c_wait_us;
	case V003_I2C_GET_TRACE_INFO:
		return (u32)i2c_trace_n | ((u32)i2c_trace_overflow << 8) |
		       ((u32)I2C_TRACE_UNIT << 16);
	default:
		return 0;
	}
}
