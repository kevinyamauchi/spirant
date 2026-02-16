//! Error types for the OLED display driver.

use display_interface::DisplayError;

/// Errors that can occur during OLED display operations.
///
/// The `ssd1306` crate wraps all underlying I2C bus errors into
/// [`DisplayError`], so this enum is non-generic.
#[derive(Debug)]
pub enum OledError {
    /// Display interface error (wraps I2C and other bus-level failures).
    Display(DisplayError),
    /// Display hardware did not respond to initialisation.
    InitializationFailed,
    /// An operation was attempted before [`OledDriver::init()`](crate::OledDriver::init)
    /// was called.
    NotInitialized,
}

impl From<DisplayError> for OledError {
    fn from(e: DisplayError) -> Self {
        OledError::Display(e)
    }
}

#[cfg(feature = "defmt")]
impl defmt::Format for OledError {
    fn format(&self, f: defmt::Formatter) {
        match self {
            OledError::Display(_e) => defmt::write!(f, "Display interface error"),
            OledError::InitializationFailed => defmt::write!(f, "Initialization failed"),
            OledError::NotInitialized => defmt::write!(f, "Not initialized"),
        }
    }
}
