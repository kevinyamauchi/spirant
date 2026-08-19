#include "encoder_input.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include "../config.h"
#include "../log.h"

namespace
{

// Seesaw GPIO pin for each encoder's push-button, indexed by encoder. Same
// numbers as spirant-encoder-board-rs/src/registers.rs, which took them from
// Adafruit's SS_ENC{0..3}_SWITCH. All on port A, so one bulk read covers them.
const uint8_t kSwitchPins[kParamsPerPage] = {12, 14, 17, 9};

const uint32_t kSwitchMask = (1UL << 12) | (1UL << 14) | (1UL << 17) | (1UL << 9);

// A single 20 ms service window cannot legitimately contain this many
// detents, even spinning hard. A jump larger than this is a corrupted I2C
// read, and applying it would slam a parameter to its rail -- much more
// noticeable than dropping it. Resync the cache and count it instead.
const int32_t kMaxPlausibleDelta = 64;

// If the INT line will not deassert, servicing it forever at UI-task priority
// starves everything else. After this many consecutive immediate re-fires,
// give up on the interrupt and let the timer tick drive polling instead.
const uint32_t kMaxConsecutiveRefires = 64;

// Seesaw wants a gap between the register-address write and the read back.
// Adafruit_seesaw uses 250 us by default; the Pico driver uses 125.
const uint32_t kSeesawReadDelayUs = 250;

// Read-to-clear the Seesaw's GPIO interrupt flags, which is what deasserts the
// INT line. Adafruit_seesaw keeps its register accessors protected and exposes
// no flag read at all, so this is a direct transaction against the same
// two-byte register address the Pico driver uses
// (spirant-encoder-board-rs/src/registers.rs: [GPIO_BASE, INTFLAG]).
bool clear_seesaw_int_flags()
{
    Wire.beginTransmission(SEESAW_ADDR);
    Wire.write(static_cast<uint8_t>(SEESAW_GPIO_BASE));
    Wire.write(static_cast<uint8_t>(SEESAW_GPIO_INTFLAG));
    if (Wire.endTransmission() != 0) return false;

    delayMicroseconds(kSeesawReadDelayUs);

    if (Wire.requestFrom(static_cast<uint8_t>(SEESAW_ADDR), static_cast<uint8_t>(4)) != 4)
    {
        return false;
    }
    while (Wire.available())
    {
        (void)Wire.read();
    }
    return true;
}

TaskHandle_t      s_notify_task = nullptr;
volatile uint32_t s_isr_count   = 0;
volatile uint32_t s_isr_us      = 0;
volatile bool     s_int_armed   = false;

void IRAM_ATTR seesaw_isr(void* arg)
{
    (void)arg;

    // MUST be first. The source is level-triggered and stays low until the
    // Seesaw's INTFLAG is read from the task, so leaving it enabled here
    // re-enters this ISR immediately and forever.
    gpio_intr_disable(static_cast<gpio_num_t>(PIN_SEESAW_INT));
    s_int_armed = false;

    // Not ++: a read-modify-write on a volatile is deprecated in C++20 and
    // the ISR is the only writer either way.
    s_isr_count = s_isr_count + 1;
    s_isr_us    = static_cast<uint32_t>(esp_timer_get_time());

    BaseType_t higher_priority_woken = pdFALSE;
    if (s_notify_task != nullptr)
    {
        vTaskNotifyGiveFromISR(s_notify_task, &higher_priority_woken);
    }
    portYIELD_FROM_ISR(higher_priority_woken);
}

}  // namespace

uint32_t EncoderInput::isrCount() const { return s_isr_count; }

uint32_t EncoderInput::lastIsrUs() const { return s_isr_us; }

