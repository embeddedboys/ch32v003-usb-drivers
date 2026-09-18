# USB identity: the vendor and product id

`lib/v003_usb_ids.h` is the single source: the firmware builds its device
descriptor from it, `kernel/usb-mfd.c` matches on it (kbuild gets
`-I$(src)/../lib`), and `scripts/v003_usb.py` parses it so the host tools cannot
drift.  Changing the identity is a change to that one file.

```
V003_USB_VID 0x1209   V003_USB_PID 0xc303   (V003_USB_BOOTLOADER_PID 0xb003)
```

## Why these numbers, and why `lsusb` says "Generic"

`0x1209` is [pid.codes](https://pid.codes), the vendor id that is handed out to
open source hardware projects: the vendor id is shared and each project gets its
own PID under it.  That is also the id the upstream CH32V003 USB bootloader uses
(`1209:b003`, registered to cnlohr - the board is flashed with exactly that
bootloader), so staying in the same family is deliberate.

The price of a shared vendor id is the name.  `0x1209` is in the databases as
"Generic", and that is what a lot of tools print in front of the product name.

What each tool actually shows, measured on this bench:

| Tool                      | Vendor field                                            |
| ------------------------- | ------------------------------------------------------- |
| `lsusb`                   | the *database* name, i.e. `Generic`                      |
| `lsusb -v`                | same (`idVendor 0x1209 Generic`)                          |
| `usb-devices`             | the device string: `Manufacturer=embeddedboys`            |
| udev / KDE / file manager | the device string (`ID_VENDOR=embeddedboys`)              |

The rule behind it, verified by flashing the firmware with `VENDOR_ID 0xbeef`
(a vendor id no database knows):

```
$ lsusb -d beef:c303
Bus 001 Device 004: ID beef:c303 embeddedboys CH32V003 USB Bridge
  idVendor           0xbeef embeddedboys      <- falls back to iManufacturer
```

So: **a known vendor id wins over the device's own manufacturer string, an
unknown one loses to it.**  Two consequences:

- `lsusb` can only be made to print a name of our choosing by using a vendor id
  that is *not* in the database - which means using somebody else's or an
  unallocated one.  That is squatting, it can collide with a real device, and it
  silently reverts the day a database learns that id.  Not worth it for a
  cosmetic field.
- The name `Generic` for `0x1209` is by design of the shared vendor id, not a
  defect and not something the device can influence.

On a Linux host "Generic" comes from
`/usr/lib/udev/hwdb.d/20-usb-vendor-model.hwdb`:

```
usb:v1209*
 ID_VENDOR_FROM_DATABASE=Generic
```

compiled into `/usr/lib/udev/hwdb.bin`.  A product specific drop-in
(`usb:v1209pC303*` -> `ID_VENDOR_FROM_DATABASE=embeddedboys`) does **not** win:
a shorter pattern sorts first in the compiled database, and the measured result
was that the broad `usb:v1209*` entry kept being used.  Overriding it means
editing that vendor line, which renames *every* pid.codes device on that machine
(269 PIDs: ODrive, Kiibohd, Bus Pirate, ...).  See
[debugging.md](debugging.md) for the tooling used to find this.

## What to do about it

1. **Register the PID** at pid.codes (free; the project has to be open source,
   which this one is).  `1209:C303` currently returns 404, i.e. it is not
   registered, while `1209:B003` (the bootloader) is.  Registering makes the
   claim legitimate and gives a page others can check.
2. **Add a product entry upstream** so that every machine displays the device
   without knowing our repository: send a pull request to
   [hwdata](https://github.com/vcrhonek/hwdata) adding a line to `usb.ids`:

   ```
   1209  Generic
       ...
       c303  CH32V003 USB Bridge
   ```

   systemd's `20-usb-vendor-model.hwdb` is generated from that file, so the entry
   propagates to `lsusb`, udev and every desktop on the next update.  The vendor
   name stays `Generic` - again, that is the shared vendor id, so the result is
   `Generic CH32V003 USB Bridge`, which is exactly how ODrive, Kiibohd and the
   rest show up.
3. **Want the vendor field to read `embeddedboys`?** Then the device needs a
   vendor id allocated to embeddedboys (USB-IF membership, or a reseller block).
   That is a product decision, and the day it happens it is a one line change in
   `lib/v003_usb_ids.h` plus a registration of the new id in the databases.

## Firmware side

The strings next to it live in `vendor/usb_config.h` and are *not* part of the
identity header, because they are UTF-16 descriptor literals:

```c
#define STR_MANUFACTURER u"embeddedboys"
#define STR_PRODUCT      u"CH32V003 USB Bridge"
```

The serial number string is not a literal: it is built at boot from the factory
ESIG unique id (`V003_GET_DEVICE_UID`, 0x3c, and the same value in hex as the
string), so hosts and udev rules can tell two boards apart.  Why that costs a RAM
copy and not a pointer into the ESIG is in [firmware.md](firmware.md).

Their length is computed with `sizeof()` when the descriptor table is built, so
renaming the product needs no other change; each character costs two bytes of
flash.  A product string longer than the 8 byte endpoint packet (about 3
characters) is returned over several packets, which rv003usb handles - verified
with the 35 character version earlier in this session.
