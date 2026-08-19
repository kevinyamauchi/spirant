use super::error::ParameterError;
use super::page::Page;
use super::parameter::{Parameter, ParameterSlot};
use super::{N_PAGES, PARAMS_PER_PAGE, PARAM_SPECS};

/// Describes a single parameter change, returned by the change consumption methods.
#[derive(Debug, Clone, Copy)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub struct ParameterChange {
    /// Full parameter name (from [`PARAM_SPECS`]).
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
/// [`PARAM_SPECS`] configuration. Every `Some(spec)` entry becomes an
/// [`Active`](ParameterSlot::Active) slot seeded to the spec default; every
/// `None` becomes [`Null`](ParameterSlot::Null).
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
    /// Create a new instance with Active/Null slots derived from [`PARAM_SPECS`].
    ///
    /// Slots corresponding to `Some(spec)` in `PARAM_SPECS` are initialized as
    /// `Active(Parameter::from_spec(spec))` (value seeded to the spec default).
    /// Slots corresponding to `None` are `Null`.
    pub fn new() -> Self {
        let mut pages = [Page::default(); N_PAGES];

        for (page_idx, page) in pages.iter_mut().enumerate() {
            for (slot_idx, slot) in page.params.iter_mut().enumerate() {
                *slot = match &PARAM_SPECS[page_idx][slot_idx] {
                    Some(spec) => ParameterSlot::Active(Parameter::from_spec(spec)),
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
    /// // Encoder 0 on page 0 ("Waveshape", default 15, step 1) — active slot
    /// pv.update_from_encoder(0, 10);
    /// assert_eq!(pv.pages[0].params[0].as_ref().unwrap().value, 25);
    ///
    /// // Encoder 3 on page 2 (Overdrive) — null slot, no-op
    /// pv.set_page(2).unwrap();
    /// pv.update_from_encoder(3, 5);
    /// ```
    ///
    /// The applied change is `delta * step`, where `step` is the parameter's
    /// per-detent increment; the result is clamped to `[min, max]`.
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
                param.set_value(param.value + delta * param.step);
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
    /// pv.update_from_encoder(0, 42); // Waveshape: 15 default + 42 = 57
    ///
    /// let (changes, count) = pv.take_oled_changes();
    /// assert_eq!(count, 1);
    /// assert_eq!(changes[0].unwrap().value, 57);
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
                        // PARAM_SPECS entry is guaranteed Some for Active slots
                        // (maintained by the new() invariant).
                        let name = PARAM_SPECS[page_idx][enc_idx]
                            .expect("Active slot must have a spec in PARAM_SPECS")
                            .name;
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
                        let name = PARAM_SPECS[page_idx][enc_idx]
                            .expect("Active slot must have a spec in PARAM_SPECS")
                            .name;
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

    // ── System Bus (I2C target) support ──────────────────────────────

    /// Mark every active slot as pending for the Daisy.
    ///
    /// Called once at Pico boot so a fresh (or rebooted) Pico presents the
    /// MSG line LOW with the full parameter state pending, which is what
    /// makes the re-sync path identical to normal operation (protocol §3.2).
    ///
    /// Only touches `changed_i2c`; `changed_oled` is left alone, since the
    /// OLED task redraws from its own page-switch logic.
    ///
    /// # Examples
    ///
    /// ```
    /// use spirant::parameter_values::ParameterValues;
    ///
    /// let mut pv = ParameterValues::new();
    /// assert!(!pv.any_changed_i2c());
    /// pv.mark_all_changed_i2c();
    /// assert!(pv.any_changed_i2c());
    /// ```
    pub fn mark_all_changed_i2c(&mut self) {
        for page in &mut self.pages {
            for slot in &mut page.params {
                if let ParameterSlot::Active(param) = slot {
                    param.changed_i2c = true;
                }
            }
        }
    }

    /// Returns `true` if any active slot has its `changed_i2c` flag set.
    ///
    /// This is the MSG line invariant: the Pico drives MSG LOW exactly when
    /// this returns `true` (protocol §2.2).
    pub fn any_changed_i2c(&self) -> bool {
        self.pages.iter().any(|page| {
            page.params
                .iter()
                .any(|slot| matches!(slot, ParameterSlot::Active(p) if p.changed_i2c))
        })
    }

    /// Clear the `changed_i2c` flag of each reported slot, but **only** if
    /// the slot's current value still equals the value that was reported.
    ///
    /// This is the clear-if-unchanged rule (protocol §5.2), applied after a
    /// response frame is known to have been fully clocked out. A slot whose
    /// value moved again during the ~2.4 ms transmission window keeps its
    /// flag, so the newer value is re-reported on the next query. Without
    /// this rule, the last detent of an encoder burst landing inside the
    /// transmission window would be silently lost, leaving the display and
    /// the audio engine permanently divergent.
    ///
    /// `reported` is the `(global_idx, value)` list returned by
    /// [`serialize_frame`](crate::wire::serialize_frame). Entries naming a
    /// null or out-of-range slot are ignored.
    ///
    /// Note that [`take_i2c_changes()`](Self::take_i2c_changes) is *not* a
    /// substitute: it clears eagerly, before transmission is confirmed.
    ///
    /// # Examples
    ///
    /// ```
    /// use spirant::parameter_values::ParameterValues;
    ///
    /// let mut pv = ParameterValues::new();
    /// pv.update_from_encoder(0, 1); // Waveshape 15 -> 16, changed_i2c set
    ///
    /// // The value still matches what was transmitted, so the flag clears.
    /// pv.clear_i2c_flags_if_unchanged(&[(0, 16)]);
    /// assert!(!pv.any_changed_i2c());
    /// ```
    pub fn clear_i2c_flags_if_unchanged(&mut self, reported: &[(u8, i32)]) {
        for &(global_idx, reported_value) in reported {
            let Ok((page, encoder)) = self.global_to_page_encoder(global_idx as usize) else {
                continue;
            };
            if let ParameterSlot::Active(param) = &mut self.pages[page].params[encoder] {
                if param.value == reported_value {
                    param.changed_i2c = false;
                }
            }
        }
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
    use crate::parameter_values::ParamSpec;

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
    fn default_initializes_active_and_null_slots_from_param_specs() {
        let pv = ParameterValues::new();

        for (page_idx, page) in pv.pages.iter().enumerate() {
            for (slot_idx, slot) in page.params.iter().enumerate() {
                match PARAM_SPECS[page_idx][slot_idx] {
                    Some(_) => assert!(slot.is_active(), "page {} slot {} should be Active", page_idx, slot_idx),
                    None => assert!(!slot.is_active(), "page {} slot {} should be Null", page_idx, slot_idx),
                }
            }
        }
    }

    #[test]
    fn default_values_seed_from_specs() {
        let pv = ParameterValues::new();
        // A representative sample of v0 patch defaults across pages/units.
        assert_eq!(pv.pages[0].params[0].as_ref().unwrap().value, 15); // Waveshape %
        assert_eq!(pv.pages[1].params[2].as_ref().unwrap().value, 150); // Cutoff Floor Hz
        assert_eq!(pv.pages[4].params[0].as_ref().unwrap().value, 409); // Delay Time ms
        assert_eq!(pv.pages[5].params[2].as_ref().unwrap().value, 7000); // Damp LP Hz
    }

    // ── Page navigation ──────────────────────────────────────────────

    #[test]
    fn set_page_valid() {
        let mut pv = ParameterValues::new();
        assert!(pv.set_page(5).is_ok());
        assert_eq!(pv.current_page(), 5);
    }

    #[test]
    fn set_page_out_of_bounds() {
        let mut pv = ParameterValues::new();
        assert_eq!(pv.set_page(6), Err(ParameterError::InvalidPageIndex));
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
        pv.set_active_page(5).unwrap();
        assert_eq!(pv.current_page(), 5);

        // Page 5 (Reverb) has 3 active slots and 1 null.
        for (i, slot) in pv.pages[5].params.iter().enumerate() {
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
        assert_eq!(pv.set_active_page(6), Err(ParameterError::InvalidPageIndex));
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
        // Page 0 slot 0 = Waveshape, default 15, step 1.
        pv.update_from_encoder(0, 10);

        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param.value, 25);
        assert!(param.changed_oled);
        assert!(param.changed_i2c);
    }

    #[test]
    fn update_from_encoder_scales_by_step() {
        let mut pv = ParameterValues::new();
        // Page 4 slot 0 = Delay Time, default 409, step 5.
        pv.set_page(4).unwrap();
        pv.update_from_encoder(0, 2); // 2 detents × step 5 = +10

        let param = pv.pages[4].params[0].as_ref().unwrap();
        assert_eq!(param.value, 419);
    }

    #[test]
    fn update_from_encoder_null() {
        let mut pv = ParameterValues::new();
        pv.set_page(2).unwrap(); // Page 2 (Overdrive) slot 3 is Null.

        pv.update_from_encoder(3, 10);

        // Slot is still Null, no panic occurred.
        assert!(matches!(pv.pages[2].params[3], ParameterSlot::Null));
    }

    #[test]
    fn update_from_encoder_clamp_max() {
        let mut pv = ParameterValues::new();
        // Page 0 slot 0 = Waveshape, max 100, step 1.
        pv.update_from_encoder(0, 200);

        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param.value, 100);
    }

    #[test]
    fn update_from_encoder_clamp_min_with_step() {
        let mut pv = ParameterValues::new();
        // Page 3 slot 0 = LFO Rate, default 50, min 10, step 5.
        pv.set_page(3).unwrap();
        pv.update_from_encoder(0, -20); // 50 + (-20 × 5) = -50 → clamp to 10

        let param = pv.pages[3].params[0].as_ref().unwrap();
        assert_eq!(param.value, 10);
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
        pv.update_from_encoder(0, 42); // Resonance: 35 default + 42 = 77

        // Page 1 encoder 0 should be updated ("Resonance").
        let param = pv.pages[1].params[0].as_ref().unwrap();
        assert_eq!(param.value, 77);

        // Page 0 encoder 0 should be unchanged ("Waveshape" default).
        let param0 = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param0.value, 15);
    }

    // ── I2C updates ──────────────────────────────────────────────────

    #[test]
    fn update_from_i2c_sets_only_oled_flag() {
        let mut pv = ParameterValues::new();
        // I2C writes are absolute (not step-scaled).
        pv.update_from_i2c(0, 50).unwrap();

        let param = pv.pages[0].params[0].as_ref().unwrap();
        assert_eq!(param.value, 50);
        assert!(param.changed_oled);
        assert!(!param.changed_i2c);
    }

    #[test]
    fn update_from_i2c_invalid_global_idx() {
        let mut pv = ParameterValues::new();
        // 24 slots total (6 pages × 4).
        assert_eq!(pv.update_from_i2c(24, 50), Err(ParameterError::InvalidGlobalIndex));
        assert_eq!(pv.update_from_i2c(999, 50), Err(ParameterError::InvalidGlobalIndex));
    }

    #[test]
    fn update_from_i2c_null_slot() {
        let mut pv = ParameterValues::new();
        // Global index 11 = page 2 (Overdrive), slot 3 (Null).
        assert_eq!(pv.update_from_i2c(11, 50), Err(ParameterError::NullSlot));
    }

    // ── Global index access ──────────────────────────────────────────

    #[test]
    fn global_index_math_page_1() {
        let pv = make_pv_with_value(1, 2, 77);
        // Global index for page 1, encoder 2 = 4 + 2 = 6.
        let param = pv.get_param_by_global_idx(6).unwrap();
        assert_eq!(param.value, 77);
    }

    #[test]
    fn global_index_math_page_5() {
        let pv = make_pv_with_value(5, 1, 33);
        // Global index for page 5, encoder 1 = 20 + 1 = 21.
        let param = pv.get_param_by_global_idx(21).unwrap();
        assert_eq!(param.value, 33);
    }

    #[test]
    fn global_index_out_of_bounds() {
        let pv = ParameterValues::new();
        assert!(pv.get_param_by_global_idx(24).is_none());
        assert!(pv.get_param_by_global_idx(100).is_none());
    }

    #[test]
    fn global_index_null_slot_returns_none() {
        let pv = ParameterValues::new();
        // Global index 23 = page 5, slot 3 (Null).
        assert!(pv.get_param_by_global_idx(23).is_none());
        // Global index 2 = page 0, slot 2 (Null).
        assert!(pv.get_param_by_global_idx(2).is_none());
    }

    #[test]
    fn set_param_by_global_idx_works() {
        let mut pv = ParameterValues::new();
        pv.set_param_by_global_idx(5, 64).unwrap(); // page 1, encoder 1 (Brightness)

        let param = pv.pages[1].params[1].as_ref().unwrap();
        assert_eq!(param.value, 64);
        assert!(param.changed_oled);
        assert!(!param.changed_i2c); // I2C semantics
    }

    // ── Change consumption ───────────────────────────────────────────

    #[test]
    fn take_oled_changes_returns_name_and_value() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 42); // page 0, encoder 0 = "Waveshape" (15 + 42)

        let (changes, count) = pv.take_oled_changes();
        assert_eq!(count, 1);

        let change = changes[0].unwrap();
        assert_eq!(change.name, "Waveshape"); // full name, not the OLED label
        assert_eq!(change.value, 57);
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
        // Trigger changes on all 4 encoders of page 0 (2 active, 2 null).
        for i in 0..PARAMS_PER_PAGE {
            pv.update_from_encoder(i, 10);
        }

        let (changes, count) = pv.take_oled_changes();
        assert_eq!(count, 2); // Only 2 active slots on page 0.

        for i in 0..count {
            let change = changes[i].unwrap();
            assert_eq!(change.page, 0);
        }
    }

    #[test]
    fn take_oled_changes_skips_unchanged() {
        let mut pv = ParameterValues::new();
        // Only change encoder 1 on page 0 (Pulse Width).
        pv.update_from_encoder(1, 5);

        let (result, count) = pv.take_oled_changes();
        assert_eq!(count, 1);
        assert_eq!(result[0].unwrap().name, "Pulse Width");
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
    fn parameter_default_impl() {
        let param = Parameter::default();
        assert_eq!(param.min_value, 0);
        assert_eq!(param.max_value, 127);
        assert_eq!(param.value, 0);
        assert_eq!(param.step, 1);
    }

    #[test]
    fn parameter_from_spec_seeds_fields() {
        let spec = ParamSpec::new("Delay Time", "Time", 40, 750, 409, 5);
        let param = Parameter::from_spec(&spec);
        assert_eq!(param.value, 409);
        assert_eq!(param.min_value, 40);
        assert_eq!(param.max_value, 750);
        assert_eq!(param.step, 5);
        assert!(!param.changed_oled);
        assert!(!param.changed_i2c);
    }

    #[test]
    fn custom_min_max_clamp() {
        let mut param = Parameter {
            value: 0,
            min_value: 0,
            max_value: 10,
            step: 1,
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
        assert_eq!(pv.count_active_params(0), 2); // Oscillator: 2 active
        assert_eq!(pv.count_active_params(1), 4); // Filter: all 4
        assert_eq!(pv.count_active_params(2), 2); // Overdrive: 2 active
        assert_eq!(pv.count_active_params(3), 4); // Chorus: all 4
        assert_eq!(pv.count_active_params(4), 4); // Delay: all 4
        assert_eq!(pv.count_active_params(5), 3); // Reverb: 3 active, 1 null
    }

    #[test]
    fn count_active_params_out_of_bounds() {
        let pv = ParameterValues::new();
        assert_eq!(pv.count_active_params(6), 0);
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

        // Change on page 0 (Waveshape).
        pv.update_from_encoder(0, 10);

        // Change on page 1 via I2C (Resonance, global index 4).
        pv.update_from_i2c(4, 80).unwrap();

        // OLED should see both changes (encoder sets both flags, I2C sets OLED only).
        let (oled_changes, oled_count) = pv.take_oled_changes();
        assert_eq!(oled_count, 2);
        assert_eq!(oled_changes[0].unwrap().name, "Waveshape");
        assert_eq!(oled_changes[1].unwrap().name, "Resonance");

        // I2C should see only the encoder-driven change (the I2C write
        // did not set changed_i2c, so it doesn't appear here).
        let (i2c_changes, i2c_count) = pv.take_i2c_changes();
        assert_eq!(i2c_count, 1);
        assert_eq!(i2c_changes[0].unwrap().name, "Waveshape");
    }
}
