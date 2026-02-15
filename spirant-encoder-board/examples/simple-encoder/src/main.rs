//! Simple encoder example
//!
//! Demonstrates basic usage of the encoder-driver crate on the Raspberry Pi
//! Pico 2. Waits for the encoder board's INT pin to fire, reads all four
//! encoder positions, and logs them via defmt.
//!
//! # Wiring
//!
//! | Signal    | Pico 2 Pin | Notes                        |
//! |-----------|------------|------------------------------|
//! | I2C0 SDA  | GP4        |                              |
//! | I2C0 SCL  | GP5        |                              |
//! | ENC INT   | GP6        | Active-low, pull-up enabled  |

#![no_std]
#![no_main]

use defmt::*;
use embassy_time::{Duration, Timer};
use embassy_executor::Spawner;
use embassy_rp as hal;
use embassy_rp::block::ImageDef;
use embassy_rp::gpio::{Input, Pull};
use embassy_rp::i2c::{self, I2c};
use embassy_rp::peripherals::I2C0;
use embassy_rp::bind_interrupts;
use {defmt_rtt as _, panic_probe as _};

use encoder_driver::{QuadEncoderBoard, DEFAULT_ADDRESS};

/// Tell the Boot ROM about our application.
#[link_section = ".start_block"]
#[used]
pub static IMAGE_DEF: ImageDef = hal::block::ImageDef::secure_exe();

// Wire the I2C0 interrupt to Embassy's handler.
bind_interrupts!(struct Irqs {
    I2C0_IRQ => i2c::InterruptHandler<I2C0>;
});

#[embassy_executor::main]
async fn main(_spawner: Spawner) {
    let p = embassy_rp::init(Default::default());

    // --- I2C bus (GP4 = SDA, GP5 = SCL) ---
    let i2c = I2c::new_async(
        p.I2C0,
        p.PIN_21, // SCL
        p.PIN_20, // SDA
        Irqs,
        i2c::Config::default(),
    );

    // --- Encoder board INT pin (GP19, active-low) ---
    let mut int_pin = Input::new(p.PIN_19, Pull::Up);

    // --- Encoder board ---
    let mut encoder_board = QuadEncoderBoard::new(i2c, DEFAULT_ADDRESS);

    // Read once at startup so we have a baseline in the log.
    match encoder_board.read_all_positions().await {
        Ok(positions) => {
            info!(
                "Initial positions: [{}, {}, {}, {}]",
                positions[0], positions[1], positions[2], positions[3],
            );
        }
        Err(e) => error!("Initial read failed: {}", e),
    }

    Timer::after(Duration::from_millis(1000)).await;

    // Enable interrupts so the INT pin fires on encoder movement.
    encoder_board
        .enable_all_interrupts()
        .await
        .expect("Failed to enable encoder interrupts");

    if let Err(e) = encoder_board.clear_interrupt_flags().await {
        error!("Failed to clear interrupt flags: {}", e);
    }

    info!("Encoder example started — rotate knobs to see position changes");


    // Main loop: sleep until interrupt, read, log, repeat.
    loop {
        int_pin.wait_for_low().await;

        match encoder_board.read_all_positions().await {
            Ok(positions) => {
                info!(
                    "Positions: [{}, {}, {}, {}]",
                    positions[0], positions[1], positions[2], positions[3],
                );
            }
            Err(e) => error!("Read failed: {}", e),
        }

        // Clear interrupt flags so INT goes back HIGH and the next
        // encoder movement produces a fresh falling edge.
        if let Err(e) = encoder_board.clear_interrupt_flags().await {
            error!("Failed to clear interrupt flags: {}", e);
        }

    }
}
