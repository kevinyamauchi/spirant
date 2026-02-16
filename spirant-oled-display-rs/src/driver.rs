//! Core OLED driver wrapping the `ssd1306` crate in async buffered graphics mode.
//!
//! [`OledDriver`] manages the SSD1306 display lifecycle: construction without
//! I2C traffic, explicit async initialisation, and frame buffer flush.

use display_interface_i2c::I2CInterface;
use embedded_hal_async::i2c::I2c;
use ssd1306::{
    mode::BufferedGraphicsModeAsync, prelude::*, I2CDisplayInterface, Ssd1306Async,
};

use crate::error::OledError;

/// Concrete display type used internally by [`OledDriver`].
type Display<I2C> = Ssd1306Async<
    I2CInterface<I2C>,
    DisplaySize128x64,
    BufferedGraphicsModeAsync<DisplaySize128x64>,
>;

/// Async driver for an SSD1306 128×64 OLED display over I2C.
///
/// Wraps the [`ssd1306`] crate in `BufferedGraphicsMode`, providing a
/// high-level interface for initialisation, rendering, and flushing.
///
/// # Lifecycle
///
/// 1. [`OledDriver::new()`] — constructs the driver without any I2C traffic.
/// 2. [`OledDriver::init()`] — sends the SSD1306 initialisation sequence.
/// 3. Draw into the frame buffer via [`OledDriver::display_mut()`].
/// 4. [`OledDriver::flush()`] — transfers the frame buffer to hardware.
///
/// # Example
///
/// ```no_run
/// use spirant_oled_display_rs::OledDriver;
///
/// # async fn example(i2c: impl embedded_hal_async::i2c::I2c) {
/// let mut oled = OledDriver::new(i2c, 0x3C);
/// oled.init().await.unwrap();
/// oled.clear_buffer();
/// oled.flush().await.unwrap();
/// # }
/// ```
pub struct OledDriver<I2C> {
    /// The underlying ssd1306 display. `Some` after construction; provides
    /// the not-initialised guard for `flush()` and `display_mut()`.
    display: Option<Display<I2C>>,
    /// Set to `true` after a successful `init()` call.
    initialized: bool,
}

impl<I2C> OledDriver<I2C>
where
    I2C: I2c,
{
    /// Construct an uninitialised driver.
    ///
    /// No I2C traffic is generated. You **must** call [`init()`](Self::init)
    /// before any display operations.
    ///
    /// # Arguments
    /// * `i2c` — I2C peripheral (takes ownership for exclusive access).
    /// * `address` — 7-bit I2C device address (typically `0x3C` or `0x3D`).
    pub fn new(i2c: I2C, address: u8) -> Self {
        let interface = I2CDisplayInterface::new_custom_address(i2c, address);
        let display =
            Ssd1306Async::new(interface, DisplaySize128x64, DisplayRotation::Rotate0)
                .into_buffered_graphics_mode();

        Self {
            display: Some(display),
            initialized: false,
        }
    }

    /// Initialise the SSD1306 hardware.
    ///
    /// Sends the display initialisation command sequence over I2C. Must be
    /// called exactly once before any rendering or flush operations.
    ///
    /// Sets `initialized = true` on success.
    ///
    /// # Errors
    ///
    /// Returns [`OledError::InitializationFailed`] if the display does not
    /// respond, or [`OledError::Display`] on a bus-level failure.
    pub async fn init(&mut self) -> Result<(), OledError> {
        if let Some(ref mut display) = self.display {
            display
                .init()
                .await
                .map_err(|_| OledError::InitializationFailed)?;
            self.initialized = true;
            Ok(())
        } else {
            Err(OledError::NotInitialized)
        }
    }

    /// Clear the in-memory frame buffer.
    ///
    /// Does **not** send any I2C traffic — the display is unchanged until
    /// [`flush()`](Self::flush) is called. Safe to call even if the driver
    /// is not yet initialised (no-op in that case).
    pub fn clear_buffer(&mut self) {
        if let Some(ref mut display) = self.display {
            display.clear_buffer();
        }
    }

    /// Transfer the frame buffer to the display via I2C.
    ///
    /// At 400 kHz I2C this takes approximately 20 ms for a full 1024-byte
    /// frame.
    ///
    /// # Errors
    ///
    /// Returns [`OledError::NotInitialized`] if [`init()`](Self::init) has
    /// not been called, or [`OledError::Display`] on a bus-level failure.
    pub async fn flush(&mut self) -> Result<(), OledError> {
        if !self.initialized {
            return Err(OledError::NotInitialized);
        }
        if let Some(ref mut display) = self.display {
            display.flush().await?;
            Ok(())
        } else {
            Err(OledError::NotInitialized)
        }
    }

    /// Returns a mutable reference to the underlying `ssd1306` display,
    /// allowing direct use of `embedded-graphics` [`DrawTarget`] APIs.
    ///
    /// Returns `None` if the driver has not been initialised.
    ///
    /// [`DrawTarget`]: embedded_graphics::draw_target::DrawTarget
    pub fn display_mut(&mut self) -> Option<&mut Display<I2C>> {
        if self.initialized {
            self.display.as_mut()
        } else {
            None
        }
    }

    /// Check whether the display has been successfully initialised.
    ///
    /// No I2C traffic is generated.
    pub fn is_initialized(&self) -> bool {
        self.initialized
    }
}
