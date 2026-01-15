#include "ch32fun.h"
#include "rv003usb.h"
#include <stdio.h>
#include <string.h>

int main()
{
	SystemInit();
	usb_setup();

	for (;;) {
		uint32_t *ue = GetUEvent();

		if (ue)
			printf("%lu %lx %lx %lx\n", ue[0], ue[1], ue[2], ue[3]);
	}

	return 0;
}

int isprint(int c)
{
	unsigned char uc = (unsigned char)c;

	return (uc >= 32 && uc <= 126);
}

void hexdump(const void *data, uint32_t size)
{
	const uint8_t *data_ptr = (const uint8_t *)data;
	uint32_t i, b;

	for (i = 0; i < size; i++) {
		if (i % 16 == 0) {
			printf("%08lx  ", (uint32_t)data_ptr + i);
		}
		if (i % 8 == 0) {
			printf(" ");
		}
		printf("%02x ", data_ptr[i]);
		if (i % 16 == 15) {
			printf(" |");
			for (b = 0; b < 16; b++) {
				if (isprint(data_ptr[i + b - 15])) {
					printf("%c", data_ptr[i + b - 15]);
				} else {
					printf(".");
				}
			}
			printf("|\n");
		}
	}
	printf("%08lx\n", 16 + size - (size % 16));
}

void usb_handle_user_in_request(struct usb_endpoint *e, uint8_t *scratchpad,
				int endp, uint32_t sendtok,
				struct rv003usb_internal *ist)
{
	if (endp == 3) {
		// usb_send_data( (uint8_t*)"Hello!~~", 8, 0, sendtok );
		usb_send_empty(sendtok);
	} else if (endp == 1) {
		usb_send_empty(sendtok);
	} else if (endp == 2) {
		usb_send_empty(sendtok);
	} else {
		// If it's a control transfer, don't send anything.
		usb_send_empty(sendtok);
	}
}

void usb_handle_other_control_message(struct usb_endpoint *e, struct usb_urb *s,
				      struct rv003usb_internal *ist)
{
	LogUEvent(SysTick->CNT, s->wRequestTypeLSBRequestMSB,
		  s->lValueLSBIndexMSB, s->wLength);
	e->opaque = 0;
}

void usb_handle_user_data(struct usb_endpoint *e, int current_endpoint,
			  uint8_t *data, int len, struct rv003usb_internal *ist)
{
	LogUEvent(SysTick->CNT, 0xffffffff, current_endpoint, len);
	// if (current_endpoint == 2)
	// 	hexdump(data, len);
}
