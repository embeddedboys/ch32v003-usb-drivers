#!/usr/bin/env python3

import usb.core
import usb.util

dev = usb.core.find(idVendor=0x1209, idProduct=0xC303)
if dev is None:
    raise ValueError("Device not found")

# get an endpoint instance
cfg = dev.get_active_configuration()
intf = cfg[(1, 0)]
print(intf)

ep = usb.util.find_descriptor(
    intf,
    # match the first OUT endpoint
    custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
    == usb.util.ENDPOINT_OUT,
)

assert ep is not None

ctrl_buf = range(8)

# send vendor specific request type and data in ctrl transfer
# will handled by `usb_handle_other_control_message`
dev.ctrl_transfer(0x40, 0x01, 0x00, 0x00, ctrl_buf)

# write the data
# will handled by `usb_handle_user_data`
ep.write("test")
