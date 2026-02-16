//! spirant-hw-interface
//!
//! Encoder → ParameterValues → OLED display integration firmware for the
//! Raspberry Pi Pico 2. Wires the three existing library crates into a live
//! interactive loop:
//!
//! 1. A rotary encoder is turned.
//! 2. The encoder board fires an interrupt on the INT pin.
//! 3. The encoder monitor task reads the new position, calculates the delta,
//!    and calls `update_from_encoder()` on the shared `ParameterValues` mutex.
//! 4. The OLED display task wakes on its 30 Hz timer, detects the
//!    `changed_oled` flag, builds a new `DisplayState`, and flushes the
//!    updated frame to the screen.
//!
//! No I2C communication with the Daisy Seed is implemented in this stage.

#![no_std]
#![no_main]

use defmt::*;
use embassy_executor::Spawner;
use embassy_rp::block::ImageDef;
use embassy_rp::bind_interrupts;
use embassy_rp::gpio::{Input, Pull};
use embassy_rp::i2c::{self, I2c};
use embassy_rp::peripherals::I2C0;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::mutex::Mutex;
use embassy_embedded_hal::shared_bus::asynch::i2c::I2cDevice;
use static_cell::StaticCell;
use {defmt_rtt as _, panic_probe as _};

use encoder_driver::{QuadEncoderBoard, DEFAULT_ADDRESS};
use spirant::parameter_values::ParameterValues;
use spirant_oled_display_rs::{display_update_task, DisplayConfig, OledDriver};

// ---------------------------------------------------------------------------
// Boot block and interrupt binding
// ---------------------------------------------------------------------------

/// Tell the RP2350 Boot ROM about our application.
#[link_section = ".start_block"]
#[used]
pub static IMAGE_DEF: ImageDef = embassy_rp::block::ImageDef::secure_exe();

// Wire the I2C0 peripheral interrupt to Embassy's async handler.
bind_interrupts!(struct Irqs {
    I2C0_IRQ => i2c::InterruptHandler<I2C0>;
});

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

/// Shared I2C0 bus — both the encoder board and the OLED display access it
/// through I2cDevice wrappers that serialise transactions.
static I2C_BUS: StaticCell<
    Mutex<CriticalSectionRawMutex, I2c<'static, I2C0, i2c::Async>>,
> = StaticCell::new();

/// Shared synthesizer parameter state — written by the encoder task,
/// read by the OLED display task.
static PARAM_VALUES: StaticCell<
    Mutex<CriticalSectionRawMutex, ParameterValues>,
> = StaticCell::new();

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

/// Concrete I2C type for the OLED display, sharing I2C_BUS.
type OledI2c = I2cDevice<
    'static,
    CriticalSectionRawMutex,
    I2c<'static, I2C0, i2c::Async>,
>;

/// Concrete I2C type for the encoder board, sharing I2C_BUS.
type EncoderI2c = I2cDevice<
    'static,
    CriticalSectionRawMutex,
    I2c<'static, I2C0, i2c::Async>,
>;

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

/// Thin wrapper that monomorphises the generic `display_update_task` so it can
/// be spawned as a concrete Embassy task.
#[embassy_executor::task]
async fn oled_task(
    driver: OledDriver<OledI2c>,
    params: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
    config: DisplayConfig,
) {
    display_update_task(driver, params, config).await;
}

