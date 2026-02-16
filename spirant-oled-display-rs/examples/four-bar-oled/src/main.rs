//! Progress bar example
//!
//! Standalone hardware demonstration that exercises [`OledDriver`] directly
//! — no `ParameterValues`, no encoder driver. Renders 4 horizontal progress
//! bars and cycles their values automatically, verifying that the display
//! initialises and renders correctly on real hardware.
//!
//! # Wiring
//!
//! | Signal    | Pico 2 Pin | Notes           |
//! |-----------|------------|-----------------|
//! | I2C0 SDA  | GP4        |                 |
//! | I2C0 SCL  | GP5        |                 |
//! | OLED VCC  | 3V3        |                 |
//! | OLED GND  | GND        |                 |
//!
//! # Progress Bar Geometry
//!
//! | Property        | Value                                          |
//! |-----------------|------------------------------------------------|
//! | Bar row height  | 16 px (4 bars × 16 = 64 px = full display)    |
//! | Bar fill height | 14 px (1 px margin top and bottom per row)     |
//! | Bar x origin    | 0                                              |
//! | Bar y origin    | `i * 16 + 1`                                   |
//! | Bar pixel width | `value * 128 / 127`                            |
//! | Value range     | 0–127                                          |

#![no_std]
#![no_main]

use defmt::*;
use embassy_executor::Spawner;
use embassy_rp as hal;
use embassy_rp::bind_interrupts;
use embassy_rp::block::ImageDef;
use embassy_rp::i2c::{self, I2c};
use embassy_rp::peripherals::I2C0;
use embassy_time::{Duration, Timer};
use {defmt_rtt as _, panic_probe as _};

use embedded_graphics::pixelcolor::BinaryColor;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{PrimitiveStyle, Rectangle};

use spirant_oled_display_rs::OledDriver;

/// Tell the Boot ROM about our application.
#[link_section = ".start_block"]
#[used]
pub static IMAGE_DEF: ImageDef = hal::block::ImageDef::secure_exe();

// Wire the I2C0 interrupt to Embassy's handler.
bind_interrupts!(struct Irqs {
    I2C0_IRQ => i2c::InterruptHandler<I2C0>;
});

// ---------------------------------------------------------------------------
// Hardware pin assignments — change here for hardware revisions
// I2C bus is used exclusively by the OLED display in this example
// ---------------------------------------------------------------------------
// I2C_SDA → GP4  (p.PIN_4)
// I2C_SCL → GP5  (p.PIN_5)
// ---------------------------------------------------------------------------

#[embassy_executor::main]
async fn main(_spawner: Spawner) {
    let p = embassy_rp::init(Default::default());
    info!("Progress bar example starting");

    // --- I2C bus (GP4 = SDA, GP5 = SCL) ---
    let i2c = I2c::new_async(
        p.I2C0,
        p.PIN_21, // SCL
        p.PIN_20, // SDA
        Irqs,
        i2c::Config::default(),
    );

    let mut oled = OledDriver::new(i2c, 0x3C);

    oled.init().await.expect("OLED init failed");
    info!("OLED initialised");

    // Bar values 0–127, initialised offset so bars are visually distinct.
    let mut values: [u8; 4] = [0, 32, 64, 96];

    loop {
        oled.clear_buffer();

        if let Some(display) = oled.display_mut() {
            for (i, &v) in values.iter().enumerate() {
                let y = (i as i32) * 16;
                let bar_width = (v as u32) * 128 / 127;

                // Draw filled rectangle for bar.
                Rectangle::new(Point::new(0, y + 1), Size::new(bar_width, 14))
                    .into_styled(PrimitiveStyle::with_fill(BinaryColor::On))
                    .draw(display)
                    .ok();
            }
        }

        oled.flush().await.ok();

        // Increment all bars, wrapping at 127.
        for v in values.iter_mut() {
            *v = (*v + 1) % 128;
        }

        Timer::after(Duration::from_millis(33)).await; // ~30 Hz
    }
}
