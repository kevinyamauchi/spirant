use super::error::ParameterError;
use super::page::Page;
use super::parameter::{Parameter, ParameterSlot};
use super::{N_PAGES, PARAMS_PER_PAGE, PARAM_NAMES};

/// Describes a single parameter change, returned by the change consumption methods.
#[derive(Debug, Clone, Copy)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub struct ParameterChange {
    /// Static display name of the parameter (from [`PARAM_NAMES`]).
    pub name: &'static str,
    /// Current value after the change.
    pub value: i32,
    /// Page index (0-based).
    pub page: usize,
    /// Encoder/slot index within the page (0-based).
    pub encoder: usize,
}

/// Total number of parameter slots across all pages.
const TOTAL_SLOTS: usize = N_PAGES * PARAMS_PER_PAGE;

/// Main parameter storage with page-based organization.
///
/// Manages synthesizer parameter state across multiple UI pages, tracks
/// change flags for the OLED display and I2C communication consumers,
/// and provides the shared data structure accessed by multiple async tasks.
///
/// # Initialization
///
/// [`ParameterValues::new()`] builds the page/slot layout from the static
/// [`PARAM_NAMES`] configuration. Every `Some(name)` entry becomes an
/// [`Active`](ParameterSlot::Active) slot with default values; every `None`
/// becomes [`Null`](ParameterSlot::Null).
pub struct ParameterValues {
    /// All pages, indexed 0 to `N_PAGES - 1`.
    pub pages: [Page; N_PAGES],
    /// Index of the currently active page (determines which parameters
    /// the physical encoders control).
    pub current_page: usize,
}

impl Default for ParameterValues {
    fn default() -> Self {
        Self::new()
    }
}

impl ParameterValues {
    /// Create a new instance with Active/Null slots derived from [`PARAM_NAMES`].
    ///
    /// Slots corresponding to `Some(name)` in `PARAM_NAMES` are initialized as
    /// `Active(Parameter::default())`. Slots corresponding to `None` are `Null`.
    pub fn new() -> Self {
        let mut pages = [Page::default(); N_PAGES];

        for (page_idx, page) in pages.iter_mut().enumerate() {
            for (slot_idx, slot) in page.params.iter_mut().enumerate() {
                *slot = match PARAM_NAMES[page_idx][slot_idx] {
                    Some(_) => ParameterSlot::Active(Parameter::default()),
                    None => ParameterSlot::Null,
                };
            }
        }

        Self {
            pages,
            current_page: 0,
        }
    }

    // ── Page navigation ──────────────────────────────────────────────

    /// Returns the index of the currently active page.
    pub fn current_page(&self) -> usize {
        self.current_page
    }

    /// Set the active page index **without** marking OLED change flags.
    ///
    /// Use [`set_active_page()`](Self::set_active_page) when a page switch
    /// should trigger a display redraw.
    ///
    /// Returns [`ParameterError::InvalidPageIndex`] if `page >= N_PAGES`.
    pub fn set_page(&mut self, page: usize) -> Result<(), ParameterError> {
        if page >= N_PAGES {
            return Err(ParameterError::InvalidPageIndex);
        }
        self.current_page = page;
        Ok(())
    }

    /// Set the active page and mark all active slots on the new page as
    /// changed for the OLED display.
    ///
    /// This is the typical method to call when the user switches pages,
    /// since the display needs to redraw all parameter names and values.
    ///
    /// Returns [`ParameterError::InvalidPageIndex`] if `page >= N_PAGES`.
    pub fn set_active_page(&mut self, page: usize) -> Result<(), ParameterError> {
        if page >= N_PAGES {
            return Err(ParameterError::InvalidPageIndex);
        }
        self.current_page = page;

        for slot in &mut self.pages[page].params {
            if let ParameterSlot::Active(param) = slot {
                param.changed_oled = true;
            }
        }
        Ok(())
    }

    /// Returns an immutable reference to the currently active page.
    pub fn get_active_page(&self) -> &Page {
        &self.pages[self.current_page]
    }

    /// Returns a mutable reference to the currently active page.
    pub fn get_active_page_mut(&mut self) -> &mut Page {
        &mut self.pages[self.current_page]
    }

