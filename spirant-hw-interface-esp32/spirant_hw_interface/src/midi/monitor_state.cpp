#include "monitor_state.h"

#include <string.h>

namespace
{

/// Breath. The design's central signal gets its own display element, so it is
/// excluded from the generic CC slots.
const uint8_t kBreathCc = 2;

/// Window over which messages-per-second is averaged.
const uint32_t kRateWindowMs = 1000;

}  // namespace

void MonitorState::reset()
{
    memset(&s_, 0, sizeof(s_));
    memset(note_held_, 0, sizeof(note_held_));

    s_.lowest_active_note     = 0xFF;
    s_.bend14                 = 8192;
    s_.bend_min               = 8192;
    s_.bend_max               = 8192;
    s_.breath_min_interval_us = 0xFFFFFFFFUL;

    rate_window_start_ms_ = 0;
    rate_window_count_    = 0;
    breath_window_count_  = 0;
}

void MonitorState::copyTo(MonitorSnapshot& out) const
{
    memcpy(&out, &s_, sizeof(out));
}

// --- USB transport state -----------------------------------------------------

void MonitorState::onUsbDevicePresent(uint16_t vid, uint16_t pid)
{
    s_.usb_device_present = true;
    s_.usb_vid            = vid;
    s_.usb_pid            = pid;
    ++s_.usb_connect_count;
}

void MonitorState::onUsbEndpointReady(uint8_t endpoint_addr, uint16_t max_packet, uint8_t interval_ms)
{
    s_.usb_endpoint_addr = endpoint_addr;
    s_.usb_max_packet    = max_packet;
    s_.usb_interval_ms   = interval_ms;
    // Published last, deliberately. Plan section 2.2: do not advertise
    // "connected" until the endpoints are actually usable.
    s_.usb_endpoint_ready = true;
}

void MonitorState::onUsbDisconnected()
{
    if (s_.usb_device_present || s_.usb_endpoint_ready) ++s_.usb_disconnect_count;

    s_.usb_device_present = false;
    s_.usb_endpoint_ready = false;
    s_.usb_interval_ms    = 0;
    s_.usb_max_packet     = 0;
    s_.usb_endpoint_addr  = 0;

    // Held notes belong to the device that is now gone. Leaving them latched
    // would show a permanently stuck note after an unplug -- exactly the kind
    // of thing the hot-replug exit criterion is looking for.
    memset(note_held_, 0, sizeof(note_held_));
    s_.active_note_count  = 0;
    s_.lowest_active_note = 0xFF;
}

// --- Note tracking -----------------------------------------------------------

void MonitorState::noteOn(uint8_t note)
{
    if (note >= 128) return;

    const uint8_t mask = static_cast<uint8_t>(1u << (note & 7));
    uint8_t&      cell = note_held_[note >> 3];

    if ((cell & mask) == 0)
    {
        cell = static_cast<uint8_t>(cell | mask);
        if (s_.active_note_count < 128) ++s_.active_note_count;
    }

    if (s_.lowest_active_note == 0xFF || note < s_.lowest_active_note)
    {
        s_.lowest_active_note = note;
    }
}

void MonitorState::noteOff(uint8_t note)
{
    if (note >= 128) return;

    const uint8_t mask = static_cast<uint8_t>(1u << (note & 7));
    uint8_t&      cell = note_held_[note >> 3];

    if ((cell & mask) != 0)
    {
        cell = static_cast<uint8_t>(cell & ~mask);
        if (s_.active_note_count > 0) --s_.active_note_count;
    }

    if (note != s_.lowest_active_note) return;

    // The lowest note was released; find the new one.
    s_.lowest_active_note = 0xFF;
    for (uint8_t i = 0; i < 16; ++i)
    {
        if (note_held_[i] == 0) continue;
        for (uint8_t b = 0; b < 8; ++b)
        {
            if (note_held_[i] & (1u << b))
            {
                s_.lowest_active_note = static_cast<uint8_t>(i * 8 + b);
                return;
            }
        }
    }
}

// --- Controllers -------------------------------------------------------------

