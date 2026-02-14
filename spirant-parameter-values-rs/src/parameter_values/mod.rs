//! Synthesizer parameter management with page-based organization.
//!
//! This module provides the [`ParameterValues`] data structure that manages
//! synthesizer parameter state across multiple UI pages. It is the central
//! shared state accessed by the encoder monitor, OLED display, and I2C
//! communication tasks.
//!
//! # Architecture
//!
//! Parameters are organized into **pages** of up to 4 parameters each,
//! matching the 4 physical rotary encoders. Each page represents a group
//! of related synthesis parameters (e.g., Filter, Envelope, LFO, Effects).
//!
//! ```text
//! Page 0 (Filter):   [Cutoff] [Resonance] [Filter Type] [Filter Env]
//! Page 1 (Envelope): [Attack] [Decay]     [Sustain]     [Release]
//! Page 2 (LFO):      [Rate]   [Depth]     [Shape]       [---Null---]
//! Page 3 (Effects):  [Delay]  [Reverb]    [---Null---]  [---Null---]
//! ```
//!
//! # Change Tracking
//!
//! Each parameter carries two independent change flags:
//!
//! - **`changed_oled`** — set when the OLED display needs to redraw this
//!   parameter. Set by encoder changes, I2C writes, and page switches.
//! - **`changed_i2c`** — set when the value should be sent to the Daisy
//!   Seed over I2C. Set only by encoder changes (never by I2C writes,
//!   to prevent echo loops).
//!
//! Consumers call [`ParameterValues::take_oled_changes()`] or
//! [`ParameterValues::take_i2c_changes()`] to atomically read and clear
//! their respective flags.
//!
//! # `no_std` Compatibility
//!
//! This module uses no heap allocation. All storage is fixed-size arrays
//! sized by the [`N_PAGES`] and [`PARAMS_PER_PAGE`] constants. The
//! optional `defmt` feature enables structured logging for embedded targets.

mod error;
mod page;
mod parameter;
mod values;

pub use error::ParameterError;
pub use page::Page;
pub use parameter::{Parameter, ParameterSlot};
pub use values::{ParameterChange, ParameterValues};

/// Number of parameter slots per page (matches the number of physical encoders).
pub const PARAMS_PER_PAGE: usize = 4;

/// Number of pages in the parameter system.
pub const N_PAGES: usize = 4;

/// Human-readable page names for UI display, indexed by page number.
pub const PAGE_NAMES: [&str; N_PAGES] = ["Filter", "Envelope", "LFO", "Effects"];

/// Parameter names organized by page and encoder slot.
///
/// `PARAM_NAMES[page][encoder]` is `Some("Name")` for active slots and
/// `None` for null slots. This constant drives the initialization of
/// [`ParameterValues::new()`] — every `Some` becomes an
/// [`Active`](ParameterSlot::Active) slot and every `None` becomes
/// [`Null`](ParameterSlot::Null).
///
/// **Invariant:** The runtime [`ParameterSlot`] layout must always match
/// this table. Modifying parameter names or null positions here requires
/// no other code changes — `new()` derives the layout automatically.
pub const PARAM_NAMES: [[Option<&str>; PARAMS_PER_PAGE]; N_PAGES] = [
    // Page 0: Filter (all 4 slots active)
    [
        Some("Cutoff"),
        Some("Resonance"),
        Some("Filter Type"),
        Some("Filter Env"),
    ],
    // Page 1: Envelope (all 4 slots active)
    [
        Some("Attack"),
        Some("Decay"),
        Some("Sustain"),
        Some("Release"),
    ],
    // Page 2: LFO (3 active, encoder 4 is null)
    [
        Some("LFO Rate"),
        Some("LFO Depth"),
        Some("LFO Shape"),
        None,
    ],
    // Page 3: Effects (2 active, encoders 3 & 4 are null)
    [Some("Delay Time"), Some("Reverb"), None, None],
];
