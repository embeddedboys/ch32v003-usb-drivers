/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The USB identity of this device.
 *
 * One copy, shared by everything that has to agree on it:
 *
 *   - the firmware, which builds the device descriptor from it
 *     (vendor/usb_config.h, included with -I../lib);
 *   - the kernel driver, which matches on it (kernel/usb-mfd.c);
 *   - the host side python scripts, which parse this file
 *     (scripts/v003_usb.py) so they cannot silently drift.
 *
 * The IDs are pid.codes (https://pid.codes) ones: 0x1209 is the vendor id that
 * pid.codes hands out to open source hardware projects, and each project gets
 * its own PID under it.  Note what that means for the name a host displays:
 * the vendor *id* is shared, so every udev/hwdb database calls it "Generic" -
 * `lsusb` prints "Generic <product>" and ignores the device's own manufacturer
 * string, while `usb-devices` and the desktop GUIs do show it.  A vendor name
 * of our own would need a vendor id allocated to embeddedboys, which is a
 * product decision, not a firmware one.
 *
 * Registering the PID (free, the project has to be open source) and getting a
 * product entry into the upstream database are written down in
 * notes/usb-identity.md.
 */
#ifndef __V003_USB_IDS_H
#define __V003_USB_IDS_H

/* pid.codes "Generic" vendor id, shared by all of its projects */
#define V003_USB_VID 0x1209

/* this device: the CH32V003 USB to GPIO/I2C/SPI bridge firmware */
#define V003_USB_PID 0xc303

/*
 * The serial number string is built at boot out of the factory ESIG unique id
 * (96 bits, chapter 15 of the reference manual, at 0x1FFFF7E8) rendered as hex,
 * so every board identifies itself instead of sharing one placeholder.  These
 * sizes are shared with the firmware, which owns the buffer.
 */
#define V003_DEVICE_UID_SIZE  12
#define V003_SERIAL_DESC_SIZE (2 + V003_DEVICE_UID_SIZE * 4) /* header + UTF-16 */

/* the upstream USB HID bootloader the board is flashed with, for reference
 * (rv003usb / ch32fun): registered to cnlohr */
#define V003_USB_BOOTLOADER_VID V003_USB_VID
#define V003_USB_BOOTLOADER_PID 0xb003

#endif /* __V003_USB_IDS_H */
