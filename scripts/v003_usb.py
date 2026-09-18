#!/usr/bin/env python3
"""USB identity and device lookup for the CH32V003 bridge.

The vendor and product id are not repeated here: they are parsed out of
lib/v003_usb_ids.h, the same header the firmware builds its device descriptor
from and the kernel driver matches on.  One place to change, and a mismatch
between the host tools and the device becomes impossible instead of unlikely.

    from v003_usb import VID, PID, find_device

    dev = find_device()
"""

import os
import re

import usb.core
import usb.util

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib",
                      "v003_usb_ids.h")


def _parse_ids(path=HEADER):
    with open(path, encoding="utf-8") as header:
        text = header.read()

    ids = {}
    for name, key in (("V003_USB_VID", "vid"), ("V003_USB_PID", "pid")):
        match = re.search(r"^#define\s+%s\s+(0x[0-9a-fA-F]+)" % name, text,
                          re.MULTILINE)
        if not match:
            raise RuntimeError(f"{name} not found in {path}")
        ids[key] = int(match.group(1), 16)

    return ids["vid"], ids["pid"]


VID, PID = _parse_ids()


def find_device(dev=None):
    """The bridge device, or None when it is not plugged in."""
    if dev is None:
        return usb.core.find(idVendor=VID, idProduct=PID)
    return dev if (dev.idVendor, dev.idProduct) == (VID, PID) else None


if __name__ == "__main__":
    print(f"expected {VID:#06x}:{PID:#04x} (from {os.path.normpath(HEADER)})")
    found = find_device()
    print("found:", found if found is not None else "not plugged in")
