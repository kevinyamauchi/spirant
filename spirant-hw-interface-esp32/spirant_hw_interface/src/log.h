#pragma once

// Log sink abstraction (plan section 1.1).
//
// Built now, while USB CDC still works, because phase 2 puts the USB port into
// host mode and takes the CDC console away entirely (plan section 0.1). At
// that point LOG_SINK_USB goes to 0 in config.h and nothing else changes.
//
// Verify the UART sink physically with a USB-UART adapter NOW, not in phase 2.
//
// The plan calls this log_printf(); that name is already taken by the ESP32
// core (esp32-hal-log.h), hence the prefix.

void log_init();

/// Writes to every enabled sink. Newline is NOT appended -- include it.
void slog_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// True if at least one sink is enabled and initialised.
bool log_available();