    // ── Encoder-driven updates ───────────────────────────────────────

    /// Apply an encoder delta to a slot on the **current page**.
    ///
    /// If `encoder_idx` is out of bounds or the slot is
    /// [`Null`](ParameterSlot::Null), the call is a silent no-op (logged
    /// via `defmt` when that feature is enabled).
    ///
    /// # Examples
    ///
    /// ```
    /// use spirant::parameter_values::ParameterValues;
    ///
    /// let mut pv = ParameterValues::new();
    /// // Encoder 0 on page 0 ("Cutoff") — active slot
    /// pv.update_from_encoder(0, 10);
    /// assert_eq!(pv.pages[0].params[0].as_ref().unwrap().value, 10);
    ///
    /// // Encoder 3 on page 2 — null slot, no-op
    /// pv.set_page(2).unwrap();
    /// pv.update_from_encoder(3, 5);
    /// ```
    pub fn update_from_encoder(&mut self, encoder_idx: usize, delta: i32) {
        if encoder_idx >= PARAMS_PER_PAGE {
            #[cfg(feature = "defmt")]
            defmt::warn!(
                "update_from_encoder: encoder_idx {} out of bounds",
                encoder_idx
            );
            return;
        }

        let slot = &mut self.pages[self.current_page].params[encoder_idx];
        match slot {
            ParameterSlot::Active(param) => {
                param.set_value(param.value + delta);
            }
            ParameterSlot::Null => {
                #[cfg(feature = "defmt")]
                defmt::warn!(
                    "update_from_encoder called on Null slot: page={}, encoder={}",
                    self.current_page,
                    encoder_idx
                );
            }
        }
    }

    // ── I2C-driven updates ───────────────────────────────────────────

    /// Update a parameter by global index from the Daisy Seed (I2C write).
    ///
    /// Sets the value and marks **only** the OLED change flag to prevent
    /// echoing the value back over I2C.
    ///
    /// Global index mapping:
    /// - 0–3  → page 0, encoders 0–3
    /// - 4–7  → page 1, encoders 0–3
    /// - 8–11 → page 2, encoders 0–3
    /// - 12–15 → page 3, encoders 0–3
    pub fn update_from_i2c(
        &mut self,
        global_idx: usize,
        value: i32,
    ) -> Result<(), ParameterError> {
        let (page, encoder) = self.global_to_page_encoder(global_idx)?;

        match &mut self.pages[page].params[encoder] {
            ParameterSlot::Active(param) => {
                param.set_value_from_i2c(value);
                Ok(())
            }
            ParameterSlot::Null => Err(ParameterError::NullSlot),
        }
    }

    // ── Global index access ──────────────────────────────────────────

    /// Get an immutable reference to a parameter by global index.
    ///
    /// Returns `None` if the slot is [`Null`](ParameterSlot::Null) or
    /// the index is out of bounds.
    pub fn get_param_by_global_idx(&self, idx: usize) -> Option<&Parameter> {
        let (page, encoder) = self.global_to_page_encoder(idx).ok()?;
        self.pages[page].params[encoder].as_ref()
    }

    /// Set a parameter value by global index using I2C semantics
    /// (sets only the OLED change flag).
    ///
    /// Returns [`ParameterError::InvalidGlobalIndex`] if out of bounds,
    /// or [`ParameterError::NullSlot`] if the target slot is null.
    pub fn set_param_by_global_idx(
        &mut self,
        idx: usize,
        value: i32,
    ) -> Result<(), ParameterError> {
        self.update_from_i2c(idx, value)
    }

    // ── Utility ──────────────────────────────────────────────────────

    /// Count the number of active (non-null) parameter slots on a page.
    ///
    /// Returns 0 if `page_idx` is out of bounds.
    pub fn count_active_params(&self, page_idx: usize) -> usize {
        if page_idx >= N_PAGES {
            return 0;
        }
        self.pages[page_idx]
            .params
            .iter()
            .filter(|s| s.is_active())
            .count()
    }

    // ── Change consumption ───────────────────────────────────────────

