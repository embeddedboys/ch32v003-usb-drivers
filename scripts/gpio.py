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


class Gpio:
    V003_GPIO_MODULE_ID = 0x01

    def __init__(self, dev):
        self.V003_GPIO_SET = self.V003_GPIO_CMD(0x06)
        self.V003_GPIO_GET = self.V003_GPIO_CMD(0x07)

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

    def V003_GPIO_CMD(self, cmd):
        return self.V003_CMD(cmd, self.V003_GPIO_MODULE_ID)

    def V003_GPIO_VAL(self, val, idx):
        return val | idx << 8

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
        return data if is_out else data[0]

    def gpio_set(self, idx, state):
        return self.transfer(
            self.V003_GPIO_VAL(state, idx),
            self.V003_GPIO_SET,
            0,  # don't need in OUT transfer
        )

    def gpio_get(self, idx):
        return self.transfer(
            self.V003_GPIO_VAL(0, idx),
            self.V003_GPIO_GET,
            1,  # wLength to read
        )


def blink(dev):
    dev.gpio_set(PC0, 1)
    time.sleep(0.5)
    dev.gpio_set(PC0, 0)
    time.sleep(0.5)


def main():
    dev = find_device()
    if dev is None:
        raise ValueError("Device not found")

    dev = Gpio(dev)

    dev.gpio_set(PC0, 1)
    print(f"pin {PC0} state : {dev.gpio_get(PC0)}")
    dev.gpio_set(PC0, 0)
    print(f"pin {PC0} state : {dev.gpio_get(PC0)}")

    while True:
        blink(dev)


if __name__ == "__main__":
    main()
