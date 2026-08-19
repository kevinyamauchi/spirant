#pragma once

#include <stdint.h>

// I2C bus survey (plan section 1.4).
//
// Resolves the section 0.2 pin question against the actual board, and settles
// whether the encoder bus really has exactly one consumer -- design doc
// sections 4 and 7.3 hang their "no mutex needed" conclusion on that.

/// Scan `sda`/`scl` and log every responding address. Leaves Wire configured
/// for the pins it was given. Returns the number of devices found, or -1 if
/// the bus could not be brought up on those pins.
int i2c_scan(uint8_t sda, uint8_t scl, const char* label);

/// Scan the configured pins, plus the alternate candidate pair if
/// I2C_SCAN_ALT_PINS is set and it does not collide with the log UART. Leaves
/// Wire on the configured (primary) pins. Returns the device count on the
/// primary bus.
int i2c_survey();
