#ifndef __GPIO_H
#define __GPIO_H

#include "vendor.h"

#define V003_GPIO_MODULE_ID 0x01
#define V003_GPIO_CMD(cmd) V003_CMD(cmd, V003_GPIO_MODULE_ID)

#define V003_GPIO_SET V003_GPIO_CMD(0x06)
#define V003_GPIO_GET V003_GPIO_CMD(0x07)

#endif
