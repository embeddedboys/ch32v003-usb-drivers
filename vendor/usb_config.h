#ifndef _USB_CONFIG_H
#define _USB_CONFIG_H

// clang-format off

//Defines the number of endpoints for this device. (Always add one for EP0). For two EPs, this should be 3.
#define ENDPOINTS 4

#define USB_PIN_DP 3
#define USB_PIN_DM 4
#define USB_PIN_DPU 5
#define USB_PORT D

#define RV003USB_HANDLE_IN_REQUEST   1
#define RV003USB_OTHER_CONTROL       1
#define RV003USB_HANDLE_USER_DATA    1
#define RV003USB_HID_FEATURES        0
#define RV003USB_SUPPORT_CONTROL_OUT 1

/*
 * The USB stack's EXTI handler covers lines 0..7 and, without this hook, treats
 * *any* interrupt on them as bus traffic (it reads D+/D- and runs the state
 * machine).  The power module wants line 6 - the board's boot button - as a wake
 * source, and this is the upstream-provided way to say "this line is not yours":
 * the handler checks EXTI->INTFR against the mask and, when the USB line is not
 * the cause, runs RV003_ADD_EXTI_HANDLER instead and clears only the pending bits
 * for the USB line and this mask.
 */
#if V003_MODULE_PWR
#define RV003_ADD_EXTI_MASK    (1u << 6) /* PD6, the boot button */
#define RV003_ADD_EXTI_HANDLER call pwr_button_edge
#endif

#define RV003USB_EVENT_DEBUGGING   1
/* The ring is a debug capture facility: only our own LogUEvent pushes into it
 * (the USB stack's state machine does not depend on it) and main() drains it
 * every loop.  16 bytes per entry out of the same 2048 the receive rings want,
 * and 32 entries would eat 512 of them - the stack grows down from 0x20000800
 * straight into the statics, so an oversized ring does not just waste memory,
 * it causes silent corruption of whatever sits at the end of .bss.  Two entries
 * (32 bytes) is the floor the modules have left: the power module's state and
 * the UART's rings came out of the same 2 kB, and `make UEVENTS=4` (or 8) brings
 * the depth back for a debugging session - it is a debug facility, and what it
 * loses is counted nowhere, which is exactly why it is the first thing to give. */
#ifndef RV003USB_NUMUEVENTS
#define RV003USB_NUMUEVENTS        2
#endif

#ifndef __ASSEMBLER__

#include <tusb_types.h>
#include <cdc.h>
#include <usb_def.h>
#include <usb_util.h>

/* the USB identity lives in one place, shared with the kernel driver and the
 * host side scripts (see the header for why it is a pid.codes id) */
#include "v003_usb_ids.h"

#ifdef INSTANCE_DESCRIPTORS

//Taken from http://www.usbmadesimple.co.uk/ums_ms_desc_dev.htm
static const uint8_t device_descriptor[] = {

	// 0x12, 			// bLength: Length
	// TUSB_DESC_DEVICE,  	// bDescriptorType: Type (Device)
	// 0x10, 0x01, 		// bcdUSB: Spec
	// TUSB_CLASS_VENDOR_SPECIFIC, // bDeviceClass: Device Class (Let config decide)
	// 0x00, 			// bDeviceSubClass: Subclass
	// 0x00, 			// bDeviceProtocol: Device Protocol
	// 0x08, 			// bMaxPacketSize: Max packet size for EP0 (This has to be 8 because of the USB Low-Speed Standard)
	// 0x09, 0x12, // idVendor: ID Vendor
	// 0x03, 0xc3, // idProduct: ID Product
	// 0x10, 0x01, // bcdDevice: ID Rev
	// 1, // iManufacturer: Manufacturer string
	// 2, // iProduct: Product string
	// 3, // iSerial: Serial string
	// 1, // bNumConfigurations: Max number of configurations

	USB_DEVICE_DESCRIPTOR_INIT(
		USB_1_1,	/* bcdUSB */
		0xFF,		/* bDeviceClass */
		0x00,		/* bDeviceSubClass */
		0x00,		/* bDeviceProtocol */
		0x08,		/* bMaxPacketSize */
		V003_USB_VID,	/* idVendor */
		V003_USB_PID,	/* idProduct */
		USB_1_1,	/* bcdDevice */
		1		/* bNumConfigurations */
	),
};

