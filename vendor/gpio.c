#include "ch32fun.h"
#include "rv003usb.h"

#include "gpio.h"

void handle_gpio_out_request(u16 cmd, u16 data)
{
	u8 state = data & 0xff;
	u8 idx = data >> 8;

	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_GPIO_SET:
		funDigitalWrite(idx, state);
		break;
	case V003_GPIO_REQUEST:
	case V003_GPIO_FREE:
	case V003_GPIO_DIRECTION_INPUT:
		funPinMode(idx, GPIO_Speed_In | GPIO_CNF_IN_PUPD);
		break;
	case V003_GPIO_DIRECTION_OUTPUT:
		funPinMode(idx, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);
		funDigitalWrite(idx, state);
		break;
	default:
		break;
	}
}

// #define funPinMode( pin, mode ) { GpioOf(pin)->CFGLR = (GpioOf(pin)->CFGLR & (~(0xf<<(4*((pin)&0xf))))) | ((mode)<<(4*((pin)&0xf))); }
#define funPinGetMode(pin) ((GpioOf(pin)->CFGLR >> (4 * ((pin) & 0xF))) & 0xF)
u32 handle_gpio_in_request(u16 cmd, u16 data)
{
	u8 idx = data >> 8;
	u32 state = 0;

	LogUEvent(SysTick->CNT, cmd, data, 0);

	switch (cmd) {
	case V003_GPIO_GET:
		state = funDigitalRead(idx);
		break;
	case V003_GPIO_GET_DIRECTION:
		state = (funPinGetMode(idx) & 0x03) == 0x00 ? 1 : 0;
		break;
	default:
		break;
	}

	return state;
}