    /// Collect all parameters whose OLED change flag is set, then clear
    /// those flags.
    ///
    /// Returns a fixed-size array and a count of valid entries. Callers
    /// should iterate `&result.0[..result.1]`.
    ///
    /// Only clears `changed_oled`; the `changed_i2c` flag is left intact.
    ///
    /// # Examples
    ///
    /// ```
    /// use spirant::parameter_values::ParameterValues;
    ///
    /// let mut pv = ParameterValues::new();
    /// pv.update_from_encoder(0, 42);
    ///
    /// let (changes, count) = pv.take_oled_changes();
    /// assert_eq!(count, 1);
    /// assert_eq!(changes[0].unwrap().value, 42);
    ///
    /// // Flags are cleared — second call returns nothing.
    /// let (_, count2) = pv.take_oled_changes();
    /// assert_eq!(count2, 0);
    /// ```
    pub fn take_oled_changes(
        &mut self,
    ) -> ([Option<ParameterChange>; TOTAL_SLOTS], usize) {
        let mut result = [None; TOTAL_SLOTS];
        let mut count = 0;

        for (page_idx, page) in self.pages.iter_mut().enumerate() {
            for (enc_idx, slot) in page.params.iter_mut().enumerate() {
                if let ParameterSlot::Active(param) = slot {
                    if param.changed_oled {
                        // PARAM_NAMES entry is guaranteed Some for Active slots
                        // (maintained by the new() invariant).
                        let name = PARAM_NAMES[page_idx][enc_idx]
                            .expect("Active slot must have a name in PARAM_NAMES");
                        result[count] = Some(ParameterChange {
                            name,
                            value: param.value,
                            page: page_idx,
                            encoder: enc_idx,
                        });
                        count += 1;
                        param.changed_oled = false;
                    }
                }
            }
        }

        (result, count)
    }

    /// Collect all parameters whose I2C change flag is set, then clear
    /// those flags.
    ///
    /// Returns a fixed-size array and a count of valid entries. Callers
    /// should iterate `&result.0[..result.1]`.
    ///
    /// Only clears `changed_i2c`; the `changed_oled` flag is left intact.
    pub fn take_i2c_changes(
        &mut self,
    ) -> ([Option<ParameterChange>; TOTAL_SLOTS], usize) {
        let mut result = [None; TOTAL_SLOTS];
        let mut count = 0;

        for (page_idx, page) in self.pages.iter_mut().enumerate() {
            for (enc_idx, slot) in page.params.iter_mut().enumerate() {
                if let ParameterSlot::Active(param) = slot {
                    if param.changed_i2c {
                        let name = PARAM_NAMES[page_idx][enc_idx]
                            .expect("Active slot must have a name in PARAM_NAMES");
                        result[count] = Some(ParameterChange {
                            name,
                            value: param.value,
                            page: page_idx,
                            encoder: enc_idx,
                        });
                        count += 1;
                        param.changed_i2c = false;
                    }
                }
            }
        }

        (result, count)
    }

    // ── Private helpers ──────────────────────────────────────────────

    /// Convert a global parameter index to (page, encoder) coordinates.
    fn global_to_page_encoder(
        &self,
        global_idx: usize,
    ) -> Result<(usize, usize), ParameterError> {
        if global_idx >= TOTAL_SLOTS {
            return Err(ParameterError::InvalidGlobalIndex);
        }
        let page = global_idx / PARAMS_PER_PAGE;
        let encoder = global_idx % PARAMS_PER_PAGE;
        Ok((page, encoder))
    }
}

