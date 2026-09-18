#ifndef __V003_USB_GPIO_H
#define __V003_USB_GPIO_H

#define PA1 1
#define PA2 2
#define PC0 32
#define PC1 33
#define PC2 34
#define PC3 35
#define PC4 36
#define PC5 37
#define PC6 38
#define PC7 39
#define PD0 48
#define PD1 49
#define PD2 50
#define PD3 51
#define PD4 52
#define PD5 53
#define PD6 54
#define PD7 55

#define V003_NGPIO 56

#define V003_CMD(cmd, id) ((cmd) | (id << 8))
#define V003_CMD_GET_ID(cmd) (cmd >> 8)
#define V003_CMD_GET_CMD(cmd) (cmd & 0xFF)

#define V003_GENERIC_MODULE_ID 0x00
#define V003_GENERIC_CMD(cmd) V003_CMD(cmd, V003_GENERIC_MODULE_ID)
#define V003_GET_DEVICE_VER V003_GENERIC_CMD(0x30)
#define V003_GET_DEVICE_SN V003_GENERIC_CMD(0x31)
#define V003_GET_EP_STATS V003_GENERIC_CMD(0x32)

#define V003_GPIO_MODULE_ID 0x01
#define V003_GPIO_CMD(cmd) V003_CMD(cmd, V003_GPIO_MODULE_ID)

#define V003_GPIO_SET V003_GPIO_CMD(0x06)
#define V003_GPIO_GET V003_GPIO_CMD(0x07)
#define V003_GPIO_REQUEST V003_GPIO_CMD(0x08)
#define V003_GPIO_FREE V003_GPIO_CMD(0x09)
#define V003_GPIO_GET_DIRECTION V003_GPIO_CMD(0x0A)
#define V003_GPIO_DIRECTION_INPUT V003_GPIO_CMD(0x0B)
#define V003_GPIO_DIRECTION_OUTPUT V003_GPIO_CMD(0x0C)

#define V003_GPIO_VAL(idx, val) (idx << 8 | val)

#endif /* __V003_USB_GPIO_H */
