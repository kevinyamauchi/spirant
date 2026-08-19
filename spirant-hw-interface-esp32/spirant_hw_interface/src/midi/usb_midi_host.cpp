#include "usb_midi_host.h"

#include <Arduino.h>

#include "../config.h"
#include "../log.h"
#include "monitor_access.h"
#include "usb_midi_parser.h"

#if USB_MIDI_ENABLED

// The board-setting guards are in config.h, included above, so they have
// already fired if any of them apply -- deliberately before the library headers
// below, which cannot then mask a settings mistake with an include failure.

#include <USBConnection.h>

// This one has to stay here: it depends on a macro the library header defines.
#if !ESP32_HOST_MIDI_USB_AVAILABLE
#error "USB_MIDI_ENABLED requires a target with USB-OTG (S2/S3/P4). Wrong board selected?"
#endif

#include <usb/usb_host.h>

namespace usb_midi
{
namespace
{

// --- Staging -----------------------------------------------------------------
//
// USBConnection::task() dispatches every queued packet through our callback
// before it returns, so the callback cannot take the monitor lock itself
// without doing so once per message. It parks messages here instead, and
// drain() folds the whole batch in under one lock (monitor_access.h).
//
// Only ever touched by the drain task, so no locking of its own. Sized above
// the transport ring's 64 entries: one drain can never legitimately produce
// more than the ring holds.

const uint16_t kStagingCapacity = 80;

MidiMessage g_staging[kStagingCapacity];
uint16_t    g_staged             = 0;
uint16_t    g_staged_unparsed    = 0;
uint32_t    g_staging_overflows  = 0;

/// Set from the core-0 USB task, read from the drain on core 1. Latest value
/// wins and it is a single aligned word, so a plain volatile is honest here --
/// unlike the monitor snapshot, there is no multi-field consistency to protect.
volatile bool     g_device_present     = false;
volatile uint16_t g_pending_vid        = 0;
volatile uint16_t g_pending_pid        = 0;
volatile uint8_t  g_pending_ep_addr    = 0;
volatile uint16_t g_pending_max_packet = 0;
volatile uint8_t  g_pending_interval   = 0;
volatile bool     g_connect_pending    = false;
volatile bool     g_disconnect_pending = false;

// --- Transport ---------------------------------------------------------------

/// USBConnection with the enumeration detail plan section 2.2 asks for.
///
/// The library logs nothing about what it found, and "it enumerated" is not the
/// same claim as "it enumerated the way we expect". _processConfig is virtual
/// and the members it needs are protected, so a subclass is the whole cost.
class SpirantUsbHost : public USBConnection
{
  public:
    uint16_t vid() const { return vid_; }
    uint16_t pid() const { return pid_; }

  protected:
    void _processConfig(const usb_config_desc_t* config_desc) override;
    void _onDeviceGone() override;

  private:
    void logDeviceDescriptor();
    void logConfigDescriptor(const usb_config_desc_t* config_desc);

