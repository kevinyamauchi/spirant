#include "display_manager.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "../config.h"
#include "../log.h"

// --- Setup -------------------------------------------------------------------

bool DisplayManager::begin()
{
    // The panel and the header 3V3 rail are both behind this pin. Nothing
    // renders until it is high (plan section 1.2). setup() already latched it
    // as its very first action -- that is the authoritative one, because on
    // battery power the board browns out if it waits this long. Repeating it
    // here is idempotent and keeps this class usable on its own.
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    delay(20);

    lcd_.init();
    lcd_.setRotation(DISPLAY_ROTATION);
    lcd_.setBrightness(DISPLAY_BRIGHTNESS);
    lcd_.fillScreen(theme::kBg);

    header_spr_.setColorDepth(16);
    header_spr_.setPsram(false);
    column_spr_.setColorDepth(16);
    column_spr_.setPsram(false);
    scratch_spr_.setColorDepth(16);
    scratch_spr_.setPsram(false);

    const bool header_ok  = header_spr_.createSprite(kScreenW, kHeaderH) != nullptr;
    const bool column_ok  = column_spr_.createSprite(theme::kParamColumnW, theme::kParamColumnH) != nullptr;
    const bool scratch_ok = scratch_spr_.createSprite(kScratchW, kScratchH) != nullptr;

    sprites_ready_ = header_ok && column_ok && scratch_ok;

    if (!sprites_ready_)
    {
        slog_printf("display: SPRITE ALLOC FAILED (header=%d column=%d scratch=%d)\n",
                    header_ok,
                    column_ok,
                    scratch_ok);
        return false;
    }

    slog_printf("display: %ldx%ld rot=%d, sprites header=%dx%d column=%dx%d scratch=%dx%d (%u bytes)\n",
                static_cast<long>(lcd_.width()),
                static_cast<long>(lcd_.height()),
                DISPLAY_ROTATION,
                kScreenW,
                kHeaderH,
                theme::kParamColumnW,
                theme::kParamColumnH,
                kScratchW,
                kScratchH,
                static_cast<unsigned>(kScreenW * kHeaderH * 2 + theme::kParamColumnW * theme::kParamColumnH * 2 +
                                      kScratchW * kScratchH * 2));

    if (lcd_.width() != kScreenW || lcd_.height() != kScreenH)
    {
        slog_printf("display: WARNING geometry is %ldx%ld, layout assumes %dx%d\n",
                    static_cast<long>(lcd_.width()),
                    static_cast<long>(lcd_.height()),
                    kScreenW,
                    kScreenH);
    }

    return true;
}

void DisplayManager::showSplash(const char* line1, const char* line2)
{
    lcd_.fillScreen(theme::kBg);
    lcd_.setTextDatum(middle_center);

    lcd_.setFont(&fonts::Font4);
    lcd_.setTextColor(theme::kValueFg, theme::kBg);
    lcd_.drawString(line1, kScreenW / 2, kScreenH / 2 - 18);

    lcd_.setFont(&fonts::Font2);
    lcd_.setTextColor(theme::kLabelFg, theme::kBg);
    lcd_.drawString(line2, kScreenW / 2, kScreenH / 2 + 18);
}

// --- Screen dispatch ---------------------------------------------------------

void DisplayManager::setScreen(Screen* screen)
{
    if (screen == screen_) return;

    screen_ = screen;
    if (screen_ != nullptr) screen_->onEnter();

    // A screen switch always takes the full-repaint path. Nothing on the panel
    // belongs to the new screen yet, including the chrome.
    needs_full_ = true;
}

void DisplayManager::update()
{
    if (!sprites_ready_ || screen_ == nullptr) return;

    screen_->poll();

    char        note_buf[24] = {0};
    const char* note         = screen_->headerNote(note_buf, sizeof(note_buf));

    if (needs_full_ || screen_->needsFullRepaint())
    {
        const uint32_t t0 = micros();

        in_full_repaint_ = true;
        lcd_.fillRect(0, kHeaderH, kScreenW, kScreenH - kHeaderH - kFooterH, theme::kBg);
        drawHeader(screen_->title(), note);
        screen_->renderFull(*this);
        in_full_repaint_ = false;

        recordPageUs(micros() - t0);
        needs_full_ = false;
        return;
    }

    screen_->renderIncremental(*this);
}

// --- Chrome ------------------------------------------------------------------

void DisplayManager::drawHeader(const char* title, const char* note)
{
    header_spr_.fillSprite(theme::kHeaderBg);
    header_spr_.setFont(&fonts::Font4);

    header_spr_.setTextColor(theme::kHeaderFg, theme::kHeaderBg);
    header_spr_.setTextDatum(middle_left);
    header_spr_.drawString(title, 8, kHeaderH / 2);

    if (note != nullptr && note[0] != '\0')
    {
        header_spr_.setTextColor(theme::kLabelFg, theme::kHeaderBg);
        header_spr_.setTextDatum(middle_right);
        header_spr_.drawString(note, kScreenW - 8, kHeaderH / 2);
    }

    header_spr_.pushSprite(0, 0);
    lcd_.waitDMA();

    lcd_.drawFastHLine(0, kHeaderH, kScreenW, theme::kRule);
    lcd_.drawFastHLine(0, kScreenH - kFooterH - 1, kScreenW, theme::kRule);
}

void DisplayManager::drawStatus(const char* format, ...)
{
    char    buf[64];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    const int16_t y = kScreenH - kFooterH;

    lcd_.fillRect(0, y, kScreenW, kFooterH, theme::kBg);
    lcd_.setFont(&fonts::Font2);
    lcd_.setTextColor(theme::kStatusFg, theme::kBg);
    lcd_.setTextDatum(top_left);
    lcd_.drawString(buf, 6, y);
}

// --- Sprite pushes -----------------------------------------------------------

void DisplayManager::pushScratch(int16_t x, int16_t y, int16_t w, int16_t h)
{
    const uint32_t t0 = micros();

    // pushSprite pushes the whole sprite; clipping to the requested sub-region
    // is what keeps a small update small. The sprite is reused immediately by
    // the next region, so the transfer has to complete before we return.
    lcd_.setClipRect(x, y, w, h);
    scratch_spr_.pushSprite(x, y);
    lcd_.waitDMA();
    lcd_.clearClipRect();

    recordValueUs(micros() - t0);
}

void DisplayManager::pushColumn(int16_t x, int16_t y)
{
    const uint32_t t0 = micros();

    column_spr_.pushSprite(x, y);
    // The sprite buffer is reused by the next column, so the transfer has to be
    // complete before we touch it again.
    lcd_.waitDMA();

    recordValueUs(micros() - t0);
}

void DisplayManager::resetWorstCase()
{
    worst_value_us_ = 0;
    worst_page_us_  = 0;
}
