#ifndef __SPI_H
#define __SPI_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_SPI_MODULE_ID 0x02
#define V003_SPI_CMD(cmd)  V003_CMD(cmd, V003_SPI_MODULE_ID)

/* OUT, wValue = 0/1: enable/disable the SPI data path.  When enabled every
 * byte written by the host to EP2 OUT is clocked out on MOSI and the byte
 * received on MISO at the same time is queued to the EP3 IN FIFO, so one
 * EP2 OUT packet produces one EP3 IN packet. */
#define V003_SPI_ENABLE V003_SPI_CMD(0x40)

/* OUT, wValue = (prescaler << 8) | mode: mode bit0 = CPHA, bit1 = CPOL,
 * prescaler is the SPI_CTLR1 BR field (0 = /2 ... 7 = /256).  Both default
 * to 0 (mode 0, 24 MHz at 48 MHz APB2). */
#define V003_SPI_CONFIG V003_SPI_CMD(0x41)

/* IN -> (prescaler << 16) | (mode << 8) | enabled */
#define V003_SPI_GET_STATE V003_SPI_CMD(0x42)

/* IN -> number of bytes clocked out on MOSI since reset */
#define V003_SPI_GET_STATS V003_SPI_CMD(0x43)

/* OUT, wValue = pin index (0..55) or 0xffff for "no chip select": the pin is
 * driven low for the duration of a V003_SPI_TRANSFER.  Must not be one of the
 * SPI alternate function pins. */
#define V003_SPI_SET_CS V003_SPI_CMD(0x44)

/* OUT with a data stage: clock the received bytes out on MOSI (driving chip
 * select, if configured) and keep the bytes sampled on MISO for
 * V003_SPI_GET_RX.  Up to 64 bytes, one atomic transfer. */
#define V003_SPI_TRANSFER V003_SPI_CMD(0x45)

/* IN -> the MISO bytes of the last V003_SPI_TRANSFER, clamped to its length */
#define V003_SPI_GET_RX V003_SPI_CMD(0x46)

/* IN -> the chip select pin, 0xffff when chip select is not driven */
#define V003_SPI_GET_CS V003_SPI_CMD(0x47)

/* SPI1 alternate function pins, chip select is not driven by the firmware:
 * the host toggles any GPIO pin through the GPIO module (V003_GPIO_SET). */
#define V003_SPI_SCK  PC5
#define V003_SPI_MOSI PC6
#define V003_SPI_MISO PC7

extern int spi_enabled(void);
extern void spi_out_bytes(const u8 *data, int len);
extern void handle_spi_out_request(u16 cmd, u16 data);
extern u32 handle_spi_in_request(u16 cmd, u16 data);

/* MISO bytes of the last V003_SPI_TRANSFER */
extern const u8 *spi_rx_data(void);
extern u16 spi_rx_length(void);
/* one atomic transfer, used by the control-OUT data stage path */
extern int spi_transfer_bytes(const u8 *data, int len);

#endif /* __SPI_H */