void MonitorState::recordCc(const MidiMessage& m, uint32_t now_ms)
{
    // Existing slot for this controller?
    for (uint8_t i = 0; i < kMonitorCcSlots; ++i)
    {
        CcSlot& slot = s_.ccs[i];
        if (!slot.used || slot.number != m.data1) continue;

        slot.value   = m.data2;
        slot.channel = m.channel;
        slot.last_ms = now_ms;
        ++slot.updates;
        return;
    }

    // Free slot?
    for (uint8_t i = 0; i < kMonitorCcSlots; ++i)
    {
        CcSlot& slot = s_.ccs[i];
        if (slot.used) continue;

        slot.used    = true;
        slot.number  = m.data1;
        slot.value   = m.data2;
        slot.channel = m.channel;
        slot.last_ms = now_ms;
        slot.updates = 1;
        return;
    }

    // Full: recycle the least recently updated. A device that sends six
    // controllers should show its six current values, not the first five it
    // ever sent.
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < kMonitorCcSlots; ++i)
    {
        // Unsigned difference handles the millis() wrap without a special case.
        if ((now_ms - s_.ccs[i].last_ms) > (now_ms - s_.ccs[oldest].last_ms)) oldest = i;
    }

    CcSlot& slot = s_.ccs[oldest];
    slot.number  = m.data1;
    slot.value   = m.data2;
    slot.channel = m.channel;
    slot.last_ms = now_ms;
    slot.updates = 1;
}

// --- Message fold ------------------------------------------------------------

void MonitorState::onMessage(const MidiMessage& m, uint32_t now_ms, uint32_t now_us)
{
    ++s_.messages;
    ++rate_window_count_;

    if (m.isChannelMessage() && m.channel < 16)
    {
        s_.channels_seen = static_cast<uint16_t>(s_.channels_seen | (1u << m.channel));
    }

    switch (m.type)
    {
        case MidiType::NoteOn:
            noteOn(m.data1);
            s_.note_seen         = true;
            s_.last_note         = m.data1;
            s_.last_velocity     = m.data2;
            s_.last_note_channel = m.channel;
            s_.last_note_was_on  = true;
            break;

        case MidiType::NoteOff:
            noteOff(m.data1);
            s_.note_seen         = true;
            s_.last_note         = m.data1;
            s_.last_velocity     = m.data2;
            s_.last_note_channel = m.channel;
            s_.last_note_was_on  = false;
            break;

        case MidiType::ControlChange:
            if (m.data1 == kBreathCc)
            {
                if (s_.breath_seen)
                {
                    // A gap of zero means "same drain pass", not "simultaneous":
                    // the whole batch shares one timestamp. Skipping those is
                    // what makes this a bound on burst spacing rather than a
                    // rate. breath_per_sec below is the rate.
                    const uint32_t gap = now_us - s_.breath_last_us;
                    if (gap > 0 && gap < s_.breath_min_interval_us)
                    {
                        s_.breath_min_interval_us = gap;
                    }
                }
                ++breath_window_count_;
                s_.breath_seen    = true;
                s_.breath         = m.data2;
                s_.breath_channel = m.channel;
                s_.breath_last_us = now_us;
                ++s_.breath_updates;
            }
            else
            {
                recordCc(m, now_ms);
            }
            break;

        case MidiType::PitchBend:
        {
            const uint16_t v = m.bend14();
            if (!s_.bend_seen)
            {
                s_.bend_min = v;
                s_.bend_max = v;
            }
            else
            {
                if (v < s_.bend_min) s_.bend_min = v;
                if (v > s_.bend_max) s_.bend_max = v;
            }
            s_.bend_seen    = true;
            s_.bend14       = v;
            s_.bend_channel = m.channel;
            break;
        }

        case MidiType::SystemRealtime:
            ++s_.realtime_messages;
            break;

        case MidiType::PolyPressure:
        case MidiType::ProgramChange:
        case MidiType::ChannelPressure:
        case MidiType::SystemCommon:
        case MidiType::None:
            break;
    }
}

void MonitorState::onUnparsed()
{
    ++s_.messages_unparsed;
}

// --- Health ------------------------------------------------------------------

void MonitorState::onDrain(uint16_t queue_depth, uint16_t queue_capacity, uint16_t batch)
{
    s_.queue_capacity = queue_capacity;

    if (queue_depth > s_.queue_hwm) s_.queue_hwm = queue_depth;

    // The transport discards silently when its ring fills and tells no one, so
    // a true overrun count is not observable from here (see usb_midi_host.h).
    // Reaching capacity-1 is the closest thing to a warning we can produce, and
    // it fires before data is lost rather than after.
    if (queue_capacity > 1 && queue_depth >= static_cast<uint16_t>(queue_capacity - 1))
    {
        ++s_.queue_near_full;
    }

    if (batch > s_.drain_max_batch) s_.drain_max_batch = batch;
}

void MonitorState::tick(uint32_t now_ms)
{
    if (rate_window_start_ms_ == 0)
    {
        rate_window_start_ms_ = now_ms;
        return;
    }

    const uint32_t elapsed = now_ms - rate_window_start_ms_;
    if (elapsed < kRateWindowMs) return;

    s_.messages_per_sec   = (rate_window_count_ * 1000UL) / elapsed;
    s_.breath_per_sec     = (breath_window_count_ * 1000UL) / elapsed;
    rate_window_count_    = 0;
    breath_window_count_  = 0;
    rate_window_start_ms_ = now_ms;
}
