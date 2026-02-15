//! Low-level Seesaw protocol driver.
//!
//! Implements the I2C communication primitives required by the Seesaw firmware,
//! including the mandatory 125µs delay between write and read phases.
//!
//! This module is crate-private — consumers interact with [`QuadEncoderBoard`]
//! in `encoder_board.rs` instead.

use embassy_time::{Duration, Timer};
use embedded_hal_async::i2c::I2c;

use crate::error::EncoderError;
use crate::registers::SEESAW_DELAY_US;

/// Low-level Seesaw protocol driver.
///
/// Owns an I2C peripheral and provides read/write primitives that respect
/// the Seesaw timing requirements.
pub(crate) struct SeesawDriver<I2C> {
    i2c: I2C,
    address: u8,
}

impl<I2C> SeesawDriver<I2C>
where
    I2C: I2c,
{
    /// Create a new Seesaw driver.
    ///
    /// # Arguments
    /// * `i2c` — I2C peripheral (takes ownership for exclusive access)
    /// * `address` — 7-bit I2C device address (typically 0x36)
    pub fn new(i2c: I2C, address: u8) -> Self {
        Self { i2c, address }
    }

    // -----------------------------------------------------------------------
    // Core protocol primitives
    // -----------------------------------------------------------------------

    /// Write a register address, wait the required delay, then read the response.
    ///
    /// This implements the Seesaw protocol timing requirement:
    /// 1. Write register address (2 bytes)
    /// 2. Wait 125µs for firmware to prepare data
    /// 3. Read response bytes
    ///
    /// Uses separate `write()` and `read()` operations rather than `write_read()`
    /// because many I2C implementations use a repeated-start for `write_read()`
    /// which does not allow sufficient delay for the Seesaw firmware.
    async fn write_then_read(
        &mut self,
        register: &[u8],
        buffer: &mut [u8],
    ) -> Result<(), EncoderError<I2C::Error>> {
        // Write register address
        self.i2c.write(self.address, register).await?;

        // Critical delay — Seesaw firmware needs time to prepare response
        Timer::after(Duration::from_micros(SEESAW_DELAY_US)).await;

        // Read response
        self.i2c.read(self.address, buffer).await?;

        Ok(())
    }

    // -----------------------------------------------------------------------
    // Typed read/write helpers
    // -----------------------------------------------------------------------

    /// Read a 32-bit signed integer from a register.
    ///
    /// Reads 4 bytes and converts from big-endian (Seesaw byte order) to
    /// the native little-endian representation of the RP2350.
    pub async fn read_i32(
        &mut self,
        register: &[u8],
    ) -> Result<i32, EncoderError<I2C::Error>> {
        let mut buf = [0u8; 4];
        self.write_then_read(register, &mut buf).await?;
        Ok(i32::from_be_bytes(buf))
    }

    /// Write a 32-bit signed integer to a register.
    ///
    /// Converts `value` to big-endian bytes and sends them together with the
    /// 2-byte register address in a single I2C write transaction.
    pub async fn write_i32(
        &mut self,
        register: &[u8],
        value: i32,
    ) -> Result<(), EncoderError<I2C::Error>> {
        let bytes = value.to_be_bytes();

        // Full write buffer: [register_hi, register_lo, b3, b2, b1, b0]
        let mut buf = [0u8; 6];
        buf[0..2].copy_from_slice(register);
        buf[2..6].copy_from_slice(&bytes);

        self.i2c.write(self.address, &buf).await?;

        Ok(())
    }

    /// Write a single byte to a register.
    pub async fn write_u8(
        &mut self,
        register: &[u8],
        value: u8,
    ) -> Result<(), EncoderError<I2C::Error>> {
        let mut buf = [0u8; 3];
        buf[0..2].copy_from_slice(register);
        buf[2] = value;

        self.i2c.write(self.address, &buf).await?;

        Ok(())
    }
}
