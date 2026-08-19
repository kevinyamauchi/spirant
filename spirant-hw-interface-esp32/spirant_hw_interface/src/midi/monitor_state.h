#pragma once

#include <stdint.h>

#include "midi_types.h"

// --- MIDI monitor state (plan sections 2.0 and 2.4) --------------------------
//
// The shared struct plan section 2.0 warns about: written by the drain loop,
// read by the UI task to render. Semantics are "latest value wins", so this is
// a snapshot, not a queue -- section 2.0 is explicit that the queue machinery
// does not belong here.
//
// **This class does no locking of its own, on purpose.** It is Arduino-free so
// tests/ can compile it with the system compiler, which rules out FreeRTOS
// primitives. Serialisation lives in monitor_access.h, one layer up, and the
// reason it cannot be a lock-free seqlock is documented there -- it is not the
// obvious answer.
//
// ParameterValues is untouched by any of this and stays single-writer, as
// section 2.0 requires.

/// One tracked controller other than CC2, which has its own display element.
struct CcSlot
{
    uint8_t  number;
    uint8_t  value;
    uint8_t  channel;
    uint32_t updates;
    uint32_t last_ms;
    bool     used;
};

/// How many distinct non-breath controllers the monitor remembers. Beyond this
/// the least recently updated slot is recycled, so a chatty device shows its
/// current controllers rather than the first ones it happened to send.
static const uint8_t kMonitorCcSlots = 5;

/// Everything the monitor page renders. Plain old data: no pointers, no
/// virtuals, memcpy-able, which is what lets the reader copy it under a lock
/// and then render at its leisure.
struct MonitorSnapshot
{
    // -- USB transport state.
    // `endpoint_ready` is the one to trust. See usb_midi_host.cpp for why
    // "device connected" and "endpoints usable" are genuinely different here
    // (plan section 2.2's trap, which this library reproduces faithfully).
    bool     usb_device_present;
    bool     usb_endpoint_ready;
    uint16_t usb_vid;
    uint16_t usb_pid;
    uint8_t  usb_interval_ms;   ///< bInterval from the IN endpoint descriptor.
    uint16_t usb_max_packet;
    uint8_t  usb_endpoint_addr;
    uint32_t usb_connect_count;
    uint32_t usb_disconnect_count;

    // -- Notes.
    bool    note_seen;
    uint8_t last_note;
    uint8_t last_velocity;
    uint8_t last_note_channel;
    bool    last_note_was_on;
    uint8_t active_note_count;
    uint8_t lowest_active_note;   ///< 0xFF when nothing is held.

    // -- Breath (CC2), the design's central signal.
    bool     breath_seen;
    uint8_t  breath;
    uint8_t  breath_channel;
    uint32_t breath_updates;

    /// CC2 messages per second, averaged over a 1 s window.
    ///
    /// **This is the authoritative CC2 rate** -- the figure plan section 2.5
    /// reports back into the section 6.4 bandwidth budget, and the one to divide
    /// the ring capacity by for section 2.3's margin. Prefer it over
    /// breath_min_interval_us, for the reason given there.
    uint32_t breath_per_sec;

    /// Smallest observed gap between two CC2 messages, microseconds.
    ///
    /// **Reads long.** Every message in one drain pass is folded in with the
    /// same timestamp, so this can only resolve gaps *between* drain passes --
    /// two CC2s arriving in the same 1 ms drain register as no gap at all and
    /// are skipped. It is a useful lower bound on burst spacing and a bad
    /// measure of rate. Use breath_per_sec for rate.
    uint32_t breath_min_interval_us;
    uint32_t breath_last_us;

    // -- Pitch bend.
    bool     bend_seen;
    uint16_t bend14;              ///< 0..16383, centre 8192.
    uint16_t bend_min;
    uint16_t bend_max;
    uint8_t  bend_channel;

    // -- Other controllers.
    CcSlot ccs[kMonitorCcSlots];

    // -- Traffic.
    uint16_t channels_seen;    ///< Bit per channel 0-15.
    uint32_t messages;
    uint32_t messages_unparsed;
    uint32_t messages_per_sec;
    uint32_t realtime_messages;

    // -- Ring-buffer health (plan section 2.3).
    uint16_t queue_capacity;
    uint16_t queue_hwm;        ///< Deepest the transport ring was ever seen.
    uint32_t queue_near_full;  ///< Times the ring was within one slot of full.
    uint16_t drain_max_batch;  ///< Most packets consumed in a single drain.
};

class MonitorState
{
  public:
    MonitorState() { reset(); }

    void reset();

    // -- Writer side. All of these are called from the drain loop.

    void onUsbDevicePresent(uint16_t vid, uint16_t pid);
    void onUsbEndpointReady(uint8_t endpoint_addr, uint16_t max_packet, uint8_t interval_ms);
    void onUsbDisconnected();

    /// Fold one decoded message in. `now_us` is used only for the breath
    /// interval measurement plan section 2.5 asks for; pass micros().
    void onMessage(const MidiMessage& m, uint32_t now_ms, uint32_t now_us);

    /// A packet the parser declined to model. Counted, not dropped silently.
    void onUnparsed();

    /// Reports the transport ring's depth as observed before a drain pass, and
    /// how many packets that pass consumed.
    void onDrain(uint16_t queue_depth, uint16_t queue_capacity, uint16_t batch);

    // Drain lateness is deliberately not tracked here: ui_task::reportLoad()
    // already carries exactly that number to the footer and the heartbeat, and
    // two mechanisms for one measurement is one too many.

    /// Rolls the messages-per-second window. Call once per drain pass; it is
    /// cheap and self-throttling.
    void tick(uint32_t now_ms);

    // -- Reader side.

    const MonitorSnapshot& data() const { return s_; }
    void                   copyTo(MonitorSnapshot& out) const;

  private:
    void noteOn(uint8_t note);
    void noteOff(uint8_t note);
    void recordCc(const MidiMessage& m, uint32_t now_ms);

    MonitorSnapshot s_;

    uint32_t rate_window_start_ms_;
    uint32_t rate_window_count_;
    uint32_t breath_window_count_;

    /// Which notes are currently held, so the active count survives overlapping
    /// note-ons and out-of-order note-offs. 16 bytes, and it makes the count
    /// correct rather than approximately correct.
    uint8_t note_held_[16];
};
