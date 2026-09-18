#include "ch32fun.h"
#include "rv003usb.h"

#include "uart.h"

/*
 * USART1, remapped to PD0 (TX) and PD1 (RX); uart.h says why the default
 * mapping cannot be used on this board.
 *
 * Two contexts, like everywhere else in this firmware: the interrupt moves
 * received bytes into one ring and takes the next byte out of the other, and
 * does nothing else.  The command handlers (main loop) configure, queue and
 * drain.  Both rings are single producer / single consumer, so the indices need
 * no locking: the receive ring is written by the interrupt and read by main,
 * the transmit ring the other way round.
 *
 * The transmitter is interrupt driven rather than synchronous because main()
 * owes the host its control requests: sending 64 bytes at 9600 baud takes 66 ms,
 * an order of magnitude more than the slowest thing main does today (a 64 byte
 * I2C block read takes 15 ms, a 64 byte SPI transfer 6 ms).  Queueing costs
 * V003_UART_TX_RING_SIZE bytes of RAM and keeps main free; whatever does not fit
 * is counted as dropped.
 */

#define RX_MASK (V003_UART_RX_RING_SIZE - 1)
#define TX_MASK (V003_UART_TX_RING_SIZE - 1)

static u8 rx_ring[V003_UART_RX_RING_SIZE];
static volatile u8 rx_head; /* interrupt writes */
static volatile u8 rx_tail; /* main writes */

static u8 tx_ring[V003_UART_TX_RING_SIZE];
static volatile u8 tx_head; /* main writes */
static volatile u8 tx_tail; /* interrupt writes */

static u32 tx_bytes, rx_bytes;
/* 8 bit saturating counters: a host reads them through GET_STATE and
 * GET_ERRORS, and a counter that wrapped silently would be worse than one that
 * sticks at its maximum.  V003_UART_FLUSH clears the error ones. */
static u8 tx_dropped, rx_dropped;
static u8 rx_overrun, rx_framing, rx_parity;
/* how often the receiver's interrupt ran at all: a receiver that looks dead is
 * either not being interrupted or not being fed, and those need different
 * answers */
static u8 rx_isrs;

static volatile u8 uart_on;
static struct v003_uart_cfg uart_cfg;

/* Set once, so the interrupt stays enabled for the lifetime of the firmware and
 * is masked through CTLR1 instead of being enabled around every
 * reconfiguration. */
static u8 uart_irq_ready;

static void u8_sat(u8 *counter)
{
	if (*counter != 0xff)
		(*counter)++;
}

static void rx_reset(void)
{
	rx_head = 0;
	rx_tail = 0;
}

static void tx_reset(void)
{
	tx_head = 0;
	tx_tail = 0;
}

/* ------------------------------------------------------------ interrupt */

void USART1_IRQHandler(void) __attribute__((interrupt));
void USART1_IRQHandler(void)
{
	u16 stat = (u16)USART1->STATR;

	if (stat & USART_STATR_RXNE) {
		/* reading DATAR clears RXNE, and the error flags with it */
		u8 byte = (u8)USART1->DATAR;
		u8 next;

		u8_sat(&rx_isrs);

		if (stat & (USART_STATR_ORE | USART_STATR_FE | USART_STATR_NE |
			    USART_STATR_PE)) {
			/* the byte is not trustworthy: count which error and
			 * drop it instead of putting it into the stream */
			if (stat & USART_STATR_ORE)
				u8_sat(&rx_overrun);
			if (stat & USART_STATR_FE)
				u8_sat(&rx_framing);
			if (stat & USART_STATR_PE)
				u8_sat(&rx_parity);
		} else {
			next = (u8)((rx_head + 1) & RX_MASK);
			if (next == rx_tail) {
				/* full: drop the new byte and never move the
				 * reader's index */
				u8_sat(&rx_dropped);
			} else {
				rx_ring[rx_head] = byte;
				rx_head = next;
				rx_bytes++;
			}
		}
	}

	if (stat & USART_STATR_TXE) {
		if (tx_tail == tx_head) {
			/* nothing queued: stop asking to be told.  The test is
			 * tail == head and not "next(head) == tail": the latter
			 * is the *full* condition, and using it here made an
			 * empty ring look as if it held a byte, so the interrupt
			 * pushed a stale entry and advanced the tail - which is
			 * how a byte disappears between the queue and the line. */
			USART1->CTLR1 &= ~USART_CTLR1_TXEIE;
		} else {
			USART1->DATAR = tx_ring[tx_tail];
			tx_tail = (u8)((tx_tail + 1) & TX_MASK);
		}
	}
}

