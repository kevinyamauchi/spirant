#include "parameter_values.h"

namespace
{

int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

// --- Parameter ---------------------------------------------------------------

void Parameter::setValue(int32_t v)
{
    value           = clamp_i32(v, spec->min, spec->max);
    changed_display = true;
    changed_midi    = true;
}

void Parameter::setValueFromMidi(int32_t v)
{
    value           = clamp_i32(v, spec->min, spec->max);
    changed_display = true;
    // Deliberately does NOT set changed_midi -- that is the echo guard.
}

// --- ParameterValues ---------------------------------------------------------

ParameterValues::ParameterValues() : current_page_(0)
{
    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            const ParamSpec& spec = kParamSpecs[p][e];

            slots_[p][e].spec            = &spec;
            slots_[p][e].value           = spec.def;
            slots_[p][e].changed_display = false;
            slots_[p][e].changed_midi    = false;
        }
    }
}

ParameterError ParameterValues::setPage(uint8_t page)
{
    if (page >= kNumPages) return ParameterError::InvalidPageIndex;

    current_page_ = page;
    return ParameterError::None;
}

ParameterError ParameterValues::setActivePage(uint8_t page)
{
    if (page >= kNumPages) return ParameterError::InvalidPageIndex;

    current_page_ = page;

    for (uint8_t e = 0; e < kParamsPerPage; ++e)
    {
        if (slots_[page][e].active()) slots_[page][e].changed_display = true;
    }
    return ParameterError::None;
}

void ParameterValues::pageForward()
{
    setActivePage(static_cast<uint8_t>((current_page_ + 1) % kNumPages));
}

void ParameterValues::pageBackward()
{
    setActivePage(static_cast<uint8_t>((current_page_ + kNumPages - 1) % kNumPages));
}

void ParameterValues::updateFromEncoder(uint8_t encoder_idx, int32_t delta)
{
    if (encoder_idx >= kParamsPerPage) return;

    Parameter& param = slots_[current_page_][encoder_idx];
    if (!param.active()) return;

    param.setValue(param.value + delta * param.spec->step);
}

ParameterError ParameterValues::updateFromMidi(uint8_t global_idx, int32_t value)
{
    if (global_idx >= kTotalSlots) return ParameterError::InvalidGlobalIndex;

    Parameter& param = slots_[global_idx / kParamsPerPage][global_idx % kParamsPerPage];
    if (!param.active()) return ParameterError::NullSlot;

    param.setValueFromMidi(value);
    return ParameterError::None;
}

const Parameter* ParameterValues::paramAt(uint8_t page, uint8_t encoder) const
{
    if (page >= kNumPages || encoder >= kParamsPerPage) return nullptr;
    return &slots_[page][encoder];
}

const Parameter* ParameterValues::paramByGlobalIdx(uint8_t global_idx) const
{
    if (global_idx >= kTotalSlots) return nullptr;
    return &slots_[global_idx / kParamsPerPage][global_idx % kParamsPerPage];
}

uint8_t ParameterValues::countActiveParams(uint8_t page) const
{
    if (page >= kNumPages) return 0;

    uint8_t n = 0;
    for (uint8_t e = 0; e < kParamsPerPage; ++e)
    {
        if (slots_[page][e].active()) ++n;
    }
    return n;
}

size_t ParameterValues::takeDisplayChanges(ParameterChange* out, size_t max_out)
{
    size_t count = 0;

    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            Parameter& param = slots_[p][e];
            if (!param.active() || !param.changed_display) continue;

            // Stop before clearing anything we cannot report -- a flag dropped
            // here is a value that never reaches the screen.
            if (count >= max_out) return count;

            out[count].name       = param.spec->name;
            out[count].value      = param.value;
            out[count].page       = p;
            out[count].encoder    = e;
            out[count].global_idx = static_cast<uint8_t>(p * kParamsPerPage + e);
            ++count;

            param.changed_display = false;
        }
    }
    return count;
}

size_t ParameterValues::takeMidiChanges(ParameterChange* out, size_t max_out)
{
    size_t count = 0;

    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            Parameter& param = slots_[p][e];
            if (!param.active() || !param.changed_midi) continue;

            if (count >= max_out) return count;

            out[count].name       = param.spec->name;
            out[count].value      = param.value;
            out[count].page       = p;
            out[count].encoder    = e;
            out[count].global_idx = static_cast<uint8_t>(p * kParamsPerPage + e);
            ++count;

            param.changed_midi = false;
        }
    }
    return count;
}

void ParameterValues::markAllChangedMidi()
{
    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            if (slots_[p][e].active()) slots_[p][e].changed_midi = true;
        }
    }
}

void ParameterValues::markAllChangedDisplay()
{
    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            if (slots_[p][e].active()) slots_[p][e].changed_display = true;
        }
    }
}

bool ParameterValues::anyChangedMidi() const
{
    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            if (slots_[p][e].active() && slots_[p][e].changed_midi) return true;
        }
    }
    return false;
}

bool ParameterValues::anyChangedDisplay() const
{
    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            if (slots_[p][e].active() && slots_[p][e].changed_display) return true;
        }
    }
    return false;
}
