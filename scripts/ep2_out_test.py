#!/usr/bin/env python3

import usb.core
import usb.util

from v003_usb import find_device

dev = find_device()
if dev is None:
    raise ValueError("Device not found")

# get an endpoint instance
cfg = dev.get_active_configuration()
intf = cfg[(0, 0)]
print(intf)

ep = usb.util.find_descriptor(
    intf,
    # match the first OUT endpoint
    custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
    == usb.util.ENDPOINT_OUT,
)

assert ep is not None

# write the data
ep.write("Hello, World!")
