use rusb::Context;
use std::thread;
use std::time::Duration;

use crate::usb_peri_bridge::gpio_pins::PC0;
use crate::usb_peri_bridge::UsbGpio;

mod usb_peri_bridge {
    use std::time::Duration;

    use rusb::{DeviceHandle, Direction, Recipient, RequestType, UsbContext};

    #[allow(dead_code)]
    pub mod gpio_pins {
        pub const PA1: u8 = 1;
        pub const PA2: u8 = 2;
        pub const PC0: u8 = 32;
        pub const PC1: u8 = 33;
        pub const PC2: u8 = 34;
        pub const PC3: u8 = 35;
        pub const PC4: u8 = 36;
        pub const PC5: u8 = 37;
        pub const PC6: u8 = 38;
        pub const PC7: u8 = 39;
        pub const PD0: u8 = 48;
        pub const PD1: u8 = 49;
        pub const PD2: u8 = 50;
        pub const PD3: u8 = 51;
        pub const PD4: u8 = 52;
        pub const PD5: u8 = 53;
        pub const PD6: u8 = 54;
        pub const PD7: u8 = 55;
    }

    const TYPE_VENDOR_OUT: u8 =
        rusb::request_type(Direction::Out, RequestType::Vendor, Recipient::Device);
    const TYPE_VENDOR_IN: u8 =
        rusb::request_type(Direction::In, RequestType::Vendor, Recipient::Device);

    pub struct UsbGpio<T: UsbContext> {
        handle: DeviceHandle<T>,
    }

    impl<T: UsbContext> UsbGpio<T> {
        pub fn open(vid: u16, pid: u16, context: &T) -> Result<Self, rusb::Error> {
            let handle = context
                .open_device_with_vid_pid(vid, pid)
                .expect("Error no such device!");

            if handle.kernel_driver_active(0).unwrap_or(false) {
                handle.detach_kernel_driver(0)?;
            }

            Ok(UsbGpio { handle })
        }

        #[allow(dead_code)]
        pub fn new(handle: DeviceHandle<T>) -> Self {
            UsbGpio { handle }
        }

        pub fn set_gpio(&mut self, idx: u8, state: u8) -> rusb::Result<()> {
            _ = self.handle.write_control(
                TYPE_VENDOR_OUT,
                0x00,
                u16::from_le_bytes([state, idx]),
                0x106,
                &[],
                Duration::from_millis(100),
            );
            Ok(())
        }

        pub fn get_gpio(&mut self, idx: u8) -> rusb::Result<u8> {
            let mut data: [u8; 1] = [0];
            _ = self.handle.read_control(
                TYPE_VENDOR_IN,
                0x00,
                u16::from_le_bytes([0x00, idx]),
                0x107,
                &mut data,
                Duration::from_millis(100),
            );
            Ok(data[0])
        }
    }
}

fn main() -> rusb::Result<()> {
    println!("Hello, world!");

    let context = Context::new()?;

    let vid = 0x1209;
    let pid = 0xc303;

    let mut gpio = UsbGpio::open(vid, pid, &context)?;

    loop {
        gpio.set_gpio(PC0, 1)?;
        println!("gpio {}, state : {}", PC0, gpio.get_gpio(PC0)?);

        thread::sleep(Duration::from_millis(500));

        gpio.set_gpio(PC0, 0)?;
        println!("gpio {}, state : {}", PC0, gpio.get_gpio(PC0)?);

        thread::sleep(Duration::from_millis(500));
    }
}
