#!/usr/bin/env python3
"""End-to-end test for the CH32V003 vendor device (VID 1209, PID C303).

Covers:
  1. generic module: device version / serial number / EP stats
  2. endpoint OUT reception (EP1/EP2) via interrupt OUT writes
  3. endpoint IN transmission (EP3) via interrupt IN read
  4. vendor control-OUT with a data stage
"""

import sys

import usb.core
import usb.util

VID = 0x1209
PID = 0xC303

V003_GENERIC_MODULE_ID = 0x00
V003_GPIO_MODULE_ID = 0x01


def v003_cmd(cmd, mid):
    return cmd | (mid << 8)


def generic_cmd(cmd):
    return v003_cmd(cmd, V003_GENERIC_MODULE_ID)


GET_DEVICE_VER = generic_cmd(0x30)
GET_DEVICE_SN = generic_cmd(0x31)
GET_EP_STATS = generic_cmd(0x32)

EP1_OUT = 0x01
EP2_OUT = 0x02
EP3_IN = 0x83


def find_ep(intf, addr):
    return usb.util.find_descriptor(intf, bEndpointAddress=addr)


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("FAIL: device not found")
        return 1

    cfg = dev.get_active_configuration()
    intf0 = cfg[(0, 0)]

    ep1 = find_ep(intf0, EP1_OUT)
    ep2 = find_ep(intf0, EP2_OUT)
    ep3 = find_ep(intf0, EP3_IN)
    if not (ep1 and ep2 and ep3):
        print("FAIL: endpoints not found:", ep1, ep2, ep3)
        return 1

    def tx(val, idx, ln=0):
        t = 0x40 if ln == 0 else 0xC0
        return dev.ctrl_transfer(t, 0x00, val, idx, ln, timeout=2000)

    def rx_u32(idx, which):
        r = tx(which, idx, 4)
        return int.from_bytes(r[0:4], "little")

    # --- 1. generic module -------------------------------------------
    ver = rx_u32(GET_DEVICE_VER, 0)
    sn = rx_u32(GET_DEVICE_SN, 0)
    print(f"1. device version : {ver:04x}  (expect 1010)")
    print(f"   device SN      : {sn:08x}  (expect 12345678)")
    if ver != 0x1010 or sn != 0x12345678:
        print("FAIL: generic module")
        return 1

    # --- 2. reset EP stats --------------------------------------------
    tx(0xFF, GET_EP_STATS, 0)  # OUT, wValue=0xff -> reset counters
    print("2. EP stats reset")

    # --- 3. EP1 OUT reception -----------------------------------------
    ep1.write(b"ep1data!")
    rx1 = rx_u32(GET_EP_STATS, 1)
    print(f"3. EP1 rx bytes : {rx1}  (expect 8)")
    if rx1 < 8:
        print("FAIL: EP1 reception")
        return 1

    # --- 4. EP2 OUT reception -----------------------------------------
    ep2.write(b"Hello, World!")
    rx2 = rx_u32(GET_EP_STATS, 2)
    print(f"4. EP2 rx bytes : {rx2}  (expect 13)")
    if rx2 < 13:
        print("FAIL: EP2 reception")
        return 1

    # --- 5. EP3 IN transmission ----------------------------------------
    data = ep3.read(8, timeout=2000)
    s = bytes(data)
    print(f"5. EP3 IN  data  : {s!r}  (expect b'Hello!~~')")
    tx_cnt = rx_u32(GET_EP_STATS, 3)
    print(f"   EP3 tx packets: {tx_cnt}")
    if s != b"Hello!~~":
        print("FAIL: EP3 transmission")
        return 1

    # --- 6. control-OUT with data stage --------------------------------
    payload = bytes(range(8))
    dev.ctrl_transfer(0x40, 0x00, 0x1234, 0x0034, payload, timeout=2000)
    print("6. control-OUT data stage sent:", payload.hex())

    print("\nALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
