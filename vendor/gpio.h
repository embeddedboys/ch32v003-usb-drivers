#ifndef __GPIO_H
#define __GPIO_H

#include "ch32fun.h"
#include "vendor.h"

#define V003_GPIO_MODULE_ID 0x01
#define V003_GPIO_CMD(cmd)  V003_CMD(cmd, V003_GPIO_MODULE_ID)

#define V003_GPIO_SET		   V003_GPIO_CMD(0x06)
#define V003_GPIO_GET		   V003_GPIO_CMD(0x07)
#define V003_GPIO_REQUEST	   V003_GPIO_CMD(0x08)
#define V003_GPIO_FREE		   V003_GPIO_CMD(0x09)
#define V003_GPIO_GET_DIRECTION	   V003_GPIO_CMD(0x0A)
#define V003_GPIO_DIRECTION_INPUT  V003_GPIO_CMD(0x0B)
#define V003_GPIO_DIRECTION_OUTPUT V003_GPIO_CMD(0x0C)

extern u32 handle_gpio_in_request(u16 cmd, u16 data);
extern void handle_gpio_out_request(u16 cmd, u16 data);

#endif
