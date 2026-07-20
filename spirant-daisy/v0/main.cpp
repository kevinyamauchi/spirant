#include "daisy_pod.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

DaisyPod hw;

// --- Michael Brecker EWI-style lead -----------------------------------------
// A saw-ish VariableShapeOscillator run through the classic expressive-lead
// chain: resonant lowpass -> overdrive -> chorus (mono thickener), with a
// stereo ping-pong delay widening the field, into a hall reverb.
//
//   voice (var_shape)
//     -> LadderFilter (LP24)      breath-driven, resonant "vocal" cutoff
//     -> Overdrive                dynamic soft-clip grit (post-envelope)
//     -> Chorus (mono/stereo)     lush analog thickening; switch 1 selects width
//     +  stereo ping-pong delay   width, fed from the mono pre-chorus lead
//     -> ReverbSc (stereo)        hall space
//
// The key to the EWI feel is that breath pressure moves *both* brightness and
// volume together. Breath arrives as MIDI CC2 (0-127), normalised and smoothed,
// and drives both the filter cutoff and the amplitude. The amplitude is applied
// *before* the overdrive, so soft notes stay clean and hard notes bite --
// dynamic timbre, like a real reed. Pitch arrives as MIDI note-on and latches;
// breath alone controls loudness, so a note falls silent when breath drops to
// zero (no note-off needed), just like a wind controller.
//
// In mono chorus mode the dry lead is kept dead center; additional stereo width
// comes from the ping-pong delay's L/R taps (and the reverb). Stereo chorus mode
// also spreads the core lead itself.
//
// Pots:
//   knob1 -> ping-pong delay time
//   knob2 -> overdrive amount (SetDrive)
// Encoder:
//   turn  -> reverb send amount (dry -> wet), accumulated from detent ticks
// Switch:
//   button1 -> toggle chorus mono/stereo (LED1 white = mono, cyan = stereo)
VariableShapeOscillator var_shape;
LadderFilter            filt;
Overdrive               drive;
Chorus                  chorus;
ReverbSc                verb;

// Filter brightness ceiling, fixed at 66% (knob1 free).
constexpr float kBrightness = 0.66f;

// Reverb send (dry + wet). The encoder nudges this live; starts at 75% wet.
constexpr float kReverbStep = 0.05f;   // change per encoder detent
float           reverb_send = 0.75f;   // current send, clamped to [0, 1]

// Output trim after the drive's makeup gain (peak level before reverb).
constexpr float kPlayLevel = 0.4f;

// Chorus mode, toggled live by switch 1 (button1). Mono = classic dead-center
// thickener; stereo = decorrelated engines spread hard L/R. Only touched in the
// audio callback. Applied via apply_chorus_mode() on init and on each toggle.
bool chorus_stereo = false;

// --- MIDI breath control ----------------------------------------------------
// Shared between the MIDI loop (main) and the audio callback. Single-word float
// reads/writes are atomic on the Cortex-M7, so no lock is needed.
float note_frequency = 440.f;  // set by note-on via mtof()
float breath_level   = 0.f;    // CC2, normalised 0.0-1.0

// One-pole smoothing coefficient for breath_level, set once in init_synth().
// coeff = 1 / (time_seconds * sample_rate); see fonepole() in DaisySP/dsp.h.
float breath_smoothing_coeff = 0.f;

// --- Stereo ping-pong delay -------------------------------------------------
// Two delay lines fed from the mono lead. The left tap feeds the right line and
// the right tap feeds (damped) back into the left, so repeats bounce L->R->L
// and decay. The delay lines are large, so they live in SDRAM.
#define MAX_DELAY ((size_t)(48000 * 1.0f))  // 1 s per line @ 48 kHz
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_l;
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_r;
OnePole                     delay_damp;  // darkens repeats in the feedback loop

constexpr float kDelayFeedback = 0.4f;   // repeat decay
constexpr float kDelayMix      = 0.5f;   // wet level of the ping-pong taps

// Knob1 sweeps the delay time over this range (seconds), mapped exponentially
// so the low end has fine control. Kept below MAX_DELAY (1 s).
constexpr float kDelayMinSec = 0.04f;
constexpr float kDelayMaxSec = 0.75f;

// Default ping-pong time the knob starts near (~dotted-eighth at 110 BPM).
constexpr float kDelayTimeSec = 0.409f;

float g_sample_rate    = 48000.f;  // set in init, needed to convert time->samples
float delay_time_smooth = 0.f;     // glided delay length in samples

// Set the oscillator pitch. VariableShapeOscillator's audible (slave) pitch is
// SetSyncFreq; SetFreq only feeds the (disabled) sync master.
void set_pitch(float freq)
{
    var_shape.SetSyncFreq(freq);
}

// Configure the two chorus engines for the current mode. Called on init and on
// each switch-1 toggle -- never per-sample. Mono: identical engines panned to
// center, so GetLeft() == GetRight() and the lead stays dead center (matching
// the original patch). Stereo: decorrelated rates/delays panned hard L/R, so the
// two outputs spread across the field.
void apply_chorus_mode()
{
    if(chorus_stereo)
    {
        chorus.SetLfoFreq(0.40f, 0.55f);
        chorus.SetDelay(0.5f, 0.7f);
        chorus.SetLfoDepth(0.35f, 0.30f);
        chorus.SetPan(0.0f, 1.0f);
    }
    else
    {
        chorus.SetLfoFreq(0.5f, 0.5f);
        chorus.SetDelay(0.6f, 0.6f);
        chorus.SetLfoDepth(0.35f, 0.35f);
        chorus.SetPan(0.5f, 0.5f);
    }
}

