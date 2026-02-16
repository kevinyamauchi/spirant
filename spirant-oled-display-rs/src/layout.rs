//! Display layout types and rendering logic.
//!
//! This module defines the immutable [`DisplayState`] snapshot, the
//! [`DisplayChanges`] diff, and the [`render_display`] function that draws
//! a frame using `embedded-graphics`.

use core::fmt::Write;

use embedded_graphics::{
    mono_font::{ascii::FONT_6X10, MonoTextStyle},
    pixelcolor::BinaryColor,
    prelude::*,
    text::{Alignment, Text},
};
use heapless::String;


// ── DisplayConfig ────────────────────────────────────────────────────────

/// Configuration for the display layout and update task.
///
/// All layout geometry lives here — there are **no** module-level layout
/// constants. Callers can tune every dimension at construction time
/// without modifying library source.
///
/// [`DisplayConfig::default()`] reproduces the original design geometry
/// (128×64, 4 × 32 px columns, 60 Hz).
pub struct DisplayConfig {
    /// Display refresh rate in Hz. Default: 30. Max: 60.
    pub update_frequency_hz: u32,

    // ── Layout geometry ──────────────────────────────────────────────
    /// Total display width in pixels. Default: 128.
    pub display_width: u32,
    /// Total display height in pixels. Default: 64.
    pub display_height: u32,
    /// Width of each parameter column in pixels. Default: 32.
    pub column_width: u32,
    /// Height reserved for the page-name header at the top. Default: 12.
    pub header_height: u32,
    /// Y coordinate (pixels from top) for parameter name text. Default: 24.
    pub param_name_y: i32,
    /// Y coordinate (pixels from top) for parameter value text. Default: 40.
    pub param_value_y: i32,
}

impl Default for DisplayConfig {
    fn default() -> Self {
        Self {
            update_frequency_hz: 60,
            display_width: 128,
            display_height: 64,
            column_width: 32,
            header_height: 12,
            param_name_y: 24,
            param_value_y: 40,
        }
    }
}

impl DisplayConfig {
    /// Convert the configured frequency to a timer period in milliseconds.
    ///
    /// Formula: `1000 / update_frequency_hz`.
    pub fn update_period_ms(&self) -> u64 {
        1000 / self.update_frequency_hz as u64
    }
}

// ── DisplayState ─────────────────────────────────────────────────────────

/// Immutable snapshot of everything the display needs to render one frame.
///
/// Fixed-size arrays avoid heap allocation. Strings are stored as
/// null-padded UTF-8 byte buffers with a maximum length of 15 usable
/// characters (the 16th byte is always `\0`).
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct DisplayState {
    /// Page name, null-padded UTF-8 (max 15 chars).
    pub page_name: [u8; 16],
    /// Parameter names, null-padded UTF-8, one per column.
    pub param_names: [[u8; 16]; 4],
    /// Parameter values. `None` indicates a null slot (blank column).
    pub param_values: [Option<i32>; 4],
}

impl DisplayState {
    /// Construct from live parameter data.
    ///
    /// Strings are copied into fixed-size buffers and silently truncated
    /// if longer than 15 characters.
    ///
    /// # Arguments
    ///
    /// * `page_name` — Name of the active page.
    /// * `param_names` — Display names for each encoder slot (`None` = null slot).
    /// * `param_values` — Current values (`None` = null slot, blank column).
    pub fn from_params(
        page_name: &str,
        param_names: [Option<&str>; 4],
        param_values: [Option<i32>; 4],
    ) -> Self {
        let mut state = Self::default();

        // Copy page name (truncate to 15 bytes).
        let name_bytes = page_name.as_bytes();
        let len = name_bytes.len().min(15);
        state.page_name[..len].copy_from_slice(&name_bytes[..len]);

        // Copy parameter names.
        for (i, name_opt) in param_names.iter().enumerate() {
            if let Some(name) = name_opt {
                let bytes = name.as_bytes();
                let len = bytes.len().min(15);
                state.param_names[i][..len].copy_from_slice(&bytes[..len]);
            }
        }

        state.param_values = param_values;
        state
    }

    /// Convert a fixed-size null-padded byte array back to a `&str`.
    ///
    /// Stops at the first null byte. Returns `""` if the first byte is
    /// null or the slice is not valid UTF-8.
    pub fn bytes_to_str(bytes: &[u8]) -> &str {
        let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        core::str::from_utf8(&bytes[..end]).unwrap_or("")
    }
}

// ── DisplayChanges ───────────────────────────────────────────────────────

