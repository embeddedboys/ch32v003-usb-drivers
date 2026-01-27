#!/usr/bin/env python3
import os
import sys
import time

SYS_GPIO = "/sys/class/gpio"
EXPORT = f"{SYS_GPIO}/export"
UNEXPORT = f"{SYS_GPIO}/unexport"


def sysfs_write(path, value):
    try:
        with open(path, "w") as f:
            f.write(str(value))
        return True
    except Exception as e:
        print("error :", e)
        return False


def sysfs_read(path):
    with open(path, "r") as f:
        return f.read()


def test_one_gpio(gpio_num: int):
    gpio_path = f"{SYS_GPIO}/gpio{gpio_num}"

    print(f"\nTesting GPIO {gpio_num}...")

    # 1. export
    if not os.path.exists(gpio_path):
        if not sysfs_write(EXPORT, gpio_num):
            return False

    if not sysfs_write(f"{gpio_path}/direction", "out"):
        return False

    print("blink 3 times...")
    for i in range(3):
        sysfs_write(f"{gpio_path}/value", 1)
        time.sleep(0.1)
        sysfs_write(f"{gpio_path}/value", 0)
        time.sleep(0.1)

    if not sysfs_write(f"{gpio_path}/direction", "in"):
        return False

    print("read value...")
    value = sysfs_read(f"{gpio_path}/value")
    print(f"value: {value}")

    sysfs_write(UNEXPORT, gpio_num)

    print("done")
    return True


"""
[13316.600851] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_request, offset : 32
[13316.600855] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_get_direction, offset : 32
[13316.600905] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_direction_output, offset : 32, value : 0
[13316.600917] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_set, offset : 32, value : 1
[13316.701239] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_set, offset : 32, value : 0
[13316.801659] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_set, offset : 32, value : 1
[13316.902077] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_set, offset : 32, value : 0
[13317.002696] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_set, offset : 32, value : 1
[13317.103182] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_set, offset : 32, value : 0
[13317.203621] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_direction_input, offset : 32
[13317.203653] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_get, offset : 32
[13317.203655] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_get, offset : 32
[13317.203699] v003-usb-mfd 1-9.1:1.0: v003_usb_gpio_free, offset : 32
"""

if __name__ == "__main__":
    try:
        test_one_gpio(int(sys.argv[1]))
    except:
        print("Usage: {} <gpio_num>".format(sys.argv[0]))
