//! Async driver for the Adafruit Quad Rotary Encoder Breakout.
//!
//! This crate provides an Embassy-compatible async I2C driver for the
//! Adafruit Seesaw-based Quad Rotary Encoder Breakout board (Product #5752).
//!
//! # Architecture
//!
//! The crate is split into two layers:
//!
//! - **`driver`** (crate-private) — Low-level Seesaw protocol primitives that
//!   handle I2C timing, endianness, and register addressing.
//! - **[`QuadEncoderBoard`]** (public) — Validated, high-level API for reading
//!   and writing encoder positions.
//!
//! # Quick start
//!
//! ```no_run
//! use encoder_driver::QuadEncoderBoard;
//!
//! // Construct with any `embedded-hal-async` I2C implementation
//! let mut board = QuadEncoderBoard::new(i2c, 0x36);
//!
//! // Read all four encoder positions
//! let positions = board.read_all_positions().await?;
//! ```
//!
//! # Features
//!
//! - **`defmt`** — Enable [`defmt::Format`] implementations on error types
//!   for embedded logging.

#![no_std]

pub use encoder_board::QuadEncoderBoard;
pub use error::EncoderError;
pub use registers::{DEFAULT_ADDRESS, ENCODER_COUNT};

mod driver;
mod encoder_board;
mod error;
mod registers;