// ── Unit Tests ───────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    // Helper: make a ParameterValues with a known active slot value.
    fn make_pv_with_value(page: usize, encoder: usize, value: i32) -> ParameterValues {
        let mut pv = ParameterValues::new();
        if let ParameterSlot::Active(param) = &mut pv.pages[page].params[encoder] {
            param.value = value;
        }
        pv
    }

    // ── Default state ────────────────────────────────────────────────

    #[test]
    fn default_state() {
        let pv = ParameterValues::new();
        assert_eq!(pv.current_page(), 0);

        // No changes should be pending.
        let mut pv = pv;
        let (_, oled_count) = pv.take_oled_changes();
        let (_, i2c_count) = pv.take_i2c_changes();
        assert_eq!(oled_count, 0);
        assert_eq!(i2c_count, 0);
    }

    #[test]
    fn default_initializes_active_and_null_slots_from_param_names() {
        let pv = ParameterValues::new();

        for (page_idx, page) in pv.pages.iter().enumerate() {
            for (slot_idx, slot) in page.params.iter().enumerate() {
                match PARAM_NAMES[page_idx][slot_idx] {
                    Some(_) => assert!(slot.is_active(), "page {} slot {} should be Active", page_idx, slot_idx),
                    None => assert!(!slot.is_active(), "page {} slot {} should be Null", page_idx, slot_idx),
                }
            }
        }
    }

    // ── Page navigation ──────────────────────────────────────────────

    #[test]
    fn set_page_valid() {
        let mut pv = ParameterValues::new();
        assert!(pv.set_page(2).is_ok());
        assert_eq!(pv.current_page(), 2);
    }

    #[test]
    fn set_page_out_of_bounds() {
        let mut pv = ParameterValues::new();
        assert_eq!(pv.set_page(4), Err(ParameterError::InvalidPageIndex));
        assert_eq!(pv.set_page(100), Err(ParameterError::InvalidPageIndex));
        // current_page unchanged
        assert_eq!(pv.current_page(), 0);
    }

    #[test]
    fn set_page_does_not_mark_oled() {
        let mut pv = ParameterValues::new();
        pv.set_page(1).unwrap();

        // No OLED flags should be set.
        let (_, count) = pv.take_oled_changes();
        assert_eq!(count, 0);
    }

    #[test]
    fn set_active_page_marks_oled_flags() {
        let mut pv = ParameterValues::new();
        pv.set_active_page(2).unwrap();
        assert_eq!(pv.current_page(), 2);

        // Page 2 has 3 active slots (LFO Rate, LFO Depth, LFO Shape).
        for (i, slot) in pv.pages[2].params.iter().enumerate() {
            match slot {
                ParameterSlot::Active(param) => {
                    assert!(param.changed_oled, "Active slot {} should have changed_oled", i);
                }
                ParameterSlot::Null => {
                    // Null slots have no flags to check — this is fine.
                }
            }
        }
    }

    #[test]
    fn set_active_page_does_not_mark_i2c_flags() {
        let mut pv = ParameterValues::new();
        pv.set_active_page(1).unwrap();

        for slot in &pv.pages[1].params {
            if let ParameterSlot::Active(param) = slot {
                assert!(!param.changed_i2c);
            }
        }
    }

    #[test]
    fn set_active_page_out_of_bounds() {
        let mut pv = ParameterValues::new();
        assert_eq!(pv.set_active_page(4), Err(ParameterError::InvalidPageIndex));
        assert_eq!(pv.current_page(), 0);
    }

    #[test]
    fn get_active_page_reflects_current() {
        let mut pv = ParameterValues::new();
        pv.set_page(1).unwrap();

        // Mutate page 1 slot 0 via get_active_page_mut.
        if let ParameterSlot::Active(param) = &mut pv.get_active_page_mut().params[0] {
            param.value = 99;
        }

        // Read back via get_active_page.
        let page = pv.get_active_page();
        assert_eq!(page.params[0].as_ref().unwrap().value, 99);
    }

    // ── Encoder updates ──────────────────────────────────────────────

    #[test]
    fn update_from_encoder_active() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 10);

        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param.value, 10);
        assert!(param.changed_oled);
        assert!(param.changed_i2c);
    }

    #[test]
    fn update_from_encoder_null() {
        let mut pv = ParameterValues::new();
        pv.set_page(2).unwrap(); // Page 2 slot 3 is Null.

        pv.update_from_encoder(3, 10);

        // Slot is still Null, no panic occurred.
        assert!(matches!(pv.pages[2].params[3], ParameterSlot::Null));
    }

    #[test]
    fn update_from_encoder_clamp_max() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 200); // default max is 127

        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param.value, 127);
    }

    #[test]
    fn update_from_encoder_clamp_min() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, -50); // default min is 0

        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param.value, 0);
    }

    #[test]
    fn update_from_encoder_invalid_idx() {
        let mut pv = ParameterValues::new();
        // Should be a no-op, no panic.
        pv.update_from_encoder(4, 10);
        pv.update_from_encoder(100, 10);

        // Page 0 slots are unmodified.
        let (_, count) = pv.take_oled_changes();
        assert_eq!(count, 0);
    }

    #[test]
    fn page_change_returns_correct_active_slot() {
        let mut pv = ParameterValues::new();
        pv.set_page(1).unwrap();
        pv.update_from_encoder(0, 42);

        // Page 1 encoder 0 should be updated ("Attack").
        let param = pv.pages[1].params[0].as_ref().unwrap();
        assert_eq!(param.value, 42);

        // Page 0 encoder 0 should be unchanged ("Cutoff").
        let param0 = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param0.value, 0);
    }

    // ── I2C updates ──────────────────────────────────────────────────

    #[test]
    fn update_from_i2c_sets_only_oled_flag() {
        let mut pv = ParameterValues::new();
        pv.update_from_i2c(0, 50).unwrap();

        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param.value, 50);
        assert!(param.changed_oled);
        assert!(!param.changed_i2c);
    }

    #[test]
    fn update_from_i2c_invalid_global_idx() {
        let mut pv = ParameterValues::new();
        assert_eq!(pv.update_from_i2c(16, 50), Err(ParameterError::InvalidGlobalIndex));
        assert_eq!(pv.update_from_i2c(999, 50), Err(ParameterError::InvalidGlobalIndex));
    }

    #[test]
    fn update_from_i2c_null_slot() {
        let mut pv = ParameterValues::new();
        // Global index 11 = page 2, slot 3 (Null).
        assert_eq!(pv.update_from_i2c(11, 50), Err(ParameterError::NullSlot));
    }

    // ── Global index access ──────────────────────────────────────────

    #[test]
    fn global_index_math_page_0() {
        let pv = make_pv_with_value(0, 2, 77);
        let param = pv.get_param_by_global_idx(2).unwrap();
        assert_eq!(param.value, 77);
    }

    #[test]
    fn global_index_math_page_1() {
        let pv = make_pv_with_value(1, 1, 33);
        // Global index for page 1, encoder 1 = 4 + 1 = 5.
        let param = pv.get_param_by_global_idx(5).unwrap();
        assert_eq!(param.value, 33);
    }

    #[test]
    fn global_index_out_of_bounds() {
        let pv = ParameterValues::new();
        assert!(pv.get_param_by_global_idx(16).is_none());
        assert!(pv.get_param_by_global_idx(100).is_none());
    }

    #[test]
    fn global_index_null_slot_returns_none() {
        let pv = ParameterValues::new();
        // Global index 11 = page 2, slot 3 (Null).
        assert!(pv.get_param_by_global_idx(11).is_none());
    }

    #[test]
    fn set_param_by_global_idx_works() {
        let mut pv = ParameterValues::new();
        pv.set_param_by_global_idx(5, 64).unwrap();

        let param = pv.pages[1].params[1].as_ref().unwrap();
        assert_eq!(param.value, 64);
        assert!(param.changed_oled);
        assert!(!param.changed_i2c); // I2C semantics
    }

    // ── Change consumption ───────────────────────────────────────────

    #[test]
    fn take_oled_changes_returns_name_and_value() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 42); // page 0, encoder 0 = "Cutoff"

        let (changes, count) = pv.take_oled_changes();
        assert_eq!(count, 1);

        let change = changes[0].unwrap();
        assert_eq!(change.name, "Cutoff");
        assert_eq!(change.value, 42);
        assert_eq!(change.page, 0);
        assert_eq!(change.encoder, 0);
    }

    #[test]
    fn take_oled_changes_clears_flags() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 10);

        let (_, count1) = pv.take_oled_changes();
        assert_eq!(count1, 1);

        let (_, count2) = pv.take_oled_changes();
        assert_eq!(count2, 0);
    }

    #[test]
    fn take_oled_changes_skips_null_slots() {
        let mut pv = ParameterValues::new();
        // Trigger changes on all 4 encoders of page 2 (3 active, 1 null).
        pv.set_page(2).unwrap();
        for i in 0..PARAMS_PER_PAGE {
            pv.update_from_encoder(i, 10);
        }

        let (changes, count) = pv.take_oled_changes();
        assert_eq!(count, 3); // Only 3 active slots on page 2.

        for i in 0..count {
            let change = changes[i].unwrap();
            assert_eq!(change.page, 2);
        }
    }

    #[test]
    fn take_oled_changes_skips_unchanged() {
        let mut pv = ParameterValues::new();
        // Only change encoder 1 on page 0.
        pv.update_from_encoder(1, 5);

        let (result, count) = pv.take_oled_changes();
        assert_eq!(count, 1);
        assert_eq!(result[0].unwrap().name, "Resonance");
    }

    #[test]
    fn take_i2c_changes_does_not_clear_oled_flag() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 10); // Sets both flags.

        // Consume I2C changes.
        let (_, i2c_count) = pv.take_i2c_changes();
        assert_eq!(i2c_count, 1);

        // OLED flag should still be set.
        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert!(param.changed_oled);
    }

    #[test]
    fn take_oled_changes_does_not_clear_i2c_flag() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 10); // Sets both flags.

        // Consume OLED changes.
        let (_, oled_count) = pv.take_oled_changes();
        assert_eq!(oled_count, 1);

        // I2C flag should still be set.
        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert!(param.changed_i2c);
    }

    // ── Parameter defaults and custom ranges ─────────────────────────

    #[test]
    fn min_max_defaults() {
        let param = Parameter::default();
        assert_eq!(param.min_value, 0);
        assert_eq!(param.max_value, 127);
        assert_eq!(param.value, 0);
    }

    #[test]
    fn custom_min_max_clamp() {
        let mut param = Parameter {
            value: 0,
            min_value: 0,
            max_value: 10,
            changed_oled: false,
            changed_i2c: false,
        };
        param.set_value(50);
        assert_eq!(param.value, 10);

        param.set_value(-5);
        assert_eq!(param.value, 0);
    }

    // ── count_active_params ──────────────────────────────────────────

    #[test]
    fn count_active_params_all_pages() {
        let pv = ParameterValues::new();
        assert_eq!(pv.count_active_params(0), 4); // Filter: all 4
        assert_eq!(pv.count_active_params(1), 4); // Envelope: all 4
        assert_eq!(pv.count_active_params(2), 3); // LFO: 3 active, 1 null
        assert_eq!(pv.count_active_params(3), 2); // Effects: 2 active, 2 null
    }

    #[test]
    fn count_active_params_out_of_bounds() {
        let pv = ParameterValues::new();
        assert_eq!(pv.count_active_params(4), 0);
        assert_eq!(pv.count_active_params(100), 0);
    }

    // ── ParameterSlot helpers ────────────────────────────────────────

    #[test]
    fn parameter_slot_is_active() {
        let active = ParameterSlot::Active(Parameter::default());
        let null = ParameterSlot::Null;
        assert!(active.is_active());
        assert!(!null.is_active());
    }

    #[test]
    fn parameter_slot_as_mut_and_as_ref() {
        let mut slot = ParameterSlot::Active(Parameter::default());
        slot.as_mut().unwrap().value = 100;
        assert_eq!(slot.as_ref().unwrap().value, 100);

        let mut null_slot = ParameterSlot::Null;
        assert!(null_slot.as_mut().is_none());
        assert!(null_slot.as_ref().is_none());
    }

    // ── Multiple changes across pages ────────────────────────────────

    #[test]
    fn changes_from_multiple_pages() {
        let mut pv = ParameterValues::new();

        // Change on page 0.
        pv.update_from_encoder(0, 10);

        // Change on page 1 via I2C.
        pv.update_from_i2c(4, 80).unwrap(); // page 1, encoder 0

        // OLED should see both changes (encoder sets both flags, I2C sets OLED only).
        let (oled_changes, oled_count) = pv.take_oled_changes();
        assert_eq!(oled_count, 2);
        assert_eq!(oled_changes[0].unwrap().name, "Cutoff");
        assert_eq!(oled_changes[1].unwrap().name, "Attack");

        // I2C should see only the encoder-driven change (the I2C write
        // did not set changed_i2c, so it doesn't appear here).
        let (i2c_changes, i2c_count) = pv.take_i2c_changes();
        assert_eq!(i2c_count, 1);
        assert_eq!(i2c_changes[0].unwrap().name, "Cutoff");
    }
}
