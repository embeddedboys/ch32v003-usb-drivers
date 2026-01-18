#!/usr/bin/env python3

import time

import usb.core
import usb.util

PA1 = 1
PA2 = 2
PC0 = 32
PC1 = 33
PC2 = 34
PC3 = 35
PC4 = 36
PC5 = 37
PC6 = 38
PC7 = 39
PD0 = 48
PD1 = 49
PD2 = 50
PD3 = 51
PD4 = 52
PD5 = 53
PD6 = 54
PD7 = 55

dev = usb.core.find(idVendor=0x1209, idProduct=0xC303)
if dev is None:
    raise ValueError("Device not found")

# get an endpoint instance
cfg = dev.get_active_configuration()
intf = cfg[(0, 0)]
# print(intf)

ep = usb.util.find_descriptor(
    intf,
    # match the first OUT endpoint
    custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
    == usb.util.ENDPOINT_OUT,
)

assert ep is not None

V003_GPIO_MODULE_ID = 0x01


def V003_CMD(cmd, id):
    return cmd | id << 8


def V003_GPIO_CMD(cmd):
    return V003_CMD(cmd, V003_GPIO_MODULE_ID)


def V003_GPIO_VAL(val, idx):
    return val | idx << 8


V003_GPIO_SET = V003_GPIO_CMD(0x06)
V003_GPIO_GET = V003_GPIO_CMD(0x07)

ctrl_buf = range(8)

# send vendor specific request type and data in ctrl transfer
# will handled by `usb_handle_other_control_message`
dev.ctrl_transfer(
    0x40,  # bmRequestType
    0x00,  # bRequest
    V003_GPIO_VAL(1, PC0),  # wValue | val & idx
    V003_GPIO_SET,  # wIndex | cmd & id
    None,  # ctrl_buf
)

data = dev.ctrl_transfer(0xC0, 0x00, V003_GPIO_VAL(0, PC0), V003_GPIO_GET, 1)
print(f"pin {PC0} state : {data[0]}")

time.sleep(0.3)

dev.ctrl_transfer(
    0x40,  # bmRequestType
    0x00,  # bRequest
    V003_GPIO_VAL(0, PC0),  # wValue | val & idx
    V003_GPIO_SET,  # wIndex | cmd & id
    None,  # ctrl_buf
)

data = dev.ctrl_transfer(0xC0, 0x00, V003_GPIO_VAL(0, PC0), V003_GPIO_GET, 1)
print(f"pin {PC0} state : {data[0]}")


def blink(val, idx):
    dev.ctrl_transfer(
        0x40,  # bmRequestType
        0x00,  # bRequest
        V003_GPIO_VAL(val, idx),  # wValue | val & idx
        V003_GPIO_SET,  # wIndex | cmd & id
        None,  # ctrl_buf
    )


while True:
    blink(1, PC0)
    time.sleep(0.5)
    blink(0, PC0)
    time.sleep(0.5)
