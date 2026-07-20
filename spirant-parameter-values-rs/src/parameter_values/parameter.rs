/// Static description of a parameter: names, range, default, and encoder step.
///
/// The `PARAM_SPECS` table is built from these, and
/// [`ParameterValues::new()`](super::ParameterValues::new) constructs each
/// runtime [`Parameter`] from its spec via [`Parameter::from_spec`].
///
/// Two name fields are carried deliberately:
/// - `name` — the full, human-readable identity used for `defmt` logging and
///   as the parameter's canonical name (and, in future, its I2C identity to
///   the Daisy Seed).
/// - `label` — a short (≤5 character) string for the 32 px OLED column.
#[derive(Debug, Clone, Copy)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub struct ParamSpec {
    /// Full parameter name (logging / canonical identity).
    pub name: &'static str,
    /// Short OLED display label (≤5 characters).
    pub label: &'static str,
    /// Minimum allowed value (inclusive).
    pub min: i32,
    /// Maximum allowed value (inclusive).
    pub max: i32,
    /// Value the parameter boots to (should equal the v0 patch value).
    pub default: i32,
    /// Units moved per encoder detent.
    pub step: i32,
}

impl ParamSpec {
    /// Construct a spec. `const` so the `PARAM_SPECS` table stays a `const`.
    pub const fn new(
        name: &'static str,
        label: &'static str,
        min: i32,
        max: i32,
        default: i32,
        step: i32,
    ) -> Self {
        Self {
            name,
            label,
            min,
            max,
            default,
            step,
        }
    }
}

/// Individual synthesizer parameter with value, range, and change tracking.
///
/// Each parameter has a clamped value range, a per-detent `step`, and two
/// independent change flags for the OLED display and I2C communication
/// consumers. Values are stored as integers in a real engineering unit
/// (percent, Hz, ms, …) documented per parameter in [`ParamSpec`].
#[derive(Debug, Clone, Copy)]
#[cfg_attr(feature = "defmt", derive(defmt::Format))]
pub struct Parameter {
    /// Current parameter value, always within `[min_value, max_value]`.
    pub value: i32,
    /// Minimum allowed value (inclusive).
    pub min_value: i32,
    /// Maximum allowed value (inclusive).
    pub max_value: i32,
    /// Units the value moves per encoder detent.
    pub step: i32,
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
            step: 1,
            changed_oled: false,
            changed_i2c: false,
        }
    }
}

impl Parameter {
    /// Build a runtime parameter from its static [`ParamSpec`].
    ///
    /// The value is seeded to `spec.default`; both change flags start clear.
    pub fn from_spec(spec: &ParamSpec) -> Self {
        Self {
            value: spec.default,
            min_value: spec.min,
            max_value: spec.max,
            step: spec.step,
            changed_oled: false,
            changed_i2c: false,
        }
    }

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
