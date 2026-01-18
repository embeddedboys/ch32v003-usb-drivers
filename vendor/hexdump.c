#include <stdio.h>
#include <stdint.h>

static int isprint(int c)
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
