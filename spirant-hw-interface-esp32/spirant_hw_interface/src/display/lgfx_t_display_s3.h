#pragma once

// LovyanGFX device definition for the LilyGO T-Display-S3 (plan section 1.2).
//
// ST7789 on an 8-bit I8080 parallel bus. Every number here comes from LilyGO's
// own board definition rather than being derived -- the plan's risk register
// calls out the panel offset and inversion as a day-eating trap, and the
// 170px panel on a 240px-wide controller is exactly that trap.
//
// The pins live in config.h so there is one place to look.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "../config.h"

class LGFX_TDisplayS3 : public lgfx::LGFX_Device
{
  public:
    LGFX_TDisplayS3()
    {
        {
            auto cfg = bus_.config();

            cfg.freq_write = LCD_BUS_FREQ_HZ;
            cfg.pin_wr     = PIN_LCD_WR;
            cfg.pin_rd     = PIN_LCD_RD;
            cfg.pin_rs     = PIN_LCD_DC;

            cfg.pin_d0 = PIN_LCD_D0;
            cfg.pin_d1 = PIN_LCD_D1;
            cfg.pin_d2 = PIN_LCD_D2;
            cfg.pin_d3 = PIN_LCD_D3;
            cfg.pin_d4 = PIN_LCD_D4;
            cfg.pin_d5 = PIN_LCD_D5;
            cfg.pin_d6 = PIN_LCD_D6;
            cfg.pin_d7 = PIN_LCD_D7;

            bus_.config(cfg);
            panel_.setBus(&bus_);
        }

        {
            auto cfg = panel_.config();

            cfg.pin_cs   = PIN_LCD_CS;
            cfg.pin_rst  = PIN_LCD_RST;
            cfg.pin_busy = -1;

            cfg.memory_width  = LCD_MEMORY_WIDTH;
            cfg.memory_height = LCD_MEMORY_HEIGHT;
            cfg.panel_width   = LCD_PANEL_WIDTH;
            cfg.panel_height  = LCD_PANEL_HEIGHT;
            cfg.offset_x      = LCD_OFFSET_X;
            cfg.offset_y      = LCD_OFFSET_Y;

            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;

            // The parallel bus is write-only on this board.
            cfg.readable = false;
            // ST7789 on this panel boots inverted.
            cfg.invert     = true;
            cfg.rgb_order  = false;
            cfg.dlen_16bit = false;
            // Nothing else shares the bus, so LovyanGFX can skip re-asserting
            // bus settings around every transaction.
            cfg.bus_shared = false;

            panel_.config(cfg);
        }

        {
            auto cfg = light_.config();

            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;

            light_.config(cfg);
            panel_.setLight(&light_);
        }

        setPanel(&panel_);
    }

  private:
    lgfx::Panel_ST7789   panel_;
    lgfx::Bus_Parallel8  bus_;
    lgfx::Light_PWM      light_;
};
