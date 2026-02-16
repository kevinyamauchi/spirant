//! Async OLED display driver for the SSD1306 (128×64) using Embassy.
//!
//! This crate provides [`OledDriver`], a wrapper around the [`ssd1306`]
//! crate in async buffered-graphics mode, and [`display_update_task`], a
//! periodic update loop that reads [`ParameterValues`] and renders the
//! active page to the display.
//!
//! # Quick Start
//!
//! ```ignore
//! use spirant_oled_display_rs::{OledDriver, DisplayConfig, display_update_task};
//!
//! // In your Embassy main:
//! let oled = OledDriver::new(i2c_oled, 0x3C);
//! let config = DisplayConfig::default();
//! spawner.spawn(oled_task(oled, param_values, config)).unwrap();
//!
//! // Thin task wrapper (Embassy tasks cannot be generic):
//! #[embassy_executor::task]
//! async fn oled_task(
//!     driver: OledDriver<MyI2cType>,
//!     params: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
//!     config: DisplayConfig,
//! ) {
//!     display_update_task(driver, params, config).await;
//! }
//! ```
//!
//! # Crate Features
//!
//! - **`defmt`** *(default)* — structured logging via [`defmt`].
//!
//! [`ParameterValues`]: spirant::parameter_values::ParameterValues

#![no_std]

#[cfg(feature = "task")]
pub mod display_task;
pub mod driver;
pub mod error;
pub mod layout;

// ── Re-exports for convenience ───────────────────────────────────────────

#[cfg(feature = "task")]
pub use display_task::display_update_task;
pub use driver::OledDriver;
pub use error::OledError;
pub use layout::{display_state_changed, DisplayChanges, DisplayConfig, DisplayState};
