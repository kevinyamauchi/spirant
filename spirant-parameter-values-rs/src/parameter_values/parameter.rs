/// Individual synthesizer parameter with value, range, and change tracking.
///
/// Each parameter has a clamped value range and two independent change flags
/// for the OLED display and I2C communication consumers.
#[derive(Debug, Clone, Copy)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub struct Parameter {
    /// Current parameter value, always within `[min_value, max_value]`.
    pub value: i32,
    /// Minimum allowed value (inclusive). Default: 0.
    pub min_value: i32,
    /// Maximum allowed value (inclusive). Default: 127.
    pub max_value: i32,
    /// Flag indicating the OLED display needs to update for this parameter.
    pub changed_oled: bool,
    /// Flag indicating the I2C bus needs to send this parameter to the Daisy Seed.
    pub changed_i2c: bool,
}

impl Default for Parameter {
    fn default() -> Self {
        Self {
            value: 0,
            min_value: 0,
            max_value: 127,
            changed_oled: false,
            changed_i2c: false,
        }
    }
}

impl Parameter {
    /// Update the value from an encoder or other local source.
    ///
    /// Clamps the new value to `[min_value, max_value]` and sets **both**
    /// `changed_oled` and `changed_i2c` flags, since local changes must
    /// propagate to both the display and the Daisy Seed.
    pub fn set_value(&mut self, v: i32) {
        self.value = v.clamp(self.min_value, self.max_value);
        self.changed_oled = true;
        self.changed_i2c = true;
    }

    /// Update the value from an I2C write (Daisy Seed → Pico).
    ///
    /// Clamps the new value to `[min_value, max_value]` and sets **only**
    /// the `changed_oled` flag. The `changed_i2c` flag is intentionally
    /// left unchanged to prevent echo back to the Daisy Seed.
    pub fn set_value_from_i2c(&mut self, v: i32) {
        self.value = v.clamp(self.min_value, self.max_value);
        self.changed_oled = true;
        // Intentionally do NOT set changed_i2c to prevent echo.
    }
}

/// A parameter slot that is either active (holding a [`Parameter`]) or null.
///
/// Pages always have [`PARAMS_PER_PAGE`](super::PARAMS_PER_PAGE) slots, but
/// not all may be in use. A `Null` slot means the corresponding physical
/// encoder has no effect on that page.
#[derive(Debug, Clone, Copy, Default)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub enum ParameterSlot {
    /// Slot holds an active parameter mapped to a physical encoder.
    Active(Parameter),
    /// Empty slot — encoder movements are ignored, nothing is displayed.
    #[default]
    Null,
}

impl ParameterSlot {
    /// Returns `true` if this slot holds an active parameter.
    pub fn is_active(&self) -> bool {
        matches!(self, ParameterSlot::Active(_))
    }

    /// Returns an immutable reference to the inner [`Parameter`], or `None`
    /// if this slot is [`Null`](ParameterSlot::Null).
    pub fn as_ref(&self) -> Option<&Parameter> {
        match self {
            ParameterSlot::Active(param) => Some(param),
            ParameterSlot::Null => None,
        }
    }

    /// Returns a mutable reference to the inner [`Parameter`], or `None`
    /// if this slot is [`Null`](ParameterSlot::Null).
    pub fn as_mut(&mut self) -> Option<&mut Parameter> {
        match self {
            ParameterSlot::Active(param) => Some(param),
            ParameterSlot::Null => None,
        }
    }
}
