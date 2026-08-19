#pragma once

#include <stdint.h>

// --- Static parameter descriptor table --------------------------------------
//
// C++ port of PARAM_SPECS in spirant-parameter-values-rs/src/parameter_values/
// mod.rs. Six pages of four slots, mirroring the Daisy v0 signal chain.
//
//   Page 0 (Oscillator): [Wave]  [PW]     [--null--] [--null--]
//   Page 1 (Filter):     [Res]   [Brite]  [Floor]    [Pass]
//   Page 2 (Overdrive):  [Drive] [Trim]   [--null--] [--null--]
//   Page 3 (Chorus):     [Rate]  [Depth]  [Dly]      [FB]
//   Page 4 (Delay):      [Time]  [FB]     [Wet]      [Damp]
//   Page 5 (Reverb):     [Send]  [Size]   [LP]       [--null--]
//
// SYNC CONTRACT: name, min, max, default and step must match PARAM_SPECS in
// the Rust crate; name, min and max must additionally match kParamTable in
// spirant-daisy/v1/comm/param_table.h. Three copies of one table is two too
// many, but they are in three languages on three MCUs -- changing a range is
// a three-firmware change.
//
// Values are integers in a real engineering unit, NOT 0-127. The unit column
// is carried here (it is not in the Rust ParamSpec) because the display has
// room to show it; it is copied from the Daisy's param_table.h.
//
// This header is deliberately free of Arduino.h so tests/ can compile it with
// the system compiler.

/// Parameter slots per page. Matches the number of physical encoders.
static const uint8_t kParamsPerPage = 4;

/// One page per v0 signal-processing stage.
static const uint8_t kNumPages = 6;

/// Total slots, active and null.
static const uint8_t kTotalSlots = kNumPages * kParamsPerPage;

struct ParamSpec
{
    bool        active;  ///< false for a null slot: the encoder does nothing.
    const char* name;    ///< Full name, for logs. Canonical identity.
    const char* label;   ///< Short label for the display column.
    const char* unit;    ///< Display suffix. Empty string for unitless.
    int32_t     min;     ///< Inclusive.
    int32_t     max;     ///< Inclusive.
    int32_t     def;     ///< Boot value. Equals the v0 patch value.
    int32_t     step;    ///< Units moved per encoder detent.
};

static const char* const kPageNames[kNumPages] = {
    "Oscillator", "Filter", "Overdrive", "Chorus", "Delay", "Reverb",
};

/// A null slot. `label`/`unit` are still readable so the renderer needs no
/// special case for them.
#define SPIRANT_NULL_SLOT \
    {                     \
        false, "(null)", "--", "", 0, 0, 0, 0 \
    }

static const ParamSpec kParamSpecs[kNumPages][kParamsPerPage] = {
    // Page 0: Oscillator (2 active)
    {
        {true, "Waveshape", "Wave", "%", 0, 100, 15, 1},
        {true, "Pulse Width", "PW", "%", 0, 100, 50, 1},
        SPIRANT_NULL_SLOT,
        SPIRANT_NULL_SLOT,
    },
    // Page 1: Filter (4 active)
    {
        {true, "Resonance", "Res", "%", 0, 100, 35, 1},
        {true, "Brightness", "Brite", "%", 0, 100, 66, 1},
        {true, "Cutoff Floor", "Floor", "Hz", 20, 500, 150, 5},
        {true, "Passband Gain", "Pass", "%", 0, 100, 0, 1},
    },
    // Page 2: Overdrive (2 active)
    {
        {true, "Drive", "Drive", "%", 0, 100, 0, 1},
        {true, "Output Trim", "Trim", "%", 0, 100, 40, 1},
        SPIRANT_NULL_SLOT,
        SPIRANT_NULL_SLOT,
    },
    // Page 3: Chorus (4 active)
    {
        {true, "LFO Rate", "Rate", "cHz", 10, 500, 50, 5},
        {true, "LFO Depth", "Depth", "%", 0, 93, 35, 1},
        {true, "Chorus Delay", "Dly", "%", 0, 100, 60, 1},
        {true, "Chorus Feedback", "FB", "%", 0, 100, 20, 1},
    },
    // Page 4: Delay (4 active)
    {
        {true, "Delay Time", "Time", "ms", 40, 750, 409, 5},
        {true, "Delay Feedback", "FB", "%", 0, 100, 40, 1},
        {true, "Wet Mix", "Wet", "%", 0, 100, 50, 1},
        {true, "Damping", "Damp", "/1k", 0, 497, 80, 5},
    },
    // Page 5: Reverb (3 active)
    {
        {true, "Reverb Send", "Send", "%", 0, 100, 75, 1},
        {true, "Reverb Size", "Size", "%", 0, 100, 85, 1},
        {true, "Damp LP", "LP", "Hz", 1000, 18000, 7000, 250},
        SPIRANT_NULL_SLOT,
    },
};

/// Number of active slots. Asserted by the host tests against the table.
static const uint8_t kNumActiveParams = 19;
