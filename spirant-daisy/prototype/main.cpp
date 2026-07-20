#include "daisy_pod.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

DaisyPod   hw;
Oscillator oscillator;

// Shared between the main loop (MIDI) and the audio callback.
// Single-word float reads/writes are atomic on the Cortex-M7, so no lock is
// needed.
float note_frequency = 440.f;  // set by note-on via mtof()
float breath_level   = 0.f;    // CC2, normalised 0.0–1.0

// One-pole smoothing coefficient for breath_level, set once in init_synth().
// coeff = 1 / (time_seconds * sample_rate); see fonepole() in DaisySP/dsp.h.
float breath_smoothing_coeff = 0.f;

void audio_callback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    // Smoothed toward breath_level each sample to remove zipper noise from
    // the coarse 0-127 CC steps. Must persist across callback invocations.
    static float smoothed_breath_level = 0.f;

    hw.ProcessAllControls();

    // Status LED: white, brightness proportional to breath (CC2).
    // Called once per block (~1 kHz) for smooth software PWM.
    hw.led1.Set(breath_level, breath_level, breath_level);
    hw.UpdateLeds();

    oscillator.SetFreq(note_frequency);

    for(size_t i = 0; i < size; i += 2)
    {
        fonepole(smoothed_breath_level, breath_level, breath_smoothing_coeff);
        float sample = oscillator.Process() * smoothed_breath_level;  // breath IS the VCA
        out[i]     = sample;  // left
        out[i + 1] = sample;  // right
    }
}

void handle_midi_message(MidiEvent message)
{
    switch(message.type)
    {
        case NoteOn:
        {
            NoteOnEvent note_on = message.AsNoteOn();
            if(note_on.velocity > 0)
                note_frequency = mtof(note_on.note);
            break;
        }
        case ControlChange:
        {
            ControlChangeEvent control_change = message.AsControlChange();
            if(control_change.control_number == 2)  // CC2 = breath
                breath_level = control_change.value / 127.f;
            break;
        }
        default:
            break;
    }
}

void init_synth(float sample_rate)
{
    oscillator.Init(sample_rate);
    oscillator.SetWaveform(Oscillator::WAVE_SIN);
    oscillator.SetAmp(1.f);
    oscillator.SetFreq(440.f);

    const float breath_smoothing_time_seconds = 0.008f;  // ~8ms
    breath_smoothing_coeff = 1.0f / (breath_smoothing_time_seconds * sample_rate);
}

int main()
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    float sample_rate = hw.AudioSampleRate();
    init_synth(sample_rate);

    hw.StartAdc();
    hw.StartAudio(audio_callback);
    hw.midi.StartReceive();

    for(;;)
    {
        hw.midi.Listen();
        while(hw.midi.HasEvents())
            handle_midi_message(hw.midi.PopEvent());
    }
}