void audio_callback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    // Smoothed toward breath_level each sample to remove zipper noise from the
    // coarse 0-127 CC steps. Must persist across callback invocations.
    static float smoothed_breath = 0.f;

    hw.ProcessAllControls();

    // Switch 1 toggles the chorus mono/stereo mode (RisingEdge is debounced and
    // true for one block per press). Reconfigure the engines only on the change.
    if(hw.button1.RisingEdge())
    {
        chorus_stereo = !chorus_stereo;
        apply_chorus_mode();
    }

    // Encoder ticks nudge the reverb send (ProcessAllControls already debounced
    // the encoder). Increment() returns -1/0/+1 per detent; clamp to [0, 1].
    reverb_send = fclamp(reverb_send + hw.encoder.Increment() * kReverbStep,
                         0.f,
                         1.f);

    const float drive_amount = hw.knob2.Value();  // overdrive

    // Knob1 sets the ping-pong delay time (samples), smoothed per-sample below.
    const float delay_target = g_sample_rate
        * fmap(hw.knob1.Value(), kDelayMinSec, kDelayMaxSec, Mapping::EXP);

    // Fixed saw-leaning timbre for the classic analog lead.
    var_shape.SetWaveshape(0.15f);  // toward saw/ramp
    var_shape.SetPW(0.5f);

    set_pitch(note_frequency);  // latched by the most recent note-on

    drive.SetDrive(drive_amount);

    // Cutoff ceiling the envelope opens toward, mapped exponentially so the
    // fixed 66% brightness sits musically in the 400 Hz .. 9 kHz range.
    const float cutoff_ceiling = fmap(kBrightness, 400.f, 9000.f, Mapping::EXP);

    // LED1: hue shows chorus mode (white = mono, cyan = stereo), brightness
    // proportional to breath (CC2). Cyan drops the red channel.
    const float led1_r = chorus_stereo ? 0.f : breath_level;
    hw.led1.Set(led1_r, breath_level, breath_level);
    // LED2: blue, brightness proportional to the reverb send (encoder).
    hw.led2.Set(0.f, 0.f, reverb_send);
    hw.UpdateLeds();

    for(size_t i = 0; i < size; i += 2)
    {
        fonepole(smoothed_breath, breath_level, breath_smoothing_coeff);

        // Breath drives brightness and amplitude together (the EWI feel).
        // Opens the filter from a nearly-closed base toward the knob-set
        // ceiling; squared for a more natural swell.
        const float cutoff
            = 150.f + smoothed_breath * smoothed_breath * (cutoff_ceiling - 150.f);
        filt.SetFreq(cutoff);

        float sig = var_shape.Process();
        sig = filt.Process(sig);
        sig *= smoothed_breath;  // breath IS the VCA; feeds the drive level-dependent
        sig = drive.Process(sig);
        sig *= kPlayLevel;       // output trim after the drive's gain

        // Chorus runs both engines; sig is the mono pre-chorus lead. In mono
        // mode the L/R outputs are identical (dead-center thickener); in stereo
        // mode they are decorrelated and spread. Mode set by switch 1.
        chorus.Process(sig);
        float chorus_l = chorus.GetLeft();
        float chorus_r = chorus.GetRight();

        // Glide the delay length toward the knob target to avoid zipper/pitch
        // artifacts, then apply it to both lines.
        fonepole(delay_time_smooth, delay_target, 0.0005f);
        delay_l.SetDelay(delay_time_smooth);
        delay_r.SetDelay(delay_time_smooth);

        // Ping-pong: left tap feeds the right line; right tap feeds (damped)
        // back into the left. Repeats alternate L->R->L and darken as they decay.
        float dl = delay_l.Read();
        float dr = delay_r.Read();
        delay_l.Write(sig + delay_damp.Process(dr) * kDelayFeedback);
        delay_r.Write(dl * kDelayFeedback);

        // Chorus (centered in mono mode, spread in stereo) plus the ping-pong
        // taps hard L/R.
        float pre_l = chorus_l + dl * kDelayMix;
        float pre_r = chorus_r + dr * kDelayMix;

        // Reverb send (stereo in/out); wet amount set live by the encoder.
        float wetl, wetr;
        verb.Process(pre_l, pre_r, &wetl, &wetr);
        out[i]     = pre_l + wetl * reverb_send;  // left
        out[i + 1] = pre_r + wetr * reverb_send;  // right
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
    var_shape.Init(sample_rate);

    filt.Init(sample_rate);
    filt.SetFilterMode(LadderFilter::FilterMode::LP24);
    filt.SetRes(0.35f);  // slight vocal resonant peak

    drive.Init();

    chorus.Init(sample_rate);
    chorus.SetFeedback(0.2f);   // same for both engines/modes
    apply_chorus_mode();        // seed per-engine rate/delay/depth/pan (mono)

    verb.Init(sample_rate);
    verb.SetFeedback(0.85f);  // long hall tail
    verb.SetLpFreq(7000.f);   // natural, not-too-bright tail

    delay_l.Init();
    delay_r.Init();
    // Seed the smoothed time at the musical default so it doesn't glide up from
    // zero at startup; knob1 takes over from there.
    delay_time_smooth = sample_rate * kDelayTimeSec;
    delay_l.SetDelay(delay_time_smooth);
    delay_r.SetDelay(delay_time_smooth);

    delay_damp.Init();
    delay_damp.SetFilterMode(OnePole::FILTER_MODE_LOW_PASS);
    delay_damp.SetFrequency(0.08f);  // ~3.8 kHz @ 48 kHz, darkens each repeat

    const float breath_smoothing_time_seconds = 0.008f;  // ~8 ms
    breath_smoothing_coeff = 1.f / (breath_smoothing_time_seconds * sample_rate);

    set_pitch(note_frequency);
}

int main()
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    float sample_rate = hw.AudioSampleRate();
    g_sample_rate     = sample_rate;
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
