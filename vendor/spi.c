#include "ch32fun.h"
#include "rv003usb.h"

#include "spi.h"

/* Hardware SPI1 master.  The chip is used as a USB <-> SPI bridge: payload
 * from EP2 OUT is clocked out on MOSI, the simultaneously received MISO byte
 * is pushed to the EP3 IN FIFO (see spi.h for the protocol). */

static volatile u8 spi_on;
static volatile u8 spi_mode;	  /* 0..3, CPOL/CPHA */
static volatile u8 spi_prescaler; /* SPI_CTLR1 BR field, 0 = /2 */
static volatile u32 spi_bytes;	  /* MOSI bytes clocked out */
static volatile u8 spi_cs_pin = 0xff; /* 0xff = chip select not driven */
static volatile u8 spi_busy;	  /* a control transfer is clocking bytes */

/* MISO bytes of the last V003_SPI_TRANSFER */
#define SPI_RX_BUF_SIZE 64
static u8 spi_rx_buf[SPI_RX_BUF_SIZE];
static volatile u16 spi_rx_len;

const u8 *spi_rx_data(void)
{
	return spi_rx_buf;
}

u16 spi_rx_length(void)
{
	return spi_rx_len;
}

static void spi_cs_drive(int active)
{
	if (spi_cs_pin == 0xff)
		return;

	/* active low */
	funDigitalWrite(spi_cs_pin, active ? 0 : 1);
}

int spi_enabled(void)
{
	return spi_on;
}

static void spi_hw_setup(void)
{
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1;

	/* SCK/MOSI alternate function push-pull, MISO floating input */
	funPinMode(V003_SPI_SCK, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF);
	funPinMode(V003_SPI_MOSI, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP_AF);
	funPinMode(V003_SPI_MISO, GPIO_Speed_In | GPIO_CNF_IN_FLOATING);

	/* CTLR1 can only be written with SPE cleared */
	SPI1->CTLR1 = 0;
	SPI1->CTLR1 = SPI_CTLR1_MSTR | SPI_CTLR1_SSM | SPI_CTLR1_SSI |
		      (u16)((spi_prescaler & 0x7) << 3) |
		      ((spi_mode & 0x1) ? SPI_CTLR1_CPHA : 0) |
		      ((spi_mode & 0x2) ? SPI_CTLR1_CPOL : 0) | SPI_CTLR1_SPE;
}

static void spi_hw_release(void)
{
	SPI1->CTLR1 = 0;

	funPinMode(V003_SPI_SCK, GPIO_Speed_In | GPIO_CNF_IN_FLOATING);
	funPinMode(V003_SPI_MOSI, GPIO_Speed_In | GPIO_CNF_IN_FLOATING);
	funPinMode(V003_SPI_MISO, GPIO_Speed_In | GPIO_CNF_IN_FLOATING);
}

static u8 spi_byte(u8 out)
{
	while (!(SPI1->STATR & SPI_STATR_TXE))
		;
	SPI1->DATAR = out;
	while (!(SPI1->STATR & SPI_STATR_RXNE))
		;

	return (u8)SPI1->DATAR;
}

void spi_out_bytes(const u8 *data, int len)
{
	u8 rx[8];
	int i;

	/* a control transfer (V003_SPI_TRANSFER) owns the peripheral while it
	 * runs; the host is expected to use one path at a time */
	if (!spi_on || len <= 0 || spi_busy)
		return;

	/* EP max packet size, usb_handle_user_data() never hands us more */
	if (len > (int)sizeof(rx))
		len = sizeof(rx);

	for (i = 0; i < len; i++)
		rx[i] = spi_byte(data[i]);

	spi_bytes += (u32)len;
	v003_ep3_in_push(rx, len);
}

/* one atomic (chip select framed) transfer, driven from the main loop when a
 * control-OUT data stage completed */
int spi_transfer_bytes(const u8 *data, int len)
{
	int i;

	if (!spi_on || len <= 0)
		return 0;

	if (len > (int)sizeof(spi_rx_buf))
		len = sizeof(spi_rx_buf);

	spi_busy = 1;
	spi_cs_drive(1);
	for (i = 0; i < len; i++)
		spi_rx_buf[i] = spi_byte(data[i]);
	spi_cs_drive(0);
	spi_rx_len = len;
	spi_bytes += (u32)len;
	spi_busy = 0;

	return len;
}

void handle_spi_out_request(u16 cmd, u16 data)
{
	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_SPI_ENABLE:
		spi_on = (data & 0xff) ? 1 : 0;
		if (spi_on)
			spi_hw_setup();
		else
			spi_hw_release();
		break;
	case V003_SPI_CONFIG:
		spi_mode = data & 0x03;
		spi_prescaler = (data >> 8) & 0x07;
		if (spi_on)
			spi_hw_setup();
		break;
	case V003_SPI_SET_CS:
		if (data == 0xffff || data > 55) {
			spi_cs_pin = 0xff;
			break;
		}

		spi_cs_pin = (u8)data;
		funPinMode(spi_cs_pin, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);
		funDigitalWrite(spi_cs_pin, 1);
		break;
	default:
		/* unsupported spi request */
		break;
	}
}

u32 handle_spi_in_request(u16 cmd, u16 data)
{
	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_SPI_GET_STATE:
		return (u32)spi_on | ((u32)spi_mode << 8) |
		       ((u32)spi_prescaler << 16);
	case V003_SPI_GET_STATS:
		return spi_bytes;
	case V003_SPI_GET_CS:
		return spi_cs_pin == 0xff ? 0xffff : spi_cs_pin;
	default:
		return 0;
	}
}