/// Identifies which parts of the display changed between two
/// [`DisplayState`] snapshots.
///
/// Currently used to gate the `flush()` call. Provides per-column
/// granularity as a foundation for future partial-update optimisation.
pub struct DisplayChanges {
    /// `true` if the page name differs.
    pub page_name_changed: bool,
    /// Per-column flag: `true` if either the name or value differs.
    pub param_changed: [bool; 4],
}

impl DisplayChanges {
    /// Diff two states field-by-field.
    pub fn detect(old: &DisplayState, new: &DisplayState) -> Self {
        let page_name_changed = old.page_name != new.page_name;

        let mut param_changed = [false; 4];
        for (i, changed) in param_changed.iter_mut().enumerate() {
            *changed = old.param_names[i] != new.param_names[i]
                || old.param_values[i] != new.param_values[i];
        }

        Self {
            page_name_changed,
            param_changed,
        }
    }

    /// Returns `true` if any field changed.
    pub fn any_changed(&self) -> bool {
        self.page_name_changed || self.param_changed.iter().any(|&c| c)
    }
}

// ── Helper ───────────────────────────────────────────────────────────────

/// Semantic wrapper around `old != new`.
///
/// Returns `true` if the two display states differ in any field.
pub fn display_state_changed(old: &DisplayState, new: &DisplayState) -> bool {
    old != new
}

// ── Rendering ────────────────────────────────────────────────────────────

/// Render a [`DisplayState`] to a display buffer using `embedded-graphics`.
///
/// All layout geometry is read from `config` — there are no module-level
/// layout constants.
///
/// # Layout
///
/// ```text
/// ┌──────────────────────────────────────────────────┐
/// │              PAGE NAME (centred)                  │  ← header_height
/// ├────────────┬────────────┬────────────┬───────────┤
/// │  ParamName │  ParamName │  ParamName │ ParamName │  ← param_name_y
/// │   Value    │   Value    │   Value    │  (blank)  │  ← param_value_y
/// └────────────┴────────────┴────────────┴───────────┘
///   col 0        col 1        col 2        col 3
/// ```
///
/// # Example
///
/// ```no_run
/// # use spirant_oled_display_rs::layout::{DisplayState, render_display};
/// # use spirant_oled_display_rs::DisplayConfig;
/// # fn example(display: &mut impl embedded_graphics::draw_target::DrawTarget<Color = embedded_graphics::pixelcolor::BinaryColor>) {
/// let state = DisplayState::from_params(
///     "Filter",
///     [Some("Cutoff"), Some("Reso"), None, None],
///     [Some(64), Some(32), None, None],
/// );
/// let config = DisplayConfig::default();
/// render_display(display, &state, &config).ok();
/// # }
/// ```
pub fn render_display<D>(
    display: &mut D,
    state: &DisplayState,
    config: &DisplayConfig,
) -> Result<(), D::Error>
where
    D: DrawTarget<Color = BinaryColor>,
{
    let text_style = MonoTextStyle::new(&FONT_6X10, BinaryColor::On);

    // ── Draw page name (centred at top) ──────────────────────────────
    let page_name = DisplayState::bytes_to_str(&state.page_name);
    if !page_name.is_empty() {
        let centre_x = config.display_width as i32 / 2;
        // Vertically centre within the header region.
        let y = config.header_height as i32 - 1;
        Text::with_alignment(page_name, Point::new(centre_x, y), text_style, Alignment::Center)
            .draw(display)?;
    }

    // ── Draw parameter columns ───────────────────────────────────────
    for i in 0..4 {
        let col_x = i as i32 * config.column_width as i32;
        let centre_x = col_x + config.column_width as i32 / 2;

        // Parameter name
        let name = DisplayState::bytes_to_str(&state.param_names[i]);
        if !name.is_empty() {
            Text::with_alignment(
                name,
                Point::new(centre_x, config.param_name_y),
                text_style,
                Alignment::Center,
            )
            .draw(display)?;
        }

        // Parameter value
        if let Some(v) = state.param_values[i] {
            let mut buf: String<8> = String::new();
            // core::fmt::Write — works in no_std without alloc.
            let _ = write!(buf, "{}", v);
            Text::with_alignment(
                buf.as_str(),
                Point::new(centre_x, config.param_value_y),
                text_style,
                Alignment::Center,
            )
            .draw(display)?;
        }
    }

    Ok(())
}

