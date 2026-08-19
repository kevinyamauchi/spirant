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
//! Page 0 (Oscillator): [Wave]  [PW]     [--Null--] [--Null--]
//! Page 1 (Filter):     [Res]   [Brite]  [Floor]    [Pass]
//! Page 2 (Overdrive):  [Drive] [Trim]   [--Null--] [--Null--]
//! Page 3 (Chorus):     [Rate]  [Depth]  [Dly]      [FB]
//! Page 4 (Delay):      [Time]  [FB]     [Wet]      [Damp]
//! Page 5 (Reverb):     [Send]  [Size]   [LP]       [--Null--]
//! ```
//!
//! The parameter set mirrors the Daisy **v0** synth (an EWI-style expressive
//! lead); see `spirant-daisy/v0/docs/parameters.md`. Each value is stored as an
//! integer in a real unit (percent, Hz, ms, centi-Hz, or a per-mille
//! coefficient) — see [`PARAM_SPECS`].
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
//! The System Bus target does **not** use `take_i2c_changes()`: it must not
//! clear a flag until the response frame has actually been clocked out to
//! the Daisy, and even then only if the value has not moved again. It
//! serializes with [`crate::wire::serialize_frame`] and clears with
//! [`ParameterValues::clear_i2c_flags_if_unchanged()`]; the MSG line level
//! follows [`ParameterValues::any_changed_i2c()`].
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
pub use parameter::{ParamSpec, Parameter, ParameterSlot};
pub use values::{ParameterChange, ParameterValues};

/// Number of parameter slots per page (matches the number of physical encoders).
pub const PARAMS_PER_PAGE: usize = 4;

/// Number of pages in the parameter system (one per v0 signal-processing stage).
pub const N_PAGES: usize = 6;

/// Human-readable page names for UI display, indexed by page number.
pub const PAGE_NAMES: [&str; N_PAGES] =
    ["Oscillator", "Filter", "Overdrive", "Chorus", "Delay", "Reverb"];

/// Parameter descriptors organized by page and encoder slot.
///
/// `PARAM_SPECS[page][encoder]` is `Some(spec)` for active slots and `None`
/// for null slots. This constant drives the initialization of
/// [`ParameterValues::new()`] — every `Some` becomes an
/// [`Active`](ParameterSlot::Active) slot built via
/// [`Parameter::from_spec`](parameter::Parameter::from_spec), and every `None`
/// becomes [`Null`](ParameterSlot::Null).
///
/// Each spec carries the full `name` (for logs), a short `label` (OLED), the
/// value `[min, max]` range in a real unit, the `default` (the v0 patch value),
/// and the per-detent `step`. Units per page: percent unless noted — Cutoff
/// Floor / Damp LP in Hz, LFO Rate in centi-Hz (×100), Delay Time in ms, and
/// Delay Damping as a per-mille `OnePole` coefficient (×1000).
///
/// **Invariant:** The runtime [`ParameterSlot`] layout must always match this
/// table. Editing specs or null positions here requires no other code changes —
/// `new()` derives the layout automatically.
pub const PARAM_SPECS: [[Option<ParamSpec>; PARAMS_PER_PAGE]; N_PAGES] = [
    // Page 0: Oscillator (2 active)
    [
        Some(ParamSpec::new("Waveshape", "Wave", 0, 100, 15, 1)),
        Some(ParamSpec::new("Pulse Width", "PW", 0, 100, 50, 1)),
        None,
        None,
    ],
    // Page 1: Filter (all 4 slots active)
    [
        Some(ParamSpec::new("Resonance", "Res", 0, 100, 35, 1)),
        Some(ParamSpec::new("Brightness", "Brite", 0, 100, 66, 1)),
        Some(ParamSpec::new("Cutoff Floor", "Floor", 20, 500, 150, 5)),
        Some(ParamSpec::new("Passband Gain", "Pass", 0, 100, 0, 1)),
    ],
    // Page 2: Overdrive (2 active)
    [
        Some(ParamSpec::new("Drive", "Drive", 0, 100, 0, 1)),
        Some(ParamSpec::new("Output Trim", "Trim", 0, 100, 40, 1)),
        None,
        None,
    ],
    // Page 3: Chorus (all 4 slots active)
    [
        Some(ParamSpec::new("LFO Rate", "Rate", 10, 500, 50, 5)),
        Some(ParamSpec::new("LFO Depth", "Depth", 0, 93, 35, 1)),
        Some(ParamSpec::new("Chorus Delay", "Dly", 0, 100, 60, 1)),
        Some(ParamSpec::new("Chorus Feedback", "FB", 0, 100, 20, 1)),
    ],
    // Page 4: Delay (all 4 slots active)
    [
        Some(ParamSpec::new("Delay Time", "Time", 40, 750, 409, 5)),
        Some(ParamSpec::new("Delay Feedback", "FB", 0, 100, 40, 1)),
        Some(ParamSpec::new("Wet Mix", "Wet", 0, 100, 50, 1)),
        Some(ParamSpec::new("Damping", "Damp", 0, 497, 80, 5)),
    ],
    // Page 5: Reverb (3 active, encoder 4 is null)
    [
        Some(ParamSpec::new("Reverb Send", "Send", 0, 100, 75, 1)),
        Some(ParamSpec::new("Reverb Size", "Size", 0, 100, 85, 1)),
        Some(ParamSpec::new("Damp LP", "LP", 1000, 18000, 7000, 250)),
        None,
    ],
];
