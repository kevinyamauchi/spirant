//! Seesaw register address constants for the Adafruit Quad Rotary Encoder Breakout.
//!
//! The Seesaw firmware uses a two-byte register addressing scheme:
//! - Byte 1: Module ID
//! - Byte 2: Register offset within the module
//!
//! For encoder-specific registers, the encoder index (0–3) is OR'd with
//! the base register offset: `[MODULE_ID, BASE_REGISTER | encoder_index]`.

// ---------------------------------------------------------------------------
// Module IDs
// ---------------------------------------------------------------------------

/// Seesaw status module identifier.
pub const MODULE_GPIO: u8 = 0x01;

/// Seesaw encoder module identifier.
pub const MODULE_ENCODER: u8 = 0x11;

// ---------------------------------------------------------------------------
// Status module registers
// ---------------------------------------------------------------------------

/// Interrupt flag register (32-bit, read-only).
/// Reading this register clears all interrupt flags and resets the INT pin.
pub const STATUS_INTFLAG: u8 = 0x0A;

// ---------------------------------------------------------------------------
// Encoder module registers (base addresses)
// ---------------------------------------------------------------------------

/// Base register for reading/writing absolute encoder position (32-bit signed).
/// Per-encoder address: `ENCODER_POSITION | encoder_index`.
pub const ENCODER_POSITION: u8 = 0x30;

/// Base register for reading encoder delta since last read (not used in v1).
#[allow(dead_code)]
pub const ENCODER_DELTA: u8 = 0x40;

/// Register for enabling per-encoder interrupts.
/// Per-encoder address: `ENCODER_INT_SET | encoder_index`.
pub const ENCODER_INT_SET: u8 = 0x10;

/// Register for disabling per-encoder interrupts (not used in v1).
#[allow(dead_code)]
pub const ENCODER_INT_CLR: u8 = 0x20;

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

/// Required delay in microseconds between I2C write and read operations
/// per Seesaw firmware specification.
pub const SEESAW_DELAY_US: u64 = 125;

/// Default I2C address for the Adafruit Quad Rotary Encoder Breakout.
pub const DEFAULT_ADDRESS: u8 = 0x49;

/// Number of rotary encoders on the board.
pub const ENCODER_COUNT: usize = 4;
