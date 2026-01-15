# CH32V003 USB Drivers

This project is based on [rv003usb](https://github.com/cnlohr/rv003usb).

| 硬件信息   |            |                                         |
| ---------- | ---------- | --------------------------------------- |
| MCU        | CH32V003   | 青稞32位RISC-V内核，RV32EC指令集 @48MHz |
| SRAM       | 2 KB       | 易失数据存储区                          |
| Bootloader | 1920 Bytes | 系统引导程序存储区                      |
| Flash      | 16 KB      | 程序存储区                              |
| USB_DP(D+) | PD3        |                                         |
| USB_DM(D-) | PD4        |                                         |
| DPU        | PD5        | 用于重新触发USB枚举                     |
| BOOT Btn   | PD6        | 用于在BootLoader阶段检测是否要烧录程序  |

## Getting Started

## Reference

- [rv003usb](https://github.com/cn)
- [Programming with PyUSB 1.0](https://github.com/pyusb/pyusb/blob/master/docs/tutorial.rst)
