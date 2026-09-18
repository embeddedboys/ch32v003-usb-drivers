#!/usr/bin/env python3

import time

import usb.core
import usb.util

from v003_usb import find_device

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


class Generic:
    V003_GENERIC_MODULE_ID = 0x00

    def __init__(self, dev):
        self.V003_GET_DEVICE_VER = self.V003_GENERIC_CMD(0x30)
        self.V003_GET_DEVICE_SN = self.V003_GENERIC_CMD(0x31)

        self.dev = dev
        # get an endpoint instance
        cfg = dev.get_active_configuration()
        intf = cfg[(0, 0)]
        # print(intf)

        self.ep = usb.util.find_descriptor(
            intf,
            # match the first OUT endpoint
            custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
            == usb.util.ENDPOINT_OUT,
        )

        assert self.ep is not None

    def V003_CMD(self, cmd, id):
        return cmd | id << 8

    def V003_GENERIC_CMD(self, cmd):
        return self.V003_CMD(cmd, self.V003_GENERIC_MODULE_ID)

    def transfer(self, val, idx, len):
        is_out: bool = False if len > 0 else True
        type = 0x40 if is_out else 0xC0
        # send vendor specific request type and data in ctrl transfer
        # will handled by `usb_handle_other_control_message`
        data = self.dev.ctrl_transfer(
            type,  # bmRequestType
            0x00,  # bRequest
            val,  # wValue | val & idx
            idx,  # wIndex | cmd & id
            len,  # wLength
        )
        return data if is_out else int.from_bytes(data[0:len], "little")

    def get_device_ver(self):
        return self.transfer(
            0,
            self.V003_GET_DEVICE_VER,
            2,
        )

    def get_device_sn(self):
        return self.transfer(
            0,
            self.V003_GET_DEVICE_SN,
            4,
        )


def main():
    dev = find_device()
    if dev is None:
        raise ValueError("Device not found")

    dev = Generic(dev)

    print(f"device version : {dev.get_device_ver():04x}")
    print(f"device serial number : {dev.get_device_sn():08x}")


if __name__ == "__main__":
    main()
