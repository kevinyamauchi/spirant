#pragma once

#include "daisy_seed.h"

#include "frame.h"

// --- System Bus: I2C1 controller + MSG line ---------------------------------
// The Daisy is the controller; the Pico (address 0x20) is the target and owns
// the parameter state. See plans/inter_mcu_communication_protocol.md.
//
// TWO NORMATIVE RULES (protocol section 7) -- both are what make audio
// disturbance structurally impossible rather than merely unlikely:
//
//   1. POLLED I2C ONLY. Every transaction uses TransmitBlocking /
//      ReceiveBlocking in polling mode. Interrupt- and DMA-mode I2C are
//      prohibited: an I2C ISR is the only mechanism by which this protocol
//      could contend with the audio interrupt for CPU time.
//
//   2. MANDATORY TIMEOUTS. Every blocking call carries a ~10 ms timeout. A
//      Pico that dies mid-transaction with SCL stretched then degrades to
//      "parameters frozen, audio and MIDI still running" instead of hanging
//      the main loop and permanently deafening the synth to MIDI.
//
// Everything here runs from the main loop. Nothing in this file may be called
// from the audio callback.

constexpr uint8_t  kSystemBusAddr    = 0x20;
constexpr uint8_t  kCmdGetAll        = 0x00;
constexpr uint8_t  kCmdGetChanged    = 0x01;
constexpr uint32_t kI2cTimeoutMs     = 10;
constexpr uint32_t kBootRetryMs      = 10;
constexpr uint32_t kSyncIntervalMs   = 33;  // ~30 Hz MSG poll

// Outcome of one query. The before/after distinction drives the section 4.4
// escalation decision: if the Pico may have finished transmitting, it may also
// have cleared its flags and raised MSG, so the changes exist nowhere but in
// its current values -- and only GetAll can recover them.
enum class SyncResult
{
    kApplied,           // CRC-verified frame decoded; records written to out[]
    kFailedBeforeRead,  // addressing/transmit/read failed -- Pico kept its
                        // flags and MSG stayed LOW, so the next poll retries
    kFailedAfterRead,   // the read completed but the frame was unusable
                        // (bad CRC or bad count) -- escalate to GetAll
};

// --- Internal state ---------------------------------------------------------

static daisy::I2CHandle system_bus_i2c;
static daisy::GPIO      msg_pin;

// Set when a GetChanged fails after its read completed. While set, every sync
// tick issues GetAll until one verifies. Deliberately a flag rather than an
// inline retry loop: an inline loop would block MIDI handling for as long as
// the fault lasted, and the whole point of the design is that the main loop
// stays responsive through bus faults.
static bool getall_pending = false;

// Counters for the bring-up log / LED2 diagnostics. Each fallback is a
// countable, explainable incident; a nonzero rate is worth investigating, not
// silently healing.
static uint32_t system_bus_failures  = 0;
static uint32_t system_bus_fallbacks = 0;

// --- Implementation ---------------------------------------------------------

// Bring up I2C1 on D11/D12 at 400 kHz and the MSG input on D16.
//
// MSG uses the internal pull-up so the line reads HIGH (idle) while the Pico
// is held in reset or still booting -- a missing Pico must never look like
// "changes pending".
inline bool system_bus_init()
{
    daisy::I2CHandle::Config config;
    config.periph          = daisy::I2CHandle::Config::Peripheral::I2C_1;
    config.speed           = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
    config.mode            = daisy::I2CHandle::Config::Mode::I2C_MASTER;
    config.pin_config.scl  = daisy::seed::D11;  // PB8
    config.pin_config.sda  = daisy::seed::D12;  // PB9

    const bool ok = system_bus_i2c.Init(config) == daisy::I2CHandle::Result::OK;

    msg_pin.Init(daisy::seed::D16,
                 daisy::GPIO::Mode::INPUT,
                 daisy::GPIO::Pull::PULLUP);

    return ok;
}

// True when the Pico has changes pending. MSG is active LOW.
inline bool system_bus_message_pending()
{
    return !msg_pin.Read();
}

// Issue one command and decode the response.
//
// The transaction is a 1-byte write followed by a fixed 97-byte read, sent as
// two separate blocking calls rather than a repeated start. The Pico handles
// both shapes, and separate calls are the simpler thing to reason about when
// each one carries its own timeout.
inline SyncResult
system_bus_query(uint8_t command, Record* out, uint8_t* count)
{
    *count = 0;

    uint8_t cmd = command;
    if(system_bus_i2c.TransmitBlocking(
           kSystemBusAddr, &cmd, 1, kI2cTimeoutMs)
       != daisy::I2CHandle::Result::OK)
    {
        system_bus_failures++;
        return SyncResult::kFailedBeforeRead;
    }

    uint8_t frame[kFrameLen];
    if(system_bus_i2c.ReceiveBlocking(
           kSystemBusAddr, frame, kFrameLen, kI2cTimeoutMs)
       != daisy::I2CHandle::Result::OK)
    {
        // The read did not complete, so the Pico saw a short read, kept its
        // flags and left MSG LOW. Nothing is lost; the next poll retries.
        system_bus_failures++;
        return SyncResult::kFailedBeforeRead;
    }

    const DecodeResult decoded = decode_frame(frame, out, kMaxRecords);
    if(decoded.status != DecodeStatus::kOk)
    {
        // The frame arrived but is unusable. The Pico may already have
        // cleared its flags and raised MSG -- only GetAll recovers from here.
        system_bus_failures++;
        return SyncResult::kFailedAfterRead;
    }

    *count = decoded.count;
    return SyncResult::kApplied;
}

// Block until the Pico answers a GetAll with a CRC-verified frame.
//
// No overall timeout, by design (protocol section 3.1): a missing Pico stalls
// visibly with the onboard LED lit rather than starting the synth with
// invented parameter values. Each individual attempt is still bounded by its
// own 10 ms timeout, so this never wedges on a stretched clock.
inline uint8_t system_bus_boot_sync(Record* out)
{
    uint8_t count = 0;
    while(system_bus_query(kCmdGetAll, out, &count) != SyncResult::kApplied)
    {
        daisy::System::Delay(kBootRetryMs);
    }
    return count;
}

// One sync tick, called from the main loop at ~30 Hz.
//
// Returns the number of records written to out[]; 0 means there was nothing
// to do (the common case -- one GPIO read per 33 ms). `failed` is set when a
// transaction failed, so the caller can flash the diagnostic LED.
//
// A pending GetAll escalation takes priority over the MSG line, because after
// a post-read failure MSG carries no useful information: the Pico may well
// have raised it while the changes never arrived.
inline uint8_t system_bus_sync(Record* out, bool* failed)
{
    *failed = false;

    if(getall_pending)
    {
        uint8_t          count  = 0;
        const SyncResult result = system_bus_query(kCmdGetAll, out, &count);
        if(result == SyncResult::kApplied)
        {
            getall_pending = false;
            return count;
        }
        *failed = true;
        return 0;
    }

    if(!system_bus_message_pending())
        return 0;

    uint8_t          count  = 0;
    const SyncResult result = system_bus_query(kCmdGetChanged, out, &count);
    switch(result)
    {
        case SyncResult::kApplied: return count;

        case SyncResult::kFailedBeforeRead:
            // MSG is still LOW and the flags are still set; the next tick
            // retries by the ordinary path.
            *failed = true;
            return 0;

        case SyncResult::kFailedAfterRead:
            getall_pending = true;
            system_bus_fallbacks++;
            *failed = true;
            return 0;
    }

    return 0;
}
