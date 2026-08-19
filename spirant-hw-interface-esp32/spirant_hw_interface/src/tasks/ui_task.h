#pragma once

#include <stdint.h>

// The UI task (plan section 1.8).
//
// Owns the display, the encoders and the parameter model, and is the ONLY
// thing that writes to the panel. Pinned to core 1 at a priority above the
// Arduino loopTask, so a busy loop() cannot delay encoder servicing -- the
// relationship section 1.8 asks us to validate before there is real MIDI
// traffic to jitter it.

namespace ui_task
{

/// Brings up the display, surveys I2C, brings up the encoders, then starts the
/// task. Call from setup() after log_init(). Returns false if the task itself
/// could not be created; display or encoder failures are logged, surfaced on
/// screen, and do not stop the rest from running.
bool start();

/// Snapshot for the stats heartbeat. Plain reads of counters that only the UI
/// task writes -- torn values would only misreport a diagnostic, so no lock.
struct Stats
{
    uint32_t wakes;
    uint32_t services;
    uint32_t isr_count;
    uint32_t detents;
    uint32_t glitches;
    uint32_t i2c_errors;
    uint32_t worst_service_latency_us;  ///< ISR to service start.
    uint32_t worst_value_us;            ///< Single value-region redraw.
    uint32_t worst_page_us;             ///< Full page redraw.
    uint8_t  page;
    bool     display_ok;
    bool     encoders_ok;
    bool     showing_monitor;  ///< Which screen is on the panel.
};

void snapshot(Stats& out);

/// Report the drain loop's worst scheduling lateness, so it can be shown on the
/// display next to the UI task's own figures (plan section 1.8: confirm neither
/// side starves the other).
///
/// Phase 1's synthetic load fed this; phase 2 points it at the real MIDI drain,
/// which is the same instrumentation for free (plan section 2.3).
void reportLoad(uint32_t worst_late_us, uint32_t iterations);

}  // namespace ui_task