// ── Tests ────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_display_state_is_empty() {
        let state = DisplayState::default();
        assert_eq!(state.page_name, [0u8; 16]);
        for name in &state.param_names {
            assert_eq!(name, &[0u8; 16]);
        }
        for val in &state.param_values {
            assert!(val.is_none());
        }
    }

    #[test]
    fn from_params_copies_strings() {
        let state = DisplayState::from_params(
            "Filter",
            [Some("Cutoff"), Some("Reso"), None, None],
            [Some(64), Some(32), None, None],
        );

        assert_eq!(DisplayState::bytes_to_str(&state.page_name), "Filter");
        assert_eq!(
            DisplayState::bytes_to_str(&state.param_names[0]),
            "Cutoff"
        );
        assert_eq!(DisplayState::bytes_to_str(&state.param_names[1]), "Reso");
        assert_eq!(DisplayState::bytes_to_str(&state.param_names[2]), "");
        assert_eq!(DisplayState::bytes_to_str(&state.param_names[3]), "");
        assert_eq!(state.param_values[0], Some(64));
        assert_eq!(state.param_values[1], Some(32));
        assert!(state.param_values[2].is_none());
        assert!(state.param_values[3].is_none());
    }

    #[test]
    fn from_params_truncates_long_strings() {
        let long_name = "ABCDEFGHIJKLMNOPQRST"; // 20 chars
        let state = DisplayState::from_params(
            long_name,
            [Some(long_name), None, None, None],
            [None; 4],
        );

        // Should be truncated to 15 chars.
        assert_eq!(
            DisplayState::bytes_to_str(&state.page_name),
            "ABCDEFGHIJKLMNO"
        );
        assert_eq!(
            DisplayState::bytes_to_str(&state.param_names[0]),
            "ABCDEFGHIJKLMNO"
        );
    }

    #[test]
    fn bytes_to_str_handles_null_padding() {
        let mut buf = [0u8; 16];
        buf[0] = b'H';
        buf[1] = b'i';
        assert_eq!(DisplayState::bytes_to_str(&buf), "Hi");
    }

    #[test]
    fn bytes_to_str_handles_fully_empty() {
        let buf = [0u8; 16];
        assert_eq!(DisplayState::bytes_to_str(&buf), "");
    }

    #[test]
    fn display_state_changed_detects_differences() {
        let a = DisplayState::default();
        let b = DisplayState::from_params("X", [None; 4], [None; 4]);
        assert!(display_state_changed(&a, &b));
        assert!(!display_state_changed(&a, &a));
    }

    #[test]
    fn display_changes_detect_page_name() {
        let a = DisplayState::from_params("A", [None; 4], [None; 4]);
        let b = DisplayState::from_params("B", [None; 4], [None; 4]);
        let changes = DisplayChanges::detect(&a, &b);
        assert!(changes.page_name_changed);
        assert!(changes.any_changed());
        assert!(!changes.param_changed.iter().any(|&c| c));
    }

    #[test]
    fn display_changes_detect_param_value() {
        let a = DisplayState::from_params("P", [Some("X"); 4], [Some(0); 4]);
        let b = DisplayState::from_params("P", [Some("X"); 4], [Some(0), Some(1), Some(0), Some(0)]);
        let changes = DisplayChanges::detect(&a, &b);
        assert!(!changes.page_name_changed);
        assert!(!changes.param_changed[0]);
        assert!(changes.param_changed[1]);
        assert!(!changes.param_changed[2]);
        assert!(!changes.param_changed[3]);
        assert!(changes.any_changed());
    }

    #[test]
    fn display_changes_no_changes() {
        let state = DisplayState::from_params("P", [Some("A"); 4], [Some(10); 4]);
        let changes = DisplayChanges::detect(&state, &state);
        assert!(!changes.any_changed());
    }

    #[test]
    fn default_config_values() {
        let c = DisplayConfig::default();
        assert_eq!(c.update_frequency_hz, 30);
        assert_eq!(c.display_width, 128);
        assert_eq!(c.display_height, 64);
        assert_eq!(c.column_width, 32);
        assert_eq!(c.header_height, 12);
        assert_eq!(c.param_name_y, 24);
        assert_eq!(c.param_value_y, 40);
    }

    #[test]
    fn update_period_30hz() {
        let c = DisplayConfig::default();
        assert_eq!(c.update_period_ms(), 33);
    }

    #[test]
    fn update_period_60hz() {
        let c = DisplayConfig {
            update_frequency_hz: 60,
            ..DisplayConfig::default()
        };
        assert_eq!(c.update_period_ms(), 16);
    }

    #[test]
    fn update_period_20hz() {
        let c = DisplayConfig {
            update_frequency_hz: 20,
            ..DisplayConfig::default()
        };
        assert_eq!(c.update_period_ms(), 50);
    }
}
