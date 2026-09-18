all : flash

TARGET:=vendor
CH32FUN:=../ch32fun/ch32fun
TARGET_MCU:=CH32V003

# Which modules the firmware contains: `make MODULES=gpio,spi` leaves I2C out,
# which frees its code and its 192 bytes of RAM.  The device reports the result
# through V003_GET_CAPABILITIES, so a host never probes a module that is not in
# the build.  Names: gpio spi i2c adc pwm uart wdg pwr (the ones without a source
# file yet are reserved and simply stay off).
MODULES?=gpio,spi,i2c,wdg,pwm,adc,uart
module_flag = -DV003_MODULE_$(1)=$(if $(findstring $(2),$(MODULES)),1,0)
TRACE?=1

ADDITIONAL_C_FILES+=../rv003usb/rv003usb.S ../rv003usb/rv003usb.c ./hexdump.c ./gpio.c ./frame.c
ifneq (,$(findstring spi,$(MODULES)))
ADDITIONAL_C_FILES+=./spi.c
endif
ifneq (,$(findstring i2c,$(MODULES)))
ADDITIONAL_C_FILES+=./i2c.c
endif
ifneq (,$(findstring wdg,$(MODULES)))
ADDITIONAL_C_FILES+=./wdg.c
endif
ifneq (,$(findstring pwm,$(MODULES)))
ADDITIONAL_C_FILES+=./pwm.c
endif
ifneq (,$(findstring adc,$(MODULES)))
ADDITIONAL_C_FILES+=./adc.c
endif
ifneq (,$(findstring uart,$(MODULES)))
ADDITIONAL_C_FILES+=./uart.c
endif

EXTRA_CFLAGS:=-I../lib -I../rv003usb -Wno-parentheses
EXTRA_CFLAGS+=$(call module_flag,GPIO,gpio) $(call module_flag,SPI,spi) \
	      $(call module_flag,I2C,i2c) $(call module_flag,ADC,adc) \
	      $(call module_flag,PWM,pwm) $(call module_flag,UART,uart) \
	      $(call module_flag,WDG,wdg) $(call module_flag,PWR,pwr)
EXTRA_CFLAGS+=-DV003_I2C_TRACER=$(TRACE)

# The rv003usb UEvent ring is a debug capture facility: 16 bytes per entry, 128
# bytes for the default 8.  `make UEVENTS=8` gives a debugging session more room
# at the cost of RAM, `make UEVENTS=4` takes some back (the UART module's
# receive ring comes out of the same 2 kB).
ifdef UEVENTS
EXTRA_CFLAGS+=-DRV003USB_NUMUEVENTS=$(UEVENTS)
endif

# `make DEBUG_EVENTS=1` prints the rv003usb UEvent ring over the debug channel.
# It is off by default: the bit-banged output blocks main(), which then stops
# servicing the requests it owes the host.
ifdef DEBUG_EVENTS
EXTRA_CFLAGS+=-DVENDOR_DEBUG_EVENTS=1
endif

include $(CH32FUN)/ch32fun.mk

flash : cv_flash
clean : cv_clean
