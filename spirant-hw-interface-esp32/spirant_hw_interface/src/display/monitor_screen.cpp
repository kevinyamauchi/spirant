#include "monitor_screen.h"

#include <Arduino.h>
#include <stdio.h>

#include "../config.h"
#include "../midi/monitor_access.h"
#include "display_manager.h"

namespace
{

// -- Row geometry. Body runs y 30..151; the footer's rule is at 153.
//
// Labels are Font2, 16 px tall, and are drawn straight to the panel rather than
// into the bar regions. Every row below is checked against the next one's
// start: the label bands are [60,75] and [98,113], which is why the bars begin
// at 76 and 114 and not a pixel earlier.
const int16_t kNoteY = theme::kBodyY;  // 30 .. 57
const int16_t kNoteH = 28;

const int16_t kBreathLabelY = 60;  // 60 .. 75
const int16_t kBreathBarY   = 76;  // 76 .. 95
const int16_t kBreathBarH   = 20;

const int16_t kBendLabelY = 98;   // 98 .. 113
const int16_t kBendBarY   = 114;  // 114 .. 127
const int16_t kBendBarH   = 14;

const int16_t kCcY = 130;  // 130 .. 149
const int16_t kCcH = 20;

static_assert(kNoteY + kNoteH < kBreathLabelY, "note row overlaps breath label");
static_assert(kBreathLabelY + 16 <= kBreathBarY, "breath label overlaps its bar");
static_assert(kBreathBarY + kBreathBarH <= kBendLabelY, "breath bar overlaps bend label");
static_assert(kBendLabelY + 16 <= kBendBarY, "bend label overlaps its bar");
static_assert(kBendBarY + kBendBarH <= kCcY, "bend bar overlaps cc row");
static_assert(kCcY + kCcH <= theme::kScreenH - theme::kFooterH - 2, "cc row overlaps footer");
static_assert(kNoteH <= DisplayManager::kScratchH && kBreathBarH <= DisplayManager::kScratchH &&
                  kBendBarH <= DisplayManager::kScratchH && kCcH <= DisplayManager::kScratchH,
              "a region is taller than the shared scratch sprite");

// -- Bar metrics. The numeric value sits inside the same region as its bar, so
// one push covers both and they can never disagree on screen.
const int16_t kBarX     = 8;
const int16_t kBarRight = 250;
const int16_t kBarW     = kBarRight - kBarX;
const int16_t kValueX   = theme::kScreenW - 8;

/// "C#4" for 61, with C4 = middle C = note 60.
void noteName(uint8_t note, char* buf, size_t buf_len)
{
    static const char* const kNames[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
    };
    snprintf(buf, buf_len, "%s%d", kNames[note % 12], static_cast<int>(note / 12) - 1);
}

/// Bar outline plus a proportional fill. `filled` is in pixels, already clamped.
void drawBar(lgfx::LGFX_Sprite& spr,
             int16_t            x,
             int16_t            y,
             int16_t            w,
             int16_t            h,
             int16_t            fill_x,
             int16_t            fill_w,
             uint16_t           colour)
{
    spr.drawRect(x, y, w, h, theme::kBarEdge);
    spr.fillRect(x + 1, y + 1, w - 2, h - 2, theme::kBarBg);
    if (fill_w > 0) spr.fillRect(fill_x, y + 1, fill_w, h - 2, colour);
}

}  // namespace

// --- Header ------------------------------------------------------------------

const char* MonitorScreen::headerNote(char* buf, unsigned buf_len) const
{
    if (snap_.usb_endpoint_ready)
    {
        snprintf(buf, buf_len, "%04X:%04X", snap_.usb_vid, snap_.usb_pid);
    }
    else if (snap_.usb_device_present)
    {
        // Enumerated but no usable MIDI IN endpoint. Worth its own wording:
        // "connected" would be true and misleading (plan section 2.2).
        snprintf(buf, buf_len, "no MIDI ep");
    }
    else
    {
        snprintf(buf, buf_len, "no device");
    }
    return buf;
}

// --- Lifecycle ---------------------------------------------------------------

void MonitorScreen::poll()
{
    // A miss leaves snap_ as it was and the page renders the previous values --
    // stale by one tick at worst, and never a stalled UI task.
    monitor::snapshot(snap_, MONITOR_LOCK_TIMEOUT_MS);
}

void MonitorScreen::onEnter()
{
    drawn_ = false;
}

bool MonitorScreen::needsFullRepaint() const
{
    // Connection transitions rewrite the header note as well as the body, and
    // they are rare, so they take the full path rather than growing a special
    // case for the header.
    return snap_.usb_endpoint_ready != d_endpoint_ready_ ||
           snap_.usb_device_present != d_device_present_;
}

