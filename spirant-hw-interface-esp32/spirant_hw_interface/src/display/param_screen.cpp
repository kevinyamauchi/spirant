#include "param_screen.h"

#include <Arduino.h>
#include <stdio.h>

#include "../log.h"
#include "display_manager.h"

namespace
{

// Column interior padding.
const int16_t kLabelY = 12;
const int16_t kValueY = 52;
const int16_t kUnitY  = 96;

int16_t columnX(uint8_t encoder)
{
    return static_cast<int16_t>(encoder * theme::kParamColumnW);
}

}  // namespace

const char* ParamScreen::title() const
{
    return kPageNames[params_.currentPage()];
}

const char* ParamScreen::headerNote(char* buf, unsigned buf_len) const
{
    snprintf(buf, buf_len, "%u/%u", params_.currentPage() + 1, kNumPages);
    return buf;
}

// --- Regions -----------------------------------------------------------------

void ParamScreen::drawChrome(DisplayManager& dm)
{
    // Static furniture: the rules between columns. Drawn once per full repaint;
    // the sprites never overwrite them.
    for (uint8_t e = 1; e < kParamsPerPage; ++e)
    {
        dm.lcd().drawFastVLine(columnX(e) - 1, theme::kBodyY, theme::kParamColumnH, theme::kRule);
    }
}

void ParamScreen::renderColumn(DisplayManager& dm, uint8_t encoder)
{
    const Parameter* param = params_.paramAt(params_.currentPage(), encoder);
    if (param == nullptr) return;

    lgfx::LGFX_Sprite& spr = dm.column();
    const int16_t      cx  = theme::kParamColumnW / 2;

    spr.fillSprite(theme::kBg);
    spr.setTextDatum(middle_center);

    if (!param->active())
    {
        spr.setFont(&fonts::Font4);
        spr.setTextColor(theme::kNullFg, theme::kBg);
        spr.drawString("--", cx, kValueY);
    }
    else
    {
        spr.setFont(&fonts::Font2);
        spr.setTextColor(theme::kLabelFg, theme::kBg);
        spr.drawString(param->spec->label, cx, kLabelY);

        char value[12];
        snprintf(value, sizeof(value), "%ld", static_cast<long>(param->value));
        spr.setFont(&fonts::Font4);
        spr.setTextColor(theme::kValueFg, theme::kBg);
        spr.drawString(value, cx, kValueY);

        if (param->spec->unit[0] != '\0')
        {
            spr.setFont(&fonts::Font2);
            spr.setTextColor(theme::kUnitFg, theme::kBg);
            spr.drawString(param->spec->unit, cx, kUnitY);
        }
    }

    dm.pushColumn(columnX(encoder), theme::kBodyY);
}

void ParamScreen::renderBody(DisplayManager& dm)
{
    drawChrome(dm);

    for (uint8_t e = 0; e < kParamsPerPage; ++e)
    {
        renderColumn(dm, e);
    }
}

// --- Screen interface --------------------------------------------------------

void ParamScreen::renderFull(DisplayManager& dm)
{
    // Consume the flags this repaint satisfies, so the incremental pass that
    // follows does not redraw every column a second time.
    ParameterChange changes[kTotalSlots];
    params_.takeDisplayChanges(changes, kTotalSlots);

    renderBody(dm);
    last_page_ = params_.currentPage();
}

void ParamScreen::renderIncremental(DisplayManager& dm)
{
    const uint8_t page = params_.currentPage();

    // Consume flags first, unconditionally. Changes on pages that are not on
    // screen still have to be cleared -- their values are already correct in
    // the model, and leaving the flags set would make every later update
    // rescan them.
    ParameterChange changes[kTotalSlots];
    const size_t    n = params_.takeDisplayChanges(changes, kTotalSlots);

    for (size_t i = 0; i < n; ++i)
    {
        if (changes[i].page != page) continue;
        renderColumn(dm, changes[i].encoder);
    }
}

// --- Profiling (plan section 1.3) --------------------------------------------

void ParamScreen::profile(DisplayManager& dm)
{
    if (!dm.spritesReady()) return;

    struct Stat
    {
        uint32_t min = 0xFFFFFFFF;
        uint32_t max = 0;
        uint64_t sum = 0;
        uint32_t n   = 0;

        void add(uint32_t us)
        {
            if (us < min) min = us;
            if (us > max) max = us;
            sum += us;
            ++n;
        }
        uint32_t avg() const { return n ? static_cast<uint32_t>(sum / n) : 0; }
    };

    // Profile against the widest page -- Filter has four active slots and the
    // 3-digit Hz value, so it is the honest worst case for both measurements.
    const uint8_t saved_page = params_.currentPage();
    params_.setPage(1);
    renderBody(dm);

    Stat value;
    for (uint32_t i = 0; i < 200; ++i)
    {
        const uint32_t t0 = micros();
        renderColumn(dm, static_cast<uint8_t>(i % kParamsPerPage));
        value.add(micros() - t0);
    }

    Stat page;
    for (uint32_t i = 0; i < 20; ++i)
    {
        const uint32_t t0 = micros();
        renderBody(dm);
        page.add(micros() - t0);
    }

    // Is the push actually asynchronous, or is LovyanGFX busy-waiting inside
    // pushSprite? If it is async, the push returns well before waitDMA() does.
    dm.column().fillSprite(theme::kBg);
    const uint32_t t_push_0 = micros();
    dm.column().pushSprite(0, theme::kBodyY);
    const uint32_t push_us = micros() - t_push_0;
    const uint32_t t_wait_0 = micros();
    dm.lcd().waitDMA();
    const uint32_t wait_us = micros() - t_wait_0;

    slog_printf("profile: value region  min=%luus avg=%luus max=%luus (target <2000)\n",
                static_cast<unsigned long>(value.min),
                static_cast<unsigned long>(value.avg()),
                static_cast<unsigned long>(value.max));
    slog_printf("profile: full page     min=%luus avg=%luus max=%luus (target <20000)\n",
                static_cast<unsigned long>(page.min),
                static_cast<unsigned long>(page.avg()),
                static_cast<unsigned long>(page.max));
    slog_printf("profile: push=%luus then waitDMA=%luus -> %s\n",
                static_cast<unsigned long>(push_us),
                static_cast<unsigned long>(wait_us),
                wait_us > push_us ? "async (DMA overlaps)" : "push appears to block");

    const bool value_ok = value.max < 2000;
    const bool page_ok  = page.max < 20000;
    slog_printf("profile: value %s, page %s\n",
                value_ok ? "WITHIN TARGET" : "OVER TARGET",
                page_ok ? "WITHIN TARGET" : "OVER TARGET");

    params_.setPage(saved_page);
    last_page_ = kNoPage;
    dm.invalidate();
    dm.resetWorstCase();
}