/* ------------------------------------------------------------------ send */

/* Queue as much of the payload as the ring holds; the interrupt clocks it out.
 * Returns how many bytes were queued. */
static u16 uart_queue(const u8 *data, int len)
{
	u16 queued = 0;
	u8 next;

	while (len > 0) {
		next = (u8)((tx_head + 1) & TX_MASK);
		if (next == tx_tail) {
			/* ring full: drop the rest and count it */
			while (len-- > 0)
				u8_sat(&tx_dropped);
			break;
		}

		tx_ring[tx_head] = *data++;
		tx_head = next;
		queued++;
		len--;
	}

	tx_bytes += queued;
	if (queued)
		USART1->CTLR1 |= USART_CTLR1_TXEIE;

	return queued;
}

/* ------------------------------------------------------------------ config */

const struct v003_uart_cfg *uart_cfg_state(void)
{
	return &uart_cfg;
}

static void uart_hw_setup(void)
{
	RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOD |
			  RCC_APB2Periph_USART1;

	/* Remap 01: TX on PD0, RX on PD1 (RM 7.3.2.1).  The field is two bits in
	 * one register, written as [bit21, bit2] - the high bit first - so 01
	 * means bit2 set and bit21 clear.  Getting that backwards selects remap
	 * 10 (TX/PD6, RX/PD5, the boot button and the USB pull-up) and leaves
	 * PD0 as an undriven pad, which looks exactly like "the UART sends
	 * nothing". */
	AFIO->PCFR1 |= AFIO_PCFR1_USART1_REMAP;
	AFIO->PCFR1 &= ~AFIO_PCFR1_USART1_REMAP_1;

	funPinMode(V003_UART_TX_PIN, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF);
	funPinMode(V003_UART_RX_PIN, GPIO_CNF_IN_FLOATING);

	if (!uart_irq_ready) {
		NVIC_EnableIRQ(USART1_IRQn);
		uart_irq_ready = 1;
	}
}

static u8 uart_apply(const struct v003_uart_cfg *cfg)
{
	u32 brr, ctlr1 = 0;

	if (cfg->baud < V003_UART_BAUD_MIN || cfg->baud > V003_UART_BAUD_MAX)
		return 0;
	if (cfg->data_bits < V003_UART_DATA_BITS_MIN ||
	    cfg->data_bits > V003_UART_DATA_BITS_MAX)
		return 0;
	if (cfg->parity > V003_UART_PARITY_ODD)
		return 0;
	if (cfg->stop_bits < V003_UART_STOP_MIN ||
	    cfg->stop_bits > V003_UART_STOP_MAX)
		return 0;

	/* baud = HCLK / BRR exactly, so rounding BRR is the whole job */
	brr = (FUNCONF_SYSTEM_CORE_CLOCK + cfg->baud / 2) / cfg->baud;
	if (brr < 16)
		brr = 16;
	if (brr > 0xffff)
		brr = 0xffff;

	uart_hw_setup();

	/* reconfigure with the port off, then drop whatever the old setting had
	 * already put in the rings: it belongs to the previous session */
	USART1->CTLR1 = 0;
	USART1->CTLR2 = 0;
	USART1->CTLR3 = 0;
	rx_reset();
	tx_reset();

	if (!cfg->enable) {
		/* A disabled port lets go of its pins instead of leaving the
		 * transmitter driving an idle high line: PD1 is the chip's SWIO
		 * debug pin, and on this bench it is jumpered to PD0 for the
		 * loopback test, so a firmware that keeps PD0 driven also keeps
		 * the programmer from talking to the chip (notes/uart.md). */
		funPinMode(V003_UART_TX_PIN, GPIO_CNF_IN_FLOATING);
		funPinMode(V003_UART_RX_PIN, GPIO_CNF_IN_FLOATING);

		uart_cfg = *cfg;
		uart_cfg.actual_baud = 0;
		uart_on = 0;
		return 1;
	}

	USART1->BRR = brr & 0xffff;

	if (cfg->data_bits == 9)
		ctlr1 |= USART_CTLR1_M;
	if (cfg->parity != V003_UART_PARITY_NONE) {
		ctlr1 |= USART_CTLR1_PCE;
		if (cfg->parity == V003_UART_PARITY_ODD)
			ctlr1 |= USART_CTLR1_PS;
	}
	if (cfg->stop_bits == 2)
		USART1->CTLR2 = USART_CTLR2_STOP_1;

	ctlr1 |= USART_CTLR1_TE | USART_CTLR1_RE | USART_CTLR1_RXNEIE |
		 USART_CTLR1_UE;

	uart_cfg = *cfg;
	uart_cfg.actual_baud = FUNCONF_SYSTEM_CORE_CLOCK / (brr & 0xffff);
	USART1->CTLR1 = (u16)ctlr1;
	uart_on = 1;

	/* clear the flags the enable just raised, so the first interrupt is a
	 * received byte and not a stale error */
	(void)USART1->STATR;
	(void)USART1->DATAR;

	return 1;
}