uint32_t MonitorScreen::ccDigest() const
{
    uint32_t d = 0;
    for (uint8_t i = 0; i < kMonitorCcSlots; ++i)
    {
        const CcSlot& c = snap_.ccs[i];
        d = d * 31u + (c.used ? (static_cast<uint32_t>(c.number) << 8 | c.value) + 1u : 0u);
    }
    return d;
}

// --- Regions -----------------------------------------------------------------

void MonitorScreen::renderNoteRow(DisplayManager& dm)
{
    lgfx::LGFX_Sprite& spr = dm.scratch();

    spr.fillSprite(theme::kBg);
    spr.setTextDatum(middle_left);

    if (!snap_.note_seen)
    {
        spr.setFont(&fonts::Font2);
        spr.setTextColor(theme::kNullFg, theme::kBg);
        spr.drawString("no notes yet", kBarX, kNoteH / 2);
    }
    else
    {
        char name[8];
        noteName(snap_.last_note, name, sizeof(name));

        spr.setFont(&fonts::Font4);
        spr.setTextColor(snap_.last_note_was_on ? theme::kValueFg : theme::kNullFg, theme::kBg);
        spr.drawString(name, kBarX, kNoteH / 2);

        char detail[40];
        snprintf(detail,
                 sizeof(detail),
                 "%s  vel %u  ch%u",
                 snap_.last_note_was_on ? "on " : "off",
                 snap_.last_velocity,
                 static_cast<unsigned>(snap_.last_note_channel) + 1u);

        spr.setFont(&fonts::Font2);
        spr.setTextColor(theme::kLabelFg, theme::kBg);
        spr.drawString(detail, kBarX + 62, kNoteH / 2);
    }

    char held[16];
    snprintf(held, sizeof(held), "held %u", snap_.active_note_count);
    spr.setFont(&fonts::Font2);
    spr.setTextColor(snap_.active_note_count > 0 ? theme::kValueFg : theme::kUnitFg, theme::kBg);
    spr.setTextDatum(middle_right);
    spr.drawString(held, kValueX, kNoteH / 2);

    dm.pushScratch(0, kNoteY, theme::kScreenW, kNoteH);
}

void MonitorScreen::renderBreathBar(DisplayManager& dm)
{
    lgfx::LGFX_Sprite& spr = dm.scratch();

    spr.fillSprite(theme::kBg);

    const int16_t fill = static_cast<int16_t>((static_cast<int32_t>(snap_.breath) * (kBarW - 2)) / 127);
    drawBar(spr, kBarX, 0, kBarW, kBreathBarH, kBarX + 1, fill, theme::kBarFg);

    char value[8];
    if (snap_.breath_seen)
    {
        snprintf(value, sizeof(value), "%u", snap_.breath);
    }
    else
    {
        snprintf(value, sizeof(value), "--");
    }

    spr.setFont(&fonts::Font4);
    spr.setTextColor(snap_.breath_seen ? theme::kBarFg : theme::kNullFg, theme::kBg);
    spr.setTextDatum(middle_right);
    spr.drawString(value, kValueX, kBreathBarH / 2);

    dm.pushScratch(0, kBreathBarY, theme::kScreenW, kBreathBarH);
}

void MonitorScreen::renderBendBar(DisplayManager& dm)
{
    lgfx::LGFX_Sprite& spr = dm.scratch();

    spr.fillSprite(theme::kBg);

    // Bipolar: the fill grows from the centre detent in whichever direction the
    // wheel went, so "no bend" is visibly a centred hairline rather than empty.
    const int16_t inner   = kBarW - 2;
    const int16_t centre  = static_cast<int16_t>(kBarX + 1 + inner / 2);
    const int32_t signed_ = static_cast<int32_t>(snap_.bend14) - 8192;
    int16_t       extent  = static_cast<int16_t>((signed_ * (inner / 2)) / 8192);

    if (extent > inner / 2) extent = inner / 2;
    if (extent < -(inner / 2)) extent = static_cast<int16_t>(-(inner / 2));

    const int16_t fill_x = (extent >= 0) ? centre : static_cast<int16_t>(centre + extent);
    const int16_t fill_w = (extent >= 0) ? extent : static_cast<int16_t>(-extent);

    drawBar(spr, kBarX, 0, kBarW, kBendBarH, fill_x, fill_w, theme::kBendFg);
    spr.drawFastVLine(centre, 1, kBendBarH - 2, theme::kCentreFg);

    char value[12];
    if (snap_.bend_seen)
    {
        snprintf(value, sizeof(value), "%+ld", static_cast<long>(signed_));
    }
    else
    {
        snprintf(value, sizeof(value), "--");
    }

    spr.setFont(&fonts::Font2);
    spr.setTextColor(snap_.bend_seen ? theme::kBendFg : theme::kNullFg, theme::kBg);
    spr.setTextDatum(middle_right);
    spr.drawString(value, kValueX, kBendBarH / 2);

    dm.pushScratch(0, kBendBarY, theme::kScreenW, kBendBarH);
}