bool EncoderInput::begin(TaskHandle_t notify_task)
{
    if (!ss_.begin(SEESAW_ADDR))
    {
        slog_printf("encoders: Seesaw NOT FOUND at 0x%02X\n", SEESAW_ADDR);
        present_ = false;
        return false;
    }

    const uint32_t version = ss_.getVersion();
    slog_printf("encoders: Seesaw at 0x%02X, fw product %u\n",
               SEESAW_ADDR,
               static_cast<unsigned>(version >> 16));

    // Push-buttons: inputs with pull-ups, so they read low when pressed.
    ss_.pinModeBulk(kSwitchMask, INPUT_PULLUP);

    // Seed the cache before enabling anything, so the first service pass
    // reports deltas relative to where the knobs actually are rather than
    // relative to zero.
    for (uint8_t i = 0; i < kParamsPerPage; ++i)
    {
        last_pos_[i] = ss_.getEncoderPosition(i);
        last_btn_[i] = (ss_.digitalReadBulk(kSwitchMask) & (1UL << kSwitchPins[i])) == 0;
    }
    slog_printf("encoders: initial positions [%ld, %ld, %ld, %ld]\n",
               static_cast<long>(last_pos_[0]),
               static_cast<long>(last_pos_[1]),
               static_cast<long>(last_pos_[2]),
               static_cast<long>(last_pos_[3]));

#if ENCODER_USE_INTERRUPT
    for (uint8_t i = 0; i < kParamsPerPage; ++i)
    {
        ss_.enableEncoderInterrupt(i);
    }

#if SEESAW_GPIO_INTERRUPTS
    // Off by default: the shipped Pico firmware keeps the buttons off the
    // shared INT line so a press cannot perturb the rotation path. The
    // buttons are polled on the UI tick either way. See config.h.
    ss_.setGPIOInterrupts(kSwitchMask, true);
#endif

    s_notify_task = notify_task;

    gpio_config_t io = {};
    io.pin_bit_mask  = 1ULL << PIN_SEESAW_INT;
    io.mode          = GPIO_MODE_INPUT;
    // The breakout does not pull INT up; it is open drain.
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_LOW_LEVEL;
    gpio_config(&io);

    const esp_err_t svc = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (svc != ESP_OK && svc != ESP_ERR_INVALID_STATE)
    {
        slog_printf("encoders: gpio_install_isr_service failed (%d)\n", svc);
    }

    gpio_isr_handler_add(static_cast<gpio_num_t>(PIN_SEESAW_INT), seesaw_isr, nullptr);

    // Read the flags once so INT starts deasserted, then arm.
    clear_seesaw_int_flags();
    gpio_intr_enable(static_cast<gpio_num_t>(PIN_SEESAW_INT));
    s_int_armed = true;

    slog_printf("encoders: level-triggered INT on GPIO%d, armed\n", PIN_SEESAW_INT);
#else
    (void)notify_task;
    slog_printf("encoders: polled mode (ENCODER_USE_INTERRUPT 0), %d ms tick\n", UI_TICK_MS);
#endif

    // Last, deliberately. begin() runs on the loopTask while the UI task is
    // already ticking; service() and rearm() both no-op until this is set, so
    // they cannot interleave their own I2C transactions with the ones above.
    // An interrupt arriving in the gap costs one tick of latency and is then
    // serviced normally.
    present_ = true;

    return true;
}

void EncoderInput::service(Events& out)
{
    for (uint8_t i = 0; i < kParamsPerPage; ++i)
    {
        out.deltas[i]  = 0;
        out.pressed[i] = false;
    }
    out.any = false;

    if (!present_) return;

    ++service_count_;

    // Every wake reads all four. The Seesaw exposes no "which source fired",
    // and reading all four is also what makes simultaneous movement on two
    // encoders impossible to drop.
    for (uint8_t i = 0; i < kParamsPerPage; ++i)
    {
        const int32_t pos   = ss_.getEncoderPosition(i);
        int32_t       delta = pos - last_pos_[i];

        if (delta > kMaxPlausibleDelta || delta < -kMaxPlausibleDelta)
        {
            ++glitch_count_;
            delta = 0;  // Resync to the new position, drop the bogus jump.
        }

        last_pos_[i] = pos;

        if (delta != 0)
        {
            out.deltas[i] = delta;
            out.any       = true;
            detent_count_ += static_cast<uint32_t>(delta < 0 ? -delta : delta);
        }
    }

    const uint32_t bulk = ss_.digitalReadBulk(kSwitchMask);
    const uint32_t now  = millis();

    for (uint8_t i = 0; i < kParamsPerPage; ++i)
    {
        const bool down = (bulk & (1UL << kSwitchPins[i])) == 0;

        if (down && !last_btn_[i] && (now - last_press_ms_[i]) >= BUTTON_DEBOUNCE_MS)
        {
            out.pressed[i]    = true;
            out.any           = true;
            last_press_ms_[i] = now;
        }
        last_btn_[i] = down;
    }

#if ENCODER_USE_INTERRUPT
    // Read-to-clear. Until this happens the INT line stays low and rearm()
    // would put us straight back in the ISR.
    clear_seesaw_int_flags();
#endif
}

void EncoderInput::rearm()
{
#if ENCODER_USE_INTERRUPT
    if (!present_ || s_int_armed) return;

    static uint32_t consecutive_refires = 0;

    if (digitalRead(PIN_SEESAW_INT) == LOW)
    {
        // Still asserted after a full service pass. Normal once in a while --
        // a detent arriving during servicing -- but a stuck line here is the
        // lockup the plan warns about, so bound it.
        if (++consecutive_refires >= kMaxConsecutiveRefires)
        {
            slog_printf(
                "encoders: INT stuck low after %u services -- falling back to "
                "polling on the %d ms tick\n",
                static_cast<unsigned>(consecutive_refires),
                UI_TICK_MS);
            consecutive_refires = 0;
            return;  // Left disarmed on purpose; the tick keeps servicing.
        }
    }
    else
    {
        consecutive_refires = 0;
    }

    s_int_armed = true;
    gpio_intr_enable(static_cast<gpio_num_t>(PIN_SEESAW_INT));
#endif
}

bool EncoderInput::probe()
{
    Wire.beginTransmission(SEESAW_ADDR);
    if (Wire.endTransmission() == 0) return true;

    ++i2c_error_count_;
    return false;
}
