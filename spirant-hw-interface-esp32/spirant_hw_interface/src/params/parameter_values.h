#pragma once

#include <stddef.h>
#include <stdint.h>

#include "param_specs.h"

// --- Runtime parameter state -------------------------------------------------
//
// C++ port of ParameterValues from spirant-parameter-values-rs (plan section
// 1.7). Same semantics, renamed change flags:
//
//   Rust changed_oled -> changed_display
//   Rust changed_i2c  -> changed_midi
//
// Change-flag rules, unchanged from the Rust:
//
//   encoder move   -> sets BOTH flags
//   inbound MIDI   -> sets changed_display ONLY, so a value the Daisy told us
//                     about is never echoed straight back at it
//   page switch    -> sets changed_display on every active slot of the new page
//
// No heap, no exceptions, no Arduino.h -- tests/ compiles this file directly
// with the system compiler.

enum class ParameterError : uint8_t
{
    None = 0,
    InvalidPageIndex,
    InvalidGlobalIndex,
    NullSlot,
};

/// One parameter's mutable state. Null slots are represented by
/// `spec->active == false` rather than a separate variant -- C++ has no cheap
/// Option, and every access already needs the spec pointer anyway.
struct Parameter
{
    const ParamSpec* spec;
    int32_t          value;
    bool             changed_display;
    bool             changed_midi;

    bool active() const { return spec->active; }

    /// Local change (encoder). Clamps, then flags both consumers.
    void setValue(int32_t v);

    /// Remote change (inbound MIDI). Clamps, then flags the display only.
    void setValueFromMidi(int32_t v);
};

/// Reported by the change-consumption calls.
struct ParameterChange
{
    const char* name;   ///< From kParamSpecs.
    int32_t     value;  ///< Value after the change.
    uint8_t     page;
    uint8_t     encoder;
    uint8_t     global_idx;  ///< page * kParamsPerPage + encoder.
};

class ParameterValues
{
  public:
    /// Builds the slot layout from kParamSpecs, seeding each active slot to
    /// its default. Both flags start clear.
    ParameterValues();

    // -- Page navigation ----------------------------------------------------

    uint8_t currentPage() const { return current_page_; }

    /// Change page WITHOUT marking anything for redraw.
    ParameterError setPage(uint8_t page);

    /// Change page and mark every active slot on the new page for redraw.
    /// This is what a button press should call.
    ParameterError setActivePage(uint8_t page);

    /// Page forward/backward with wraparound. Marks the new page for redraw.
    void pageForward();
    void pageBackward();

    // -- Updates ------------------------------------------------------------

    /// Apply an encoder delta to a slot on the *current* page. Out-of-range
    /// index or a null slot is a silent no-op, as in the Rust.
    void updateFromEncoder(uint8_t encoder_idx, int32_t delta);

    /// Apply a value from inbound MIDI, addressed by global index.
    ParameterError updateFromMidi(uint8_t global_idx, int32_t value);

    // -- Access -------------------------------------------------------------

    /// Null slots return non-null too -- check `active()`. Returns nullptr
    /// only for an out-of-range index.
    const Parameter* paramAt(uint8_t page, uint8_t encoder) const;
    const Parameter* paramByGlobalIdx(uint8_t global_idx) const;

    /// Active (non-null) slots on a page. 0 if the page index is out of range.
    uint8_t countActiveParams(uint8_t page) const;

    // -- Change consumption -------------------------------------------------

    /// Collect every slot with changed_display set, clearing those flags.
    /// Writes at most `max_out` entries; returns how many were written.
    /// Clears only changed_display.
    size_t takeDisplayChanges(ParameterChange* out, size_t max_out);

    /// Same, for changed_midi. Clears only changed_midi.
    size_t takeMidiChanges(ParameterChange* out, size_t max_out);

    /// Flag every active slot for the Daisy -- the boot-time full dump.
    void markAllChangedMidi();

    /// Flag every active slot for redraw -- forces a full repaint.
    void markAllChangedDisplay();

    bool anyChangedMidi() const;
    bool anyChangedDisplay() const;

  private:
    Parameter slots_[kNumPages][kParamsPerPage];
    uint8_t   current_page_;
};
