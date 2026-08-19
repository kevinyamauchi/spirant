#pragma once

#include "../params/param_specs.h"
#include "lgfx_t_display_s3.h"

// Shared palette and geometry. Both screens draw from this so the parameter
// page and the MIDI monitor read as one instrument rather than two programs
// that happen to share a panel (plan section 2.4).
//
// Dark background so the panel is comfortable next to a stage light, with one
// accent colour carrying whatever is moving.

namespace theme
{

const uint16_t kBg       = lgfx::color565(0, 0, 0);
const uint16_t kHeaderBg = lgfx::color565(20, 26, 38);
const uint16_t kRule     = lgfx::color565(48, 56, 72);
const uint16_t kLabelFg  = lgfx::color565(150, 160, 180);
const uint16_t kValueFg  = lgfx::color565(120, 230, 190);
const uint16_t kUnitFg   = lgfx::color565(96, 104, 120);
const uint16_t kNullFg   = lgfx::color565(56, 62, 74);
const uint16_t kHeaderFg = lgfx::color565(235, 240, 250);
const uint16_t kStatusFg = lgfx::color565(120, 130, 150);

// -- Monitor-page additions.

/// Filled portion of the breath bar. Warm, so breath reads differently from a
/// parameter value at a glance.
const uint16_t kBarFg    = lgfx::color565(255, 176, 80);
/// Unfilled bar interior.
const uint16_t kBarBg    = lgfx::color565(28, 32, 42);
const uint16_t kBarEdge  = lgfx::color565(70, 80, 98);
/// Pitch bend, which is bipolar and deserves its own colour.
const uint16_t kBendFg   = lgfx::color565(130, 190, 255);
/// Bend centre detent mark.
const uint16_t kCentreFg = lgfx::color565(96, 104, 120);
const uint16_t kOkFg     = lgfx::color565(120, 230, 190);
const uint16_t kWarnFg   = lgfx::color565(255, 120, 120);

// -- Screen geometry, landscape (rotation 1).

const int16_t kScreenW = 320;
const int16_t kScreenH = 170;
const int16_t kHeaderH = 28;
const int16_t kFooterH = 16;

/// First y available to a screen's body, and its height, between header and
/// footer.
const int16_t kBodyY = kHeaderH + 2;
const int16_t kBodyH = kScreenH - kFooterH - kBodyY - 2;

/// Parameter-page column, one per encoder. Sized here rather than in
/// ParamScreen because DisplayManager allocates the sprite.
const int16_t kParamColumnW = kScreenW / kParamsPerPage;
const int16_t kParamColumnH = kBodyH;

}  // namespace theme
