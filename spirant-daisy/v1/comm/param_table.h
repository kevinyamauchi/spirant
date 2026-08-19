#pragma once

#include <stdint.h>

// --- Parameter identity table -----------------------------------------------
// One entry per global_idx (0-23). The Pico owns the authoritative table
// (PARAM_SPECS in spirant-parameter-values-rs); this is the Daisy's mirror of
// the identity/unit/range columns, used to reject unknown indices and to clamp
// incoming values.
//
// SYNC CONTRACT: name, unit, min and max must match PARAM_SPECS exactly.
// Changing a parameter's unit or range is a two-firmware change. See protocol
// section 6. Values arrive as integers in the real unit shown below; the
// conversion to DSP units (the divisor and the target setter) is deliberately
// NOT here -- that arrives with the parameter engine in phase 4, so this
// header stays usable by the host tests with no libDaisy dependency.
//
// The five null indices (2, 3, 10, 11, 23) are slots the Pico's 4-encoder
// pages leave empty. They never appear in a frame; a record naming one is a
// protocol violation and is skipped.

struct ParamRange
{
    bool        active;  // false for the five null slots
    int32_t     min;
    int32_t     max;
    const char* name;  // for logging only
    const char* unit;
};

constexpr uint8_t kNumSlots = 24;

constexpr ParamRange kParamTable[kNumSlots] = {
    // Page 0: Oscillator
    {true, 0, 100, "Waveshape", "%"},         // 0
    {true, 0, 100, "Pulse Width", "%"},       // 1
    {false, 0, 0, "(null)", ""},              // 2
    {false, 0, 0, "(null)", ""},              // 3
    // Page 1: Filter
    {true, 0, 100, "Resonance", "%"},         // 4
    {true, 0, 100, "Brightness", "%"},        // 5
    {true, 20, 500, "Cutoff Floor", "Hz"},    // 6
    {true, 0, 100, "Passband Gain", "%"},     // 7
    // Page 2: Overdrive
    {true, 0, 100, "Drive", "%"},             // 8
    {true, 0, 100, "Output Trim", "%"},       // 9
    {false, 0, 0, "(null)", ""},              // 10
    {false, 0, 0, "(null)", ""},              // 11
    // Page 3: Chorus
    {true, 10, 500, "LFO Rate", "cHz"},       // 12
    {true, 0, 93, "LFO Depth", "%"},          // 13
    {true, 0, 100, "Chorus Delay", "%"},      // 14
    {true, 0, 100, "Chorus Feedback", "%"},   // 15
    // Page 4: Delay
    {true, 40, 750, "Delay Time", "ms"},      // 16
    {true, 0, 100, "Delay Feedback", "%"},    // 17
    {true, 0, 100, "Wet Mix", "%"},           // 18
    {true, 0, 497, "Damping", "per-mille"},   // 19
    // Page 5: Reverb
    {true, 0, 100, "Reverb Send", "%"},       // 20
    {true, 0, 100, "Reverb Size", "%"},       // 21
    {true, 1000, 18000, "Damp LP", "Hz"},     // 22
    {false, 0, 0, "(null)", ""},              // 23
};

// Number of active slots -- the record count of a GetAll frame.
constexpr uint8_t kNumActiveParams = 19;
