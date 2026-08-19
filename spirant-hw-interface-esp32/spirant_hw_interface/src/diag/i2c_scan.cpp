#include "i2c_scan.h"

#include <Arduino.h>
#include <Wire.h>

#include "../config.h"
#include "../log.h"

namespace
{

const char* describe(uint8_t addr)
{
    switch (addr)
    {
        case SEESAW_ADDR: return " <- Seesaw quad encoder (expected)";
        default: return "";
    }
}

}  // namespace

int i2c_scan(uint8_t sda, uint8_t scl, const char* label)
{
    Wire.end();

    if (!Wire.begin(sda, scl, I2C_FREQ_HZ))
    {
        slog_printf("i2c: %s (sda=%u scl=%u) FAILED to init\n", label, sda, scl);
        return -1;
    }
    Wire.setTimeOut(I2C_TIMEOUT_MS);

    slog_printf("i2c: scanning %s (sda=%u scl=%u @%dHz)\n", label, sda, scl, I2C_FREQ_HZ);

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            slog_printf("i2c:   0x%02X%s\n", addr, describe(addr));
            ++found;
        }
    }

    if (found == 0) slog_printf("i2c:   (nothing responded)\n");

    // Design doc 4/7.3 assume exactly one consumer on this bus, which is what
    // licenses the no-mutex conclusion. Say so out loud when it does not hold.
    if (found > 1)
    {
        slog_printf(
            "i2c:   %d devices -- more than the Seesaw alone. Revisit the "
            "no-mutex conclusion (design doc 4 / 7.3).\n",
            found);
    }

    return found;
}

int i2c_survey()
{
#if I2C_SCAN_ALT_PINS
    const bool alt_collides_with_log =
        LOG_SINK_UART && (PIN_I2C_SDA_ALT == PIN_LOG_UART_TX ||
                          PIN_I2C_SDA_ALT == PIN_LOG_UART_RX ||
                          PIN_I2C_SCL_ALT == PIN_LOG_UART_TX ||
                          PIN_I2C_SCL_ALT == PIN_LOG_UART_RX);

    if (alt_collides_with_log)
    {
        slog_printf(
            "i2c: skipping alt pins %u/%u -- they are the log UART. Set "
            "LOG_SINK_UART 0 to probe them.\n",
            PIN_I2C_SDA_ALT,
            PIN_I2C_SCL_ALT);
    }
    else
    {
        i2c_scan(PIN_I2C_SDA_ALT, PIN_I2C_SCL_ALT, "alternate pins");
    }
#endif

    // Primary last, so Wire is left where the rest of the firmware expects it.
    return i2c_scan(PIN_I2C_SDA, PIN_I2C_SCL, "configured pins");
}
