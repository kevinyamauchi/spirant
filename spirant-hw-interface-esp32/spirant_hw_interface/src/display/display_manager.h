#pragma once

#include <stdint.h>

#include "lgfx_t_display_s3.h"
#include "screen.h"
#include "theme.h"

// Dirty-region renderer and sprite bank (plan sections 1.3 and 2.4).
//
// Landscape 320x170, shared by every screen:
//
//   y 0   +-------------------------------------------------+
//         |  title                                    note  |  header (28)
//   y 29  +-------------------------------------------------+
//         |                                                 |
//         |                  screen body                    |  body (123)
//         |                                                 |
//   y 152 +-------------------------------------------------+
//         | status line                                     |  footer (16)
//   y 170 +-------------------------------------------------+
//
// Nothing is ever drawn full-screen after the first paint of a screen. A region
// update composes into an off-screen sprite and pushes it in a single transfer
// -- no fill-then-draw flicker.
//
// **Phase 2 change.** This class no longer knows what a parameter is. It owns
// the panel, the sprites and the timing, and delegates content to the active
// Screen (screen.h). ParamScreen carries what update(ParameterValues&) used to
// do; MonitorScreen is the section 2.4 deliverable.
//
// Measured transfer rate is ~13.9 MB/s (19,520 bytes in ~1,420 us), and cost is
// driven by pixel area rather than by how much content changed. Size regions
// with that, not with intuition about how much ink moved.
//
// All rendering happens on the UI task. Never call these from an ISR.

class DisplayManager
{
  public:
    // -- Geometry, re-exported from theme.h so callers need only this header.
    static const int16_t kScreenW = theme::kScreenW;
    static const int16_t kScreenH = theme::kScreenH;
    static const int16_t kHeaderH = theme::kHeaderH;
    static const int16_t kFooterH = theme::kFooterH;
    static const int16_t kBodyY   = theme::kBodyY;
    static const int16_t kBodyH   = theme::kBodyH;

    /// Largest region a screen may compose in the shared scratch sprite.
    /// 320x30 is 19,200 bytes, about 1,380 us to push. Raising it costs RAM and
    /// tick budget; splitting a region across two pushes usually costs neither.
    static const int16_t kScratchW = kScreenW;
    static const int16_t kScratchH = 30;

    /// Powers the panel, initialises the bus, allocates sprites. Returns false
    /// if a sprite allocation failed -- check it, a silently sprite-less
    /// renderer would draw nothing.
    bool begin();

    /// Boot banner, so section 1.0 has a visible end-to-end confirmation on a
    /// board with no user LED.
    void showSplash(const char* line1, const char* line2);

    /// Make `screen` active. Repaints in full on the next update(), which costs
    /// the section 1.3 full-page figure -- screen switches should be deliberate,
    /// not incidental. Passing the already-active screen is a no-op.
    void setScreen(Screen* screen);

    Screen* screen() const { return screen_; }

    /// Render the active screen: a full repaint if invalidated or newly
    /// entered, otherwise whatever the screen decides has changed. The only
    /// entry point the UI task needs.
    void update();

    /// Force a full repaint on the next update().
    void invalidate() { needs_full_ = true; }

    /// Header bar. Screens do not draw it themselves; update() calls this with
    /// the screen's title() and headerNote().
    void drawHeader(const char* title, const char* note);

    /// Footer text. Overwrites the whole footer; printf-style.
    void drawStatus(const char* format, ...) __attribute__((format(printf, 2, 3)));

    // -- Sprite bank ---------------------------------------------------------
    //
    // Shared and reused, so every push must be followed by waitDMA() before the
    // buffer is touched again. pushScratch() and pushColumn() do that for you.

    /// 320x30 general-purpose region buffer. Prepare it, then pushScratch().
    lgfx::LGFX_Sprite& scratch() { return scratch_spr_; }

    /// Push the scratch sprite's top-left w x h to (x, y) and wait for DMA.
    /// Records the elapsed time as a value-region measurement.
    void pushScratch(int16_t x, int16_t y, int16_t w, int16_t h);

    /// 80x122 column buffer, sized for the parameter page.
    lgfx::LGFX_Sprite& column() { return column_spr_; }
    void               pushColumn(int16_t x, int16_t y);

    bool spritesReady() const { return sprites_ready_; }

    LGFX_TDisplayS3& lcd() { return lcd_; }

    // -- Running worst-case figures, microseconds. Live numbers from real
    // traffic, as opposed to a synthetic sweep.
    uint32_t worstValueUs() const { return worst_value_us_; }
    uint32_t worstPageUs() const { return worst_page_us_; }
    void     resetWorstCase();

    /// For screens that time their own composite operations.
    ///
    /// Region pushes made during a full repaint are ignored here: `worstValueUs`
    /// keeps its section 1.3 meaning of "worst single incremental update", so
    /// the phase 1 and phase 2 numbers stay comparable. The full repaint is
    /// already accounted for by worstPageUs().
    void recordValueUs(uint32_t us)
    {
        if (in_full_repaint_) return;
        if (us > worst_value_us_) worst_value_us_ = us;
    }
    void recordPageUs(uint32_t us)
    {
        if (us > worst_page_us_) worst_page_us_ = us;
    }

  private:
    LGFX_TDisplayS3   lcd_;
    lgfx::LGFX_Sprite header_spr_{&lcd_};
    lgfx::LGFX_Sprite column_spr_{&lcd_};
    lgfx::LGFX_Sprite scratch_spr_{&lcd_};

    Screen* screen_          = nullptr;
    bool    needs_full_      = true;
    bool    sprites_ready_   = false;
    bool    in_full_repaint_ = false;

    uint32_t worst_value_us_ = 0;
    uint32_t worst_page_us_  = 0;
};
