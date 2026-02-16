//! High-level interface for the Adafruit Quad Rotary Encoder Breakout.
//!
//! [`QuadEncoderBoard`] wraps the low-level Seesaw driver with input
//! validation, encoder-specific register addressing, and a batch-read
//! convenience method.

use embedded_hal_async::i2c::I2c;

use crate::driver::SeesawDriver;
use crate::error::EncoderError;
use crate::registers::{ENCODER_COUNT, ENCODER_INT_SET, ENCODER_POSITION, MODULE_ENCODER, MODULE_GPIO, STATUS_INTFLAG};

/// High-level interface for the Adafruit Quad Rotary Encoder Breakout.
///
/// Provides validated, async methods for reading and writing encoder
/// positions over I2C via the Seesaw protocol.
///
/// # Example
///
/// ```no_run
/// use encoder_driver::QuadEncoderBoard;
///
/// // `i2c` is any `embedded-hal-async` I2C implementation
/// let mut board = QuadEncoderBoard::new(i2c, 0x36);
///
/// // Read a single encoder
/// let pos = board.read_position(0).await.unwrap();
///
/// // Read all four encoders at once
/// let positions = board.read_all_positions().await.unwrap();
/// ```
pub struct QuadEncoderBoard<I2C> {
    driver: SeesawDriver<I2C>,
}

impl<I2C> QuadEncoderBoard<I2C>
where
    I2C: I2c,
{
    /// Create a new encoder board interface.
    ///
    /// # Arguments
    /// * `i2c` — I2C peripheral (takes ownership for exclusive access)
    /// * `address` — 7-bit I2C device address (typically 0x36)
    pub fn new(i2c: I2C, address: u8) -> Self {
        Self {
            driver: SeesawDriver::new(i2c, address),
        }
    }

    // -----------------------------------------------------------------------
    // Read operations
    // -----------------------------------------------------------------------

    /// Read the absolute position of a specific encoder.
    ///
    /// The Seesaw firmware internally accumulates encoder ticks as a 32-bit
    /// signed integer with no artificial limits.
    ///
    /// # Arguments
    /// * `encoder` — Encoder index (0–3)
    ///
    /// # Errors
    /// * [`EncoderError::InvalidEncoder`] if `encoder >= 4`
    /// * [`EncoderError::I2c`] on communication failure
    ///
    /// # Example
    /// ```no_run
    /// let position = board.read_position(0).await?;
    /// ```
    pub async fn read_position(
        &mut self,
        encoder: u8,
    ) -> Result<i32, EncoderError<I2C::Error>> {
        if encoder >= ENCODER_COUNT as u8 {
            return Err(EncoderError::InvalidEncoder);
        }

        let register = [MODULE_ENCODER, ENCODER_POSITION | encoder];
        self.driver.read_i32(&register).await
    }

    /// Read all four encoder positions in sequence.
    ///
    /// Performs 4 individual I2C read transactions. While this could
    /// theoretically be optimised with auto-increment reads, the Seesaw
    /// firmware behaviour with auto-increment is not well-documented, so
    /// sequential reads are used for reliability.
    ///
    /// # Returns
    /// An array of positions indexed by encoder number (0–3).
    ///
    /// # Errors
    /// Returns the first I2C error encountered; no partial results are
    /// returned to avoid inconsistent state.
    ///
    /// # Example
    /// ```no_run
    /// let positions = board.read_all_positions().await?;
    /// for (i, pos) in positions.iter().enumerate() {
    ///     println!("Encoder {}: {}", i, pos);
    /// }
    /// ```
    pub async fn read_all_positions(
        &mut self,
    ) -> Result<[i32; 4], EncoderError<I2C::Error>> {
        let mut positions = [0i32; 4];

        for encoder in 0..4u8 {
            positions[encoder as usize] = self.read_position(encoder).await?;
        }

        Ok(positions)
    }

    // -----------------------------------------------------------------------
    // Write operations
    // -----------------------------------------------------------------------

    /// Set the absolute position of a specific encoder.
    ///
    /// Writes a new value to the encoder's internal accumulator. Useful for
    /// resetting encoders to zero or initialising them to match current
    /// parameter values.
    ///
    /// # Arguments
    /// * `encoder` — Encoder index (0–3)
    /// * `value` — New position value
    ///
    /// # Errors
    /// * [`EncoderError::InvalidEncoder`] if `encoder >= 4`
    /// * [`EncoderError::I2c`] on communication failure
    ///
    /// # Example
    /// ```no_run
    /// // Reset encoder 0 to zero
    /// board.set_position(0, 0).await?;
    /// ```
    pub async fn set_position(
        &mut self,
        encoder: u8,
        value: i32,
    ) -> Result<(), EncoderError<I2C::Error>> {
        if encoder >= ENCODER_COUNT as u8 {
            return Err(EncoderError::InvalidEncoder);
        }

        let register = [MODULE_ENCODER, ENCODER_POSITION | encoder];
        self.driver.write_i32(&register, value).await
    }

    // -----------------------------------------------------------------------
    // Interrupt configuration
    // -----------------------------------------------------------------------

    /// Enable the hardware interrupt for a specific encoder.
    ///
    /// Once enabled, the board's INT pin will pulse LOW whenever this
    /// encoder's position changes. The INT pin is active-low and shared
    /// across all four encoders.
    ///
    /// Interrupts are **disabled by default** after power-on — you must
    /// call this (or [`enable_all_interrupts`](Self::enable_all_interrupts))
    /// before `wait_for_falling_edge()` will trigger.
    ///
    /// # Arguments
    /// * `encoder` — Encoder index (0–3)
    ///
    /// # Errors
    /// * [`EncoderError::InvalidEncoder`] if `encoder >= 4`
    /// * [`EncoderError::I2c`] on communication failure
    pub async fn enable_interrupt(
        &mut self,
        encoder: u8,
    ) -> Result<(), EncoderError<I2C::Error>> {
        if encoder >= ENCODER_COUNT as u8 {
            return Err(EncoderError::InvalidEncoder);
        }

        let register = [MODULE_ENCODER, ENCODER_INT_SET | encoder];
        self.driver.write_u8(&register, 1).await
    }

    /// Enable hardware interrupts for all four encoders.
    ///
    /// Convenience method that calls [`enable_interrupt`](Self::enable_interrupt)
    /// for encoders 0–3. The INT pin will pulse LOW when **any** encoder
    /// changes position.
    ///
    /// # Example
    /// ```no_run
    /// board.enable_all_interrupts().await?;
    /// // INT pin will now fire on any encoder movement
    /// ```
    pub async fn enable_all_interrupts(
        &mut self,
    ) -> Result<(), EncoderError<I2C::Error>> {
        for encoder in 0..4u8 {
            self.enable_interrupt(encoder).await?;
        }
        Ok(())
    }

    /// Clear all pending interrupt flags and reset the INT pin.
    ///
    /// The Seesaw STATUS INTFLAG register is read-only and self-clearing:
    /// reading it resets all interrupt flags and drives the INT pin back
    /// HIGH. Call this after reading encoder positions so that the next
    /// encoder movement produces a new falling edge.
    ///
    /// # Example
    /// ```no_run
    /// let positions = board.read_all_positions().await?;
    /// board.clear_interrupt_flags().await?;
    /// ```
    pub async fn clear_interrupt_flags(
        &mut self,
    ) -> Result<(), EncoderError<I2C::Error>> {
        let register = [MODULE_GPIO, STATUS_INTFLAG];
        // Reading the register clears the flags; discard the value.
        let _ = self.driver.read_i32(&register).await?;
        Ok(())
    }
}
