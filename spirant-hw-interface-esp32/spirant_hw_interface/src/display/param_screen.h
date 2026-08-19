#pragma once

#include <stdint.h>

#include "../params/parameter_values.h"
#include "screen.h"

// The parameter page (plan section 1.3), now behind the Screen interface.
//
//   y 29  +--------+----------+----------+----------+
//         | Res    | Brite    | Floor    | Pass     |
//         |   35   |    66    |   150    |    0     |   columns (122)
//         |   %    |    %     |    Hz    |    %     |
//   y 152 +--------+----------+----------+----------+
//
// Behaviour is unchanged from phase 1: consume the display change flags,
// repaint the whole body on a page switch, otherwise repaint only the columns
// whose values moved.

class ParamScreen : public Screen
{
  public:
    explicit ParamScreen(ParameterValues& params) : params_(params) {}

    const char* title() const override;
    const char* headerNote(char* buf, unsigned buf_len) const override;

    bool needsFullRepaint() const override { return params_.currentPage() != last_page_; }

    void renderFull(DisplayManager& dm) override;
    void renderIncremental(DisplayManager& dm) override;
    void onEnter() override { last_page_ = kNoPage; }

    /// Timing sweep for plan section 1.3. Logs min/avg/max for a single value
    /// region and for a full page redraw, and reports whether the sprite push
    /// is genuinely asynchronous. Leaves the display showing the saved page.
    void profile(DisplayManager& dm);

    /// One column, exposed so profile() can drive it directly.
    void renderColumn(DisplayManager& dm, uint8_t encoder);

  private:
    static const uint8_t kNoPage = 0xFF;

    void renderBody(DisplayManager& dm);
    void drawChrome(DisplayManager& dm);

    ParameterValues& params_;
    uint8_t          last_page_ = kNoPage;
};
