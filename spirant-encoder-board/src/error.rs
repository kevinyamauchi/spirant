//! Error types for the encoder driver.

use core::fmt;

/// Errors that can occur when communicating with the encoder board.
#[derive(Debug)]
pub enum EncoderError<E> {
    /// Underlying I2C bus error.
    I2c(E),

    /// Encoder index out of valid range (must be 0–3).
    InvalidEncoder,
}

// Allow ergonomic `?` propagation from raw I2C errors.
impl<E> From<E> for EncoderError<E> {
    fn from(error: E) -> Self {
        EncoderError::I2c(error)
    }
}

impl<E: fmt::Debug> fmt::Display for EncoderError<E> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            EncoderError::I2c(e) => write!(f, "I2C error: {:?}", e),
            EncoderError::InvalidEncoder => write!(f, "Invalid encoder index (must be 0-3)"),
        }
    }
}

#[cfg(feature = "defmt")]
impl<E: defmt::Format> defmt::Format for EncoderError<E> {
    fn format(&self, f: defmt::Formatter) {
        match self {
            EncoderError::I2c(e) => defmt::write!(f, "I2C error: {}", e),
            EncoderError::InvalidEncoder => defmt::write!(f, "Invalid encoder index"),
        }
    }
}
