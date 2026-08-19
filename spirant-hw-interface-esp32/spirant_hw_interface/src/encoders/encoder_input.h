#pragma once

#include <Adafruit_seesaw.h>
#include <stdint.h>

#include "../params/param_specs.h"

// Seesaw quad rotary encoder input (plan sections 1.5 and 1.6).
//
// Two things the Adafruit library does not do for us:
//
//  1. Level-triggered host interrupt. The Seesaw INT line is open-drain and
//     stays asserted until its INTFLAG register is read. An edge-triggered
//     host GPIO would miss a change that arrives while an earlier one is
//     being serviced; a level-triggered one that is not masked re-fires
//     continuously and locks the device. So: LOW_LEVEL, disable on ISR entry,
//     re-enable after servicing.
//
//  2. Source identification. The Seesaw cannot tell us which encoder fired,
//     so every wake reads all four encoders and all four switches and diffs
//     against a cache. That is also what makes the two-encoders-at-once case
//     work by construction.

class EncoderInput
{
  public:
    /// What one service pass observed.
    struct Events
    {
        int32_t deltas[kParamsPerPage];   ///< Signed detents since last pass.
        bool    pressed[kParamsPerPage];  ///< Fresh, debounced press edge.
        bool    any;                      ///< True if anything at all moved.
    };

    /// Brings up the Seesaw, seeds the position cache, configures the switch
    /// GPIOs, and (if ENCODER_USE_INTERRUPT) installs the host ISR that
    /// notifies `notify_task`. Wire must already be started.
    /// Returns false if the Seesaw did not answer.
    bool begin(TaskHandle_t notify_task);

    /// Read all four encoders and switches, diff against cache, clear the
    /// Seesaw interrupt flags. Safe to call on a bare timer tick with no
    /// interrupt pending -- it just reports no events.
    void service(Events& out);

    /// Re-arm the host interrupt. Call after service(), from the task, never
    /// from the ISR.
    void rearm();

    bool present() const { return present_; }

    // -- Diagnostics, for the stats heartbeat and the 60 s stress test.
    uint32_t isrCount() const;
    /// micros() timestamp taken inside the last ISR. Subtracting this at the
    /// top of service() gives the ISR-to-service latency that plan section 1.8
    /// asks us to keep small under load.
    uint32_t lastIsrUs() const;
    uint32_t serviceCount() const { return service_count_; }
    uint32_t detentCount() const { return detent_count_; }
    uint32_t glitchCount() const { return glitch_count_; }
    uint32_t i2cErrorCount() const { return i2c_error_count_; }

    /// Cheap liveness probe -- an address-only transaction. Increments the
    /// error count on failure.
    bool probe();

  private:
    Adafruit_seesaw ss_;

    int32_t  last_pos_[kParamsPerPage]  = {0, 0, 0, 0};
    bool     last_btn_[kParamsPerPage]  = {false, false, false, false};
    uint32_t last_press_ms_[kParamsPerPage] = {0, 0, 0, 0};

    bool     present_         = false;
    uint32_t service_count_   = 0;
    uint32_t detent_count_    = 0;
    uint32_t glitch_count_    = 0;
    uint32_t i2c_error_count_ = 0;
};