void MonitorScreen::renderCcRow(DisplayManager& dm)
{
    lgfx::LGFX_Sprite& spr = dm.scratch();

    spr.fillSprite(theme::kBg);
    spr.setFont(&fonts::Font2);
    spr.setTextDatum(middle_left);

    int16_t x     = kBarX;
    bool    any   = false;
    const int16_t cy = kCcH / 2;

    for (uint8_t i = 0; i < kMonitorCcSlots; ++i)
    {
        const CcSlot& c = snap_.ccs[i];
        if (!c.used) continue;

        char text[16];
        snprintf(text, sizeof(text), "cc%u", c.number);
        spr.setTextColor(theme::kUnitFg, theme::kBg);
        spr.drawString(text, x, cy);
        x = static_cast<int16_t>(x + spr.textWidth(text) + 3);

        snprintf(text, sizeof(text), "%u", c.value);
        spr.setTextColor(theme::kLabelFg, theme::kBg);
        spr.drawString(text, x, cy);
        x = static_cast<int16_t>(x + spr.textWidth(text) + 14);

        any = true;
        if (x > theme::kScreenW - 40) break;
    }

    if (!any)
    {
        spr.setTextColor(theme::kNullFg, theme::kBg);
        spr.drawString("no other controllers", kBarX, cy);
    }

    dm.pushScratch(0, kCcY, theme::kScreenW, kCcH);
}

void MonitorScreen::renderLabels(DisplayManager& dm)
{
    // Static text: drawn straight to the panel on a full repaint and never
    // touched again. Keeping it out of the bar regions is what lets the breath
    // bar be 22 px tall instead of 36.
    LGFX_TDisplayS3& lcd = dm.lcd();

    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(top_left);

    lcd.setTextColor(theme::kLabelFg, theme::kBg);
    lcd.drawString("BREATH  CC2", kBarX, kBreathLabelY);
    lcd.drawString("BEND", kBarX, kBendLabelY);

    lcd.drawFastHLine(0, kNoteY + kNoteH, theme::kScreenW, theme::kRule);
}

// --- Screen interface --------------------------------------------------------

void MonitorScreen::renderFull(DisplayManager& dm)
{
    renderLabels(dm);
    renderNoteRow(dm);
    renderBreathBar(dm);
    renderBendBar(dm);
    renderCcRow(dm);

    d_note_          = snap_.last_note;
    d_velocity_      = snap_.last_velocity;
    d_note_chan_     = snap_.last_note_channel;
    d_note_on_       = snap_.last_note_was_on;
    d_note_seen_     = snap_.note_seen;
    d_active_        = snap_.active_note_count;
    d_breath_        = snap_.breath;
    d_breath_seen_   = snap_.breath_seen;
    d_bend_          = snap_.bend14;
    d_bend_seen_     = snap_.bend_seen;
    d_cc_digest_     = ccDigest();
    d_endpoint_ready_ = snap_.usb_endpoint_ready;
    d_device_present_ = snap_.usb_device_present;
    drawn_            = true;
}

void MonitorScreen::renderIncremental(DisplayManager& dm)
{
    if (!drawn_) return;

    if (snap_.last_note != d_note_ || snap_.last_velocity != d_velocity_ ||
        snap_.last_note_channel != d_note_chan_ || snap_.last_note_was_on != d_note_on_ ||
        snap_.note_seen != d_note_seen_ || snap_.active_note_count != d_active_)
    {
        renderNoteRow(dm);
        d_note_      = snap_.last_note;
        d_velocity_  = snap_.last_velocity;
        d_note_chan_ = snap_.last_note_channel;
        d_note_on_   = snap_.last_note_was_on;
        d_note_seen_ = snap_.note_seen;
        d_active_    = snap_.active_note_count;
    }

    if (snap_.breath != d_breath_ || snap_.breath_seen != d_breath_seen_)
    {
        renderBreathBar(dm);
        d_breath_      = snap_.breath;
        d_breath_seen_ = snap_.breath_seen;
    }

    if (snap_.bend14 != d_bend_ || snap_.bend_seen != d_bend_seen_)
    {
        renderBendBar(dm);
        d_bend_      = snap_.bend14;
        d_bend_seen_ = snap_.bend_seen;
    }

    const uint32_t digest = ccDigest();
    if (digest != d_cc_digest_)
    {
        renderCcRow(dm);
        d_cc_digest_ = digest;
    }
}
