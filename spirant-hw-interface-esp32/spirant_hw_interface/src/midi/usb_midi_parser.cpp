#include "usb_midi_parser.h"

namespace
{

// Code Index Numbers, USB-MIDI 1.0 Table 4-1.
enum : uint8_t
{
    kCinMisc            = 0x0,  ///< Reserved for future extension.
    kCinCableEvent      = 0x1,  ///< Reserved for future extension.
    kCinSysCommon2      = 0x2,  ///< 2-byte system common (MTC, song select).
    kCinSysCommon3      = 0x3,  ///< 3-byte system common (song position).
    kCinSysExStart      = 0x4,
    kCinSysExEnd1       = 0x5,  ///< Also single-byte system common.
    kCinSysExEnd2       = 0x6,
    kCinSysExEnd3       = 0x7,
    kCinNoteOff         = 0x8,
    kCinNoteOn          = 0x9,
    kCinPolyPressure    = 0xA,
    kCinControlChange   = 0xB,
    kCinProgramChange   = 0xC,
    kCinChannelPressure = 0xD,
    kCinPitchBend       = 0xE,
    kCinSingleByte      = 0xF,  ///< System realtime, or SysEx end with 1 byte.
};

}  // namespace

bool parseUsbMidiPacket(const uint8_t packet[4], MidiMessage& out)
{
    const uint8_t cin    = packet[0] & 0x0F;
    const uint8_t cable  = static_cast<uint8_t>((packet[0] >> 4) & 0x0F);
    const uint8_t status = packet[1];

    MidiMessage m;
    m.cable   = cable;
    m.status  = status;
    m.data1   = packet[2];
    m.data2   = packet[3];
    m.channel = static_cast<uint8_t>(status & 0x0F);

    switch (cin)
    {
        case kCinNoteOff:
            m.type = MidiType::NoteOff;
            break;

        case kCinNoteOn:
            // A note-on with velocity 0 is a note-off. Every MIDI source is
            // entitled to send it that way and many do, so normalise here
            // rather than making each consumer remember. MonitorState's active
            // note tracking would otherwise never see a key released.
            m.type = (m.data2 == 0) ? MidiType::NoteOff : MidiType::NoteOn;
            break;

        case kCinPolyPressure:
            m.type = MidiType::PolyPressure;
            break;

        case kCinControlChange:
            m.type = MidiType::ControlChange;
            break;

        case kCinProgramChange:
            m.type  = MidiType::ProgramChange;
            m.data2 = 0;  // One data byte only; byte 3 is padding.
            break;

        case kCinChannelPressure:
            m.type  = MidiType::ChannelPressure;
            m.data2 = 0;  // One data byte only.
            break;

        case kCinPitchBend:
            m.type = MidiType::PitchBend;
            break;

        case kCinSysCommon2:
        case kCinSysCommon3:
            m.type    = MidiType::SystemCommon;
            m.channel = MidiMessage::kChannelNone;
            break;

        case kCinSingleByte:
            // 0xF8-0xFF is system realtime. Anything else with this CIN is a
            // single-byte SysEx tail, which the transport already consumed.
            if (status >= 0xF8)
            {
                m.type    = MidiType::SystemRealtime;
                m.channel = MidiMessage::kChannelNone;
                m.data1   = 0;
                m.data2   = 0;
                break;
            }
            return false;

        case kCinSysExEnd1:
            // Doubles as single-byte system common (tune request, 0xF6).
            if (status == 0xF6)
            {
                m.type    = MidiType::SystemCommon;
                m.channel = MidiMessage::kChannelNone;
                m.data1   = 0;
                m.data2   = 0;
                break;
            }
            return false;

        case kCinSysExStart:
        case kCinSysExEnd2:
        case kCinSysExEnd3:
            // USBConnection reassembles SysEx and delivers it on a separate
            // callback, so these never reach us in practice. Rejected rather
            // than guessed at.
            return false;

        case kCinMisc:
        case kCinCableEvent:
        default:
            return false;
    }

    out = m;
    return true;
}

const char* midiTypeName(MidiType type)
{
    switch (type)
    {
        case MidiType::NoteOff:         return "NoteOff";
        case MidiType::NoteOn:          return "NoteOn";
        case MidiType::PolyPressure:    return "PolyAT";
        case MidiType::ControlChange:   return "CC";
        case MidiType::ProgramChange:   return "PgmChg";
        case MidiType::ChannelPressure: return "ChanAT";
        case MidiType::PitchBend:       return "Bend";
        case MidiType::SystemCommon:    return "SysCom";
        case MidiType::SystemRealtime:  return "RT";
        case MidiType::None:            return "none";
    }
    return "?";
}