    uint16_t vid_ = 0;
    uint16_t pid_ = 0;
};

SpirantUsbHost g_host;

void SpirantUsbHost::logDeviceDescriptor()
{
    const usb_device_desc_t* dev = nullptr;
    if (usb_host_get_device_descriptor(deviceHandle, &dev) != ESP_OK || dev == nullptr)
    {
        slog_printf("usb: device descriptor unavailable\n");
        return;
    }

    vid_ = dev->idVendor;
    pid_ = dev->idProduct;

    slog_printf("usb: VID=%04X PID=%04X bcdUSB=%04X class=%02X/%02X/%02X ep0=%u cfgs=%u\n",
                dev->idVendor,
                dev->idProduct,
                dev->bcdUSB,
                dev->bDeviceClass,
                dev->bDeviceSubClass,
                dev->bDeviceProtocol,
                dev->bMaxPacketSize0,
                dev->bNumConfigurations);
}

void SpirantUsbHost::logConfigDescriptor(const usb_config_desc_t* config_desc)
{
    const uint8_t* p     = config_desc->val;
    const uint16_t total = config_desc->wTotalLength;

    slog_printf("usb: config wTotalLength=%u interfaces=%u\n",
                total,
                config_desc->bNumInterfaces);

    uint16_t i = 0;
    while (i + 1 < total)
    {
        const uint8_t len  = p[i];
        const uint8_t type = p[i + 1];

        if (len < 2 || (i + len) > total) break;

        switch (type)
        {
            case 0x04:  // Interface
                if (len >= 9)
                {
                    slog_printf("usb:   if %u alt %u eps %u class=%02X sub=%02X proto=%02X%s\n",
                                p[i + 2],
                                p[i + 3],
                                p[i + 4],
                                p[i + 5],
                                p[i + 6],
                                p[i + 7],
                                (p[i + 5] == 0x01 && p[i + 6] == 0x03) ? "  <- MIDIStreaming" : "");
                }
                break;

            case 0x05:  // Endpoint
                if (len >= 7)
                {
                    const uint8_t  addr  = p[i + 2];
                    const uint8_t  attrs = p[i + 3];
                    const uint16_t mps   = static_cast<uint16_t>(p[i + 4] | (p[i + 5] << 8));
                    static const char* const kXferNames[4] = {"ctrl", "iso", "bulk", "intr"};

                    slog_printf("usb:     ep %02X %-3s %s mps=%u bInterval=%u\n",
                                addr,
                                (addr & 0x80) ? "IN" : "OUT",
                                kXferNames[attrs & 0x03],
                                mps,
                                p[i + 6]);
                }
                break;

            case 0x24:  // CS_INTERFACE
                // The MS Interface Header (subtype 0x01) carries bcdMSC, which
                // is the authoritative answer to plan section 2.2's "MIDI 2.0
                // UMP or MIDI 1.0 fallback" question -- 0x0100 is MIDI 1.0,
                // 0x0200 is MIDI 2.0. Note that USBConnection claims the first
                // MIDIStreaming interface it meets regardless of alt setting,
                // so on a MIDI 2.0 device it takes the MIDI 1.0 alt-0 fallback.
                if (len >= 5 && p[i + 2] == 0x01)
                {
                    const uint16_t bcd = static_cast<uint16_t>(p[i + 3] | (p[i + 4] << 8));
                    slog_printf("usb:     MS header bcdMSC=%04X (%s)\n",
                                bcd,
                                bcd >= 0x0200 ? "MIDI 2.0 capable" : "MIDI 1.0");
                }
                break;

            default:
                break;
        }

        i = static_cast<uint16_t>(i + len);
    }
}

void SpirantUsbHost::_processConfig(const usb_config_desc_t* config_desc)
{
    // Runs on the library's core-0 USB task, once per enumeration. The log is
    // blocking at 115200 -- a couple of hundred milliseconds for a verbose
    // device -- but nothing is streaming yet and no transfer is in flight, so
    // the only thing it delays is the first submit.
#if USB_MIDI_LOG_ENUMERATION
    logDeviceDescriptor();
    logConfigDescriptor(config_desc);
#else
    logDeviceDescriptor();
#endif

    g_pending_vid = vid_;
    g_pending_pid = pid_;

    USBConnection::_processConfig(config_desc);

    // isReady and midiTransfer are set by the base call above, so this is the
    // first moment either is meaningful.
    if (isReady && midiTransfer != nullptr)
    {
        g_pending_ep_addr    = midiTransfer->bEndpointAddress;
        g_pending_max_packet = static_cast<uint16_t>(midiTransfer->num_bytes);
        g_pending_interval   = interval;

        slog_printf("usb: claimed IN ep %02X mps=%u interval=%ums\n",
                    midiTransfer->bEndpointAddress,
                    static_cast<unsigned>(midiTransfer->num_bytes),
                    interval);
    }
    else
    {
        g_pending_ep_addr    = 0;
        g_pending_max_packet = 0;
        g_pending_interval   = 0;

        slog_printf("usb: NO usable MIDI IN endpoint -- device is enumerated but silent\n");
    }

    g_device_present  = true;
    g_connect_pending = true;
}

void SpirantUsbHost::_onDeviceGone()
{
    slog_printf("usb: device gone (VID=%04X PID=%04X)\n", vid_, pid_);
    vid_                 = 0;
    pid_                 = 0;
    g_device_present     = false;
    g_disconnect_pending = true;
}

// --- Receive callback --------------------------------------------------------

void onMidiData(void* /*ctx*/, const uint8_t* data, size_t length)
{
    if (length < 4) return;

    // Called from inside USBConnection::task(), i.e. on the drain task. Parks
    // the message; drain() applies the batch.
    if (g_staged >= kStagingCapacity)
    {
        ++g_staging_overflows;
        return;
    }

    MidiMessage m;
    if (parseUsbMidiPacket(data, m))
    {
        g_staging[g_staged++] = m;
    }
    else
    {
        ++g_staged_unparsed;
    }
}

}  // namespace

bool begin()
{
    if (!monitor::init())
    {
        slog_printf("usb: monitor mutex alloc FAILED\n");
        return false;
    }

    g_host.setMidiCallback(onMidiData, nullptr);

    if (!g_host.begin())
    {
        slog_printf("usb: host start FAILED -- %s\n", g_host.getLastError().c_str());
        slog_printf("usb: check Tools > USB Mode = \"USB-OTG (TinyUSB)\"\n");
        return false;
    }

    slog_printf("usb: host started, ring capacity %u packets\n", queueCapacity());
    return true;
}

uint16_t drain(uint32_t now_ms, uint32_t now_us)
{
    // Depth before the drain is the interesting one -- afterwards it is zero by
    // construction and tells us nothing about how close we came to the rail.
    const uint16_t depth = queueDepth();

    g_staged          = 0;
    g_staged_unparsed = 0;

    // Dispatches every queued packet through onMidiData() and returns.
    g_host.task();

    const bool connect    = g_connect_pending;
    const bool disconnect = g_disconnect_pending;
    if (connect) g_connect_pending = false;
    if (disconnect) g_disconnect_pending = false;

    const bool ready = g_host.isConnected();

    if (g_staged == 0 && g_staged_unparsed == 0 && !connect && !disconnect && depth == 0)
    {
        // Nothing happened. Still roll the rate window, but do not contend for
        // the lock on every idle pass.
        static uint32_t last_tick_ms = 0;
        if (now_ms - last_tick_ms >= 100)
        {
            last_tick_ms = now_ms;
            monitor::WriteLock w(MONITOR_LOCK_TIMEOUT_MS);
            if (w) w->tick(now_ms);
        }
        return 0;
    }

    monitor::WriteLock w(MONITOR_LOCK_TIMEOUT_MS);
    if (!w)
    {
        // Dropping a monitor update beats stalling the drain. The packets are
        // already out of the ring, so nothing backs up.
        return g_staged;
    }

    if (disconnect) w->onUsbDisconnected();

    if (connect)
    {
        w->onUsbDevicePresent(g_pending_vid, g_pending_pid);

        // Published only when the transport really has a usable IN endpoint --
        // the distinction this file's header opens with.
        if (ready)
        {
            w->onUsbEndpointReady(g_pending_ep_addr, g_pending_max_packet, g_pending_interval);
        }
    }

    w->onDrain(depth, queueCapacity(), g_staged);

    for (uint16_t i = 0; i < g_staged; ++i)
    {
        w->onMessage(g_staging[i], now_ms, now_us);

#if USB_MIDI_LOG_MESSAGES
        const MidiMessage& m = g_staging[i];
        slog_printf("midi: %-7s ch%-2u %3u %3u  (st %02X cable %u)\n",
                    midiTypeName(m.type),
                    m.isChannelMessage() ? (m.channel + 1u) : 0u,
                    m.data1,
                    m.data2,
                    m.status,
                    m.cable);
#endif
    }

    for (uint16_t i = 0; i < g_staged_unparsed; ++i) w->onUnparsed();

    w->tick(now_ms);

    return g_staged;
}

bool endpointReady()
{
    return g_host.isConnected();
}

uint16_t queueDepth()
{
    // getQueueSize() reads head and tail without the spinlock, and the UI task
    // calls this from a different task than the drain, so the result can be one
    // step stale or momentarily read as a full QUEUE_SIZE -- a state the ring
    // cannot actually hold, since it counts full at capacity-1. Clamped so a
    // diagnostic artifact never displays as "64/64".
    const int d = g_host.getQueueSize();
    if (d <= 0) return 0;
    if (d >= USB_RING_CAPACITY) return USB_RING_CAPACITY - 1;
    return static_cast<uint16_t>(d);
}

uint16_t queueCapacity()
{
    // USBConnection::QUEUE_SIZE is a protected compile-time constant; it is not
    // reachable and not configurable. Kept in config.h so the section 2.3
    // margin arithmetic has a single named source, with a static check there
    // against the version we validated against.
    return USB_RING_CAPACITY;
}

uint32_t stagingOverflows()
{
    return g_staging_overflows;
}

}  // namespace usb_midi

#else  // !USB_MIDI_ENABLED

namespace usb_midi
{
bool     begin() { return false; }
uint16_t drain(uint32_t, uint32_t) { return 0; }
bool     endpointReady() { return false; }
uint16_t queueDepth() { return 0; }
uint16_t queueCapacity() { return 0; }
uint32_t stagingOverflows() { return 0; }
}  // namespace usb_midi

#endif  // USB_MIDI_ENABLED
