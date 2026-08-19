#pragma once

#include <stdint.h>

#include "../midi/monitor_state.h"
#include "screen.h"

// The MIDI monitor page -- the phase 2 deliverable (plan section 2.4).
//
//   y 0   +-------------------------------------------------+
//         |  MIDI Monitor                     0E41:0001     |  header (28)
//   y 30  |  C#4  on  vel 96  ch1                 held 1    |  note   (28)
//   y 60  |  BREATH  CC2                                    |  label  (16)
//   y 76  |  [##################......................] 96  |  bar    (20)
//   y 98  |  BEND                                           |  label  (16)
//   y 114 |  [.............|####......................] +231 |  bar    (14)
//   y 130 |  cc74 32   cc11 100   cc65 0                    |  ccs    (20)
//   y 154 |  412/s  ring 7/64  near 0  late 180us           |  footer (16)
//
// **No separate rate limiter**, contrary to the plan's original section 2.4:
// the UI task's existing 20 ms tick already is one, at 50 Hz, and the section
// 1.3 measurements show the whole page fits inside it. Budgeting at the
// measured 13.9 MB/s:
//
//   note row     320x28  17,920 B  ~1,290 us
//   breath bar   320x20  12,800 B    ~920 us
//   bend bar     320x14   8,960 B    ~645 us
//   cc row       320x20  12,800 B    ~920 us
//   ------------------------------------------
//   all four in one tick             ~3.78 ms   = 19% of the tick
//
// and that is the case where everything changes at once. Breath alone, the
// common case, is ~1 ms. The display is not on the parse path at all, so it
// cannot become a bottleneck for MIDI throughput.
//
// If a bar ever needs to track the full breath rate rather than the tick, the
// lever is redrawing only the changed segment of the bar -- roughly 40x cheaper.
// Not needed here.

class MonitorScreen : public Screen
{
  public:
    const char* title() const override { return "MIDI Monitor"; }
    const char* headerNote(char* buf, unsigned buf_len) const override;

    void poll() override;
    bool needsFullRepaint() const override;

    void renderFull(DisplayManager& dm) override;
    void renderIncremental(DisplayManager& dm) override;
    void onEnter() override;

    /// The snapshot this screen last rendered from, for the footer and the
    /// stats heartbeat. Refreshed by poll().
    const MonitorSnapshot& snapshot() const { return snap_; }

  private:
    void renderNoteRow(DisplayManager& dm);
    void renderBreathBar(DisplayManager& dm);
    void renderBendBar(DisplayManager& dm);
    void renderCcRow(DisplayManager& dm);
    void renderLabels(DisplayManager& dm);

    /// Cheap digest of the CC slots, so the row is repainted when any of them
    /// moves without comparing five structs field by field every tick.
    uint32_t ccDigest() const;

    MonitorSnapshot snap_{};

    // -- Last values actually drawn, so an unchanged region costs nothing.
    bool     drawn_        = false;
    uint8_t  d_note_       = 0;
    uint8_t  d_velocity_   = 0;
    uint8_t  d_note_chan_  = 0;
    bool     d_note_on_    = false;
    bool     d_note_seen_  = false;
    uint8_t  d_active_     = 0;
    uint8_t  d_breath_     = 0;
    bool     d_breath_seen_ = false;
    uint16_t d_bend_       = 8192;
    bool     d_bend_seen_  = false;
    uint32_t d_cc_digest_  = 0;

    /// Connection state at the last full repaint. A change here rewrites the
    /// header note as well as the body, so it takes the full path.
    bool d_endpoint_ready_ = false;
    bool d_device_present_ = false;
};
