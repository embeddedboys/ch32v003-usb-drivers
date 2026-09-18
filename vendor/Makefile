all : flash

TARGET:=vendor
CH32FUN:=../ch32fun/ch32fun
TARGET_MCU:=CH32V003

ADDITIONAL_C_FILES+=../rv003usb/rv003usb.S ../rv003usb/rv003usb.c ./hexdump.c ./gpio.c ./spi.c ./i2c.c ./frame.c
EXTRA_CFLAGS:=-I../lib -I../rv003usb -Wno-parentheses

# `make DEBUG_EVENTS=1` prints the rv003usb UEvent ring over the debug channel.
# It is off by default: the bit-banged output blocks main(), which then stops
# servicing the requests it owes the host.
ifdef DEBUG_EVENTS
EXTRA_CFLAGS+=-DVENDOR_DEBUG_EVENTS=1
endif

include $(CH32FUN)/ch32fun.mk

flash : cv_flash
clean : cv_clean