static const uint8_t config_descriptor[] = {
	// configuration descriptor, USB spec 9.6.3, page 264-266, Table 9-10
	// based on https://gist.github.com/tai/acd59b125a007ad47767

#if 0
	0x09,                     // bLength;
	TUSB_DESC_CONFIGURATION,  // bDescriptorType;
	25 + 16, 0x00,            // wTotalLength
	0x02,                     // bNumInterfaces (Normally 1)
	0x01,                     // bConfigurationValue
	0x00,                     // iConfiguration
	0x80,                     // bmAttributes (was 0xa0)
	0x64,                     // bMaxPower (200mA)
#else
	USB_CONFIG_DESCRIPTOR_INIT(
		9 + 9 + 7 + 7 + 7,	/* wTotalLength */
		0x01,			/* bNumInterfaces */
		0x01,			/* bConfigurationValue */
		0x80,			/* bmAttributes */
		0x64			/* bMaxPower */
	),
	USB_INTERFACE_DESCRIPTOR_INIT(
		0x00,		/* bInterfaceNumber */
		0x00,		/* bAlternateSetting */
		0x03,		/* bNumEndpoints */
		0xFF,		/* bInterfaceClass */
		0x00,		/* bInterfaceSubClass */
		0x00,		/* bInterfaceProtocol */
		0x00		/* iInterface */
	),

	USB_ENDPOINT_DESCRIPTOR_INIT(
		0x01,
		0x03,
		0x08,
		0x01
	),

	USB_ENDPOINT_DESCRIPTOR_INIT(
		0x02,
		0x03,
		0x08,
		0x01
	),

	USB_ENDPOINT_DESCRIPTOR_INIT(
		0x83,
		0x03,
		0x08,
		0x01
	),
};
#endif

// #define STR_MANUFACTURER u"cnlohr"
// #define STR_PRODUCT      u"CDC Tester"
#define STR_MANUFACTURER u"embeddedboys"
#define STR_PRODUCT      u"CH32V003 USB Bridge"

/*
 * The serial number is not a literal: it is the factory ESIG unique id (96
 * bits, chapter 15 of the reference manual) rendered as 24 hex characters by
 * serial_from_esig() before enumeration, so every board shows its own.  Only
 * the string itself costs RAM; the descriptor table below stays in flash
 * because it holds a pointer to this buffer rather than to a literal.
 */
#define V003_SERIAL_DESC_SIZE (2 + V003_DEVICE_UID_SIZE * 4)
extern uint8_t v003_serial_descriptor[V003_SERIAL_DESC_SIZE];

struct usb_string_descriptor_struct {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t wString[];
};
const static struct usb_string_descriptor_struct string0 __attribute__((section(".rodata"))) = {
	4,
	3,
	{0x0409}
};
const static struct usb_string_descriptor_struct string1 __attribute__((section(".rodata")))  = {
	sizeof(STR_MANUFACTURER),
	3,
	STR_MANUFACTURER
};
const static struct usb_string_descriptor_struct string2 __attribute__((section(".rodata")))  = {
	sizeof(STR_PRODUCT),
	3,
	STR_PRODUCT
};



// This table defines which descriptor data is sent for each specific
// request from the host (in wValue and wIndex).
const static struct descriptor_list_struct {
	uint32_t	lIndexValue;
	const uint8_t	*addr;
	uint8_t		length;
} descriptor_list[] = {
	{0x00000100, device_descriptor, sizeof(device_descriptor)},
	{0x00000200, config_descriptor, sizeof(config_descriptor)},

	{0x00000300, (const uint8_t *)&string0, 4},
	{0x04090301, (const uint8_t *)&string1, sizeof(STR_MANUFACTURER)},
	{0x04090302, (const uint8_t *)&string2, sizeof(STR_PRODUCT)},
	{0x04090303, (const uint8_t *)v003_serial_descriptor,
	 V003_SERIAL_DESC_SIZE}
};
#define DESCRIPTOR_LIST_ENTRIES ((sizeof(descriptor_list))/(sizeof(struct descriptor_list_struct)) )

#endif // INSTANCE_DESCRIPTORS

#endif

#endif