/* main loop context: the contiguous run at the ring tail, consumed by exactly
 * the length that is handed out.  A reply that stops at the wrap point only
 * means the host asks again. */
u16 uart_rx_take(u16 want, u8 **out)
{
	u8 have = (u8)((rx_head - rx_tail) & RX_MASK);
	u8 run = (u8)(V003_UART_RX_RING_SIZE - rx_tail);
	u16 len;

	if (run > have)
		run = have;

	len = run < want ? run : want;
	*out = &rx_ring[rx_tail];
	rx_tail = (u8)((rx_tail + len) & RX_MASK);

	return len;
}

/* ---------------------------------------------------------------commands */

void uart_handle_control_data(u16 cmd, const u8 *data, int len)
{
	switch (cmd) {
	case V003_UART_CONFIG: {
		struct v003_uart_cfg cfg;

		if (len < (int)sizeof(cfg)) {
			LogUEvent(0xdead0010, len, 0, 0);
			return;
		}

		memcpy(&cfg, data, sizeof(cfg));
		if (!uart_apply(&cfg))
			LogUEvent(0xdead0011, cmd, len, 0);
		return;
	}
	case V003_UART_WRITE:
		if (!uart_on)
			return;

		uart_queue(data, len);
		return;
	default:
		return;
	}
}

u32 handle_uart_in_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_UART_GET_STATE:
		return ((u32)((tx_head - tx_tail) & TX_MASK) << 24) |
		       ((u32)((rx_head - rx_tail) & RX_MASK) << 16) |
		       ((u32)tx_dropped << 8) | rx_dropped;
	case V003_UART_GET_COUNTS:
		return ((tx_bytes & 0xffff) << 16) | (rx_bytes & 0xffff);
	case V003_UART_GET_ERRORS:
		return ((u32)rx_isrs << 24) | ((u32)rx_overrun << 16) |
		       ((u32)rx_framing << 8) | rx_parity;
	case V003_UART_GET_INFO:
		return ((u32)V003_UART_RX_RING_SIZE << 16) | V003_UART_COUNT;
	default:
		return 0;
	}
}

/* zero length OUT requests: state changes that answer nothing */
void uart_handle_out_request(u16 cmd, u16 data)
{
	switch (cmd) {
	case V003_UART_FLUSH: {
		u8 have = (u8)((rx_head - rx_tail) & RX_MASK);

		/* dropping what the receiver holds is a deliberate loss, so it
		 * is counted, not hidden */
		while (have--) {
			u8_sat(&rx_dropped);
			rx_tail = (u8)((rx_tail + 1) & RX_MASK);
		}

		return;
	}
	case V003_UART_CLEAR_STATS:
		/* statistics are what a host measures a session against; see
		 * vendor/uart.h for why they are cleared on request instead of
		 * wrapping silently */
		tx_dropped = 0;
		rx_dropped = 0;
		rx_overrun = 0;
		rx_framing = 0;
		rx_parity = 0;
		rx_isrs = 0;
		return;
	default:
		return;
	}
}
