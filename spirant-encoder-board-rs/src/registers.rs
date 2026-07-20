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
// GPIO module registers (for the encoder push-buttons)
// ---------------------------------------------------------------------------
//
// The four encoder push-buttons are GPIO inputs on the Seesaw (port A). They
// are configured as `INPUT_PULLUP` and read via the bulk register. We do NOT
// enable GPIO interrupts — the buttons are polled — so button presses stay off
// the shared INT line and the rotation interrupt path is unaffected.

/// Set selected GPIO pins as inputs (write bitmask).
pub const GPIO_DIRCLR_BULK: u8 = 0x03;

/// Bulk GPIO state register (read): each bit is the level of the matching pin.
pub const GPIO_BULK: u8 = 0x04;

/// Set output-register bits high (write bitmask). With pulls enabled this
/// selects pull-*up* direction.
pub const GPIO_BULK_SET: u8 = 0x05;

/// Enable internal pull resistors on selected GPIO pins (write bitmask).
pub const GPIO_PULLENSET: u8 = 0x0B;

/// Seesaw GPIO pin number for each encoder's push-button switch, indexed by
/// encoder (0–3). Confirmed against Adafruit's Quad Rotary Encoder example
/// (`SS_ENC{0..3}_SWITCH`). All are on port A, so a single [`GPIO_BULK`] read
/// covers them and `1 << pin` masking is valid.
pub const SWITCH_PINS: [u8; ENCODER_COUNT] = [12, 14, 17, 9];

/// Bitmask of all four switch pins, for the bulk config writes.
pub const SWITCH_MASK: u32 =
    (1 << SWITCH_PINS[0]) | (1 << SWITCH_PINS[1]) | (1 << SWITCH_PINS[2]) | (1 << SWITCH_PINS[3]);

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