/// Interrupt-driven encoder monitoring task.
///
/// Waits for the INT pin to go LOW (active-low from the encoder board),
/// reads all 4 encoder positions, computes deltas against the previous
/// baseline, and writes non-zero deltas into `ParameterValues`. The mutex
/// is held only during the in-memory update — never during I2C operations.
#[embassy_executor::task]
async fn encoder_task(
    mut int_pin: Input<'static>,
    mut encoder_board: QuadEncoderBoard<EncoderI2c>,
    param_values: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
) {
    info!("Encoder monitor task started");

    // Establish a baseline so the first delta calculation is correct.
    // If this read fails we start from [0; 4]; the first interrupt may
    // produce a spurious delta — harmless in practice.
    let mut previous_positions = [0i32; 4];
    match encoder_board.read_all_positions().await {
        Ok(positions) => previous_positions = positions,
        Err(_) => warn!("Could not read initial positions; starting from [0; 4]"),
    }

    loop {
        // wait_for_low() is used instead of wait_for_falling_edge() — confirmed
        // reliable with this encoder board during hardware testing.
        int_pin.wait_for_low().await;

        let positions = match encoder_board.read_all_positions().await {
            Ok(p) => p,
            Err(_) => {
                error!("Encoder read failed");
                // Clear interrupt flags even on error so INT returns HIGH and
                // the next movement produces a fresh interrupt rather than
                // causing the task to spin in a tight error loop.
                let _ = encoder_board.clear_interrupt_flags().await;
                continue;
            }
        };

        // Clear AFTER reading positions — drives INT back HIGH.
        // Clearing before reading would risk missing a rapid second movement
        // that arrives during the I2C read.
        if let Err(_) = encoder_board.clear_interrupt_flags().await {
            warn!("Failed to clear interrupt flags");
        }

        let deltas: [i32; 4] = core::array::from_fn(|i| {
            positions[i] - previous_positions[i]
        });

        // Update baseline unconditionally — tracks hardware state even when
        // all deltas are zero (e.g. spurious power-on interrupt).
        previous_positions = positions;

        if deltas.iter().all(|&d| d == 0) {
            continue;
        }

        // Mutex held only during in-memory updates — never during I2C.
        {
            let mut params = param_values.lock().await;
            for (encoder_idx, &delta) in deltas.iter().enumerate() {
                if delta != 0 {
                    params.update_from_encoder(encoder_idx, delta);
                    debug!(
                        "Encoder {}: delta={}, position={}",
                        encoder_idx, delta, positions[encoder_idx]
                    );
                }
            }
        } // mutex released here
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

#[embassy_executor::main]
async fn main(spawner: Spawner) {
    let p = embassy_rp::init(Default::default());
    info!("spirant-hw-interface starting");

    // —— Pin assignments ————————————————————————————————————————————————————
    // I2C_SDA → GP20  (p.PIN_20)
    // I2C_SCL → GP21  (p.PIN_21)
    // ENC_INT → GP19  (p.PIN_19)  active-low, pull-up enabled
    // ———————————————————————————————————————————————————————————————————————

    // Initialise I2C0, shared between the encoder board and OLED display.
    let i2c = I2c::new_async(
        p.I2C0,
        p.PIN_21, // SCL
        p.PIN_20, // SDA
        Irqs,
        i2c::Config::default(),
    );

    // Wrap in a mutex so both drivers can share the peripheral safely.
    let i2c_bus = I2C_BUS.init(Mutex::new(i2c));

    // Each driver gets its own I2cDevice wrapper. The wrapper acquires the
    // mutex before each I2C transaction and releases it after, serialising
    // bus access automatically.
    let i2c_encoder = I2cDevice::new(i2c_bus);
    let i2c_oled = I2cDevice::new(i2c_bus);

    // Encoder board. DEFAULT_ADDRESS is 0x49 (confirmed by hardware testing;
    // design documents show 0x36 which is incorrect).
    let mut encoder_board = QuadEncoderBoard::new(i2c_encoder, DEFAULT_ADDRESS);

    // OLED display at the standard SSD1306 I2C address.
    let oled_driver = OledDriver::new(i2c_oled, 0x3C);

    // Encoder INT pin: active-low, pull-up enabled.
    let int_pin = Input::new(p.PIN_19, Pull::Up);

    // Initialise shared parameter state.
    let param_values = PARAM_VALUES.init(Mutex::new(ParameterValues::new()));

    // —— Encoder initialisation —————————————————————————————————————————————

    // Read initial positions so the first delta calculation starts from the
    // correct hardware baseline. On failure the encoder task uses [0; 4].
    match encoder_board.read_all_positions().await {
        Ok(positions) => info!(
            "Initial encoder positions: [{}, {}, {}, {}]",
            positions[0], positions[1], positions[2], positions[3]
        ),
        Err(_) => warn!("Could not read initial encoder positions"),
    }

    // Enable hardware interrupts. Without this the INT pin never fires and
    // the encoder task sleeps forever inside wait_for_low(). On failure we
    // log an error and continue — encoder input simply will not work.
    if let Err(_) = encoder_board.enable_all_interrupts().await {
        error!("Failed to enable encoder interrupts");
    }

    // Clear any stale interrupt flags that accumulated at power-on before
    // interrupts were enabled, so INT starts HIGH and clean.
    if let Err(_) = encoder_board.clear_interrupt_flags().await {
        warn!("Failed to clear initial interrupt flags");
    }

    // —— Spawn tasks ————————————————————————————————————————————————————————

    let display_config = DisplayConfig::default(); // 30 Hz refresh rate

    spawner.spawn(oled_task(oled_driver, param_values, display_config)).unwrap();
    spawner.spawn(encoder_task(int_pin, encoder_board, param_values)).unwrap();

    info!("All tasks spawned");
}
