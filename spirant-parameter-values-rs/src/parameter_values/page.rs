use super::PARAMS_PER_PAGE;
use super::parameter::ParameterSlot;

/// A page of parameter slots mapped to the physical encoders.
///
/// Each page contains exactly [`PARAMS_PER_PAGE`] slots. Slots may be
/// [`Active`](ParameterSlot::Active) or [`Null`](ParameterSlot::Null)
/// depending on how many parameters the page needs.
#[derive(Debug, Clone, Copy)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub struct Page {
    /// Parameter slots indexed by encoder position (0–3).
    pub params: [ParameterSlot; PARAMS_PER_PAGE],
}

impl Default for Page {
    fn default() -> Self {
        Self {
            params: [ParameterSlot::Null; PARAMS_PER_PAGE],
        }
    }
}
