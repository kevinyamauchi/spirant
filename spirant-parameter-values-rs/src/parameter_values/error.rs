/// Errors that can occur when working with parameters.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub enum ParameterError {
    /// Page index is out of bounds (must be < N_PAGES).
    InvalidPageIndex,
    /// Encoder index is out of bounds (must be < PARAMS_PER_PAGE).
    InvalidEncoderIndex,
    /// Operation targeted a [`Null`](super::ParameterSlot::Null) slot.
    NullSlot,
    /// Global parameter index is out of bounds (must be < N_PAGES * PARAMS_PER_PAGE).
    InvalidGlobalIndex,
}
