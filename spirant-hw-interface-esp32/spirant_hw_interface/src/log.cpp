#include "log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "config.h"

// --- The guard that makes plan section 2.0's failure mode un-compilable ------
//
// With "USB CDC On Boot = Disabled", the ESP32 core does this
// (HardwareSerial.h):
//
//     #else   // !ARDUINO_USB_CDC_ON_BOOT -- Serial is used from UART0
//     #define Serial Serial0
//
// silently redefining `Serial` as UART0 -- GPIO 43/44, which config.h assigns
// to I2C. The Serial.begin() below would then attach the UART peripheral to
// those pins and every log line afterwards would drive SDA, killing the
// encoders. The board setting and this flag have to move together, and
// remembering that is not a plan. This is.
//
// With CDC On Boot *Enabled* -- the recommended setting -- `Serial` is
// HWCDCSerial (USB Mode "Hardware CDC and JTAG") or USBSerial (OTG mode), both
// on the USB pins, and USB logging is safe. That is why this guard tests
// CDC-on-boot rather than USB mode.
//
// The core always defines ARDUINO_USB_CDC_ON_BOOT as 0 or 1, so this tests the
// value; if some future core stops defining it at all, the test still fails
// safe (an undefined macro is 0 here, which errors rather than compiling).
#if LOG_SINK_USB && !ARDUINO_USB_CDC_ON_BOOT
#error "LOG_SINK_USB=1 with CDC-on-boot disabled maps Serial to UART0 on GPIO 43/44 -- the I2C bus. Set LOG_SINK_USB 0 in config.h, or set USB CDC On Boot back to Enabled."
#endif

namespace
{

// One shared buffer, guarded by a mutex: slog_printf() is called from the UI
// task, from loop(), and from setup(). Never from an ISR -- printf from an
// ISR is not safe here and the ISR has nothing to say anyway.
constexpr size_t kLogBufBytes = 256;

char              g_buf[kLogBufBytes];
SemaphoreHandle_t g_mutex = nullptr;
bool              g_ready = false;

}  // namespace

void log_init()
{
    if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();

#if LOG_SINK_USB
    Serial.begin(115200);

    // Make USB writes non-blocking, permanently.
    //
    // usb_host_install() hands the OTG PHY to the host driver partway through
    // boot, and from that moment the CDC console is gone for the rest of the
    // session. HWCDC's disconnected path is already non-blocking (it drops
    // oldest-first into its ring), but the *transition* window -- host gone,
    // SOF watchdog not yet timed out -- takes the connected path, where the
    // core will wait up to ~20 x 100 ms for buffer space. A two-second stall
    // inside the core-0 USB task during enumeration would be far worse than a
    // dropped log line.
    //
    // Zero means "never wait, drop instead", which is the same posture as
    // Wire.setTimeOut() and the monitor snapshot's lock timeout: diagnostics
    // must never be able to stall the thing they are diagnosing.
#if ARDUINO_USB_MODE
    Serial.setTxTimeoutMs(0);
#endif

    // Wait briefly for the host to open the CDC port, but never block boot on
    // it -- the board must come up standalone. setup() latches board power
    // before calling us precisely so this wait cannot brown out a
    // battery-powered board.
    const uint32_t deadline = millis() + LOG_USB_WAIT_MS;
    while (!Serial && millis() < deadline)
    {
        delay(10);
    }
#endif

#if LOG_SINK_UART
    Serial1.begin(LOG_UART_BAUD, SERIAL_8N1, PIN_LOG_UART_RX, PIN_LOG_UART_TX);
#endif

    g_ready = true;

#if LOG_SINK_USB || LOG_SINK_UART
    slog_printf("\n--- spirant hw interface (esp32) ---\n");
    slog_printf("log sinks: usb=%d uart=%d (tx=%d rx=%d @%d)\n",
               LOG_SINK_USB,
               LOG_SINK_UART,
               PIN_LOG_UART_TX,
               PIN_LOG_UART_RX,
               LOG_UART_BAUD);
#endif
}

bool log_available()
{
#if LOG_SINK_USB || LOG_SINK_UART
    return g_ready;
#else
    return false;
#endif
}

void slog_printf(const char* format, ...)
{
#if LOG_SINK_USB || LOG_SINK_UART
    if (!g_ready) return;

    const bool locked =
        g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE;

    va_list args;
    va_start(args, format);
    vsnprintf(g_buf, sizeof(g_buf), format, args);
    va_end(args);

#if LOG_SINK_USB
    Serial.print(g_buf);
#endif
#if LOG_SINK_UART
    Serial1.print(g_buf);
#endif

    if (locked) xSemaphoreGive(g_mutex);
#else
    (void)format;
#endif
}
