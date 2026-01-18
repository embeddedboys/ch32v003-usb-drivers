#!/usr/bin/env python3

import usb.core
import usb.util

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


def v003_cmd(cmd, id):
    return cmd | id << 8


def v003_gpio_cmd(cmd):
    return v003_cmd(cmd, V003_GPIO_MODULE_ID)


ctrl_buf = range(8)

# send vendor specific request type and data in ctrl transfer
# will handled by `usb_handle_other_control_message`
dev.ctrl_transfer(
    0x40,  # bmRequestType
    0x00,  # bRequest
    0x12,  # wValue | size
    v003_gpio_cmd(0x55),  # wIndex | cmd & id
    None,  # ctrl_buf
)

# data = dev.ctrl_transfer(0xC0, 0x00, 0, v003_gpio_cmd(0x51), 4)
# print(list(data))

# write the data
# will handled by `usb_handle_user_data`
# ep.write("test")
