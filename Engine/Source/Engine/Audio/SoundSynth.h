#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Procedural sound effects (Architecture §6.6, §10.3): a .sfx description turned into PCM deterministically, at import
// (SoundEffectImporter), so runtime cost is zero. Frozen by the M12 contract (Docs/Decisions/0015-m12-decisions.md), with the
// semantics below, which every .sfx file depends on.
//
// The description (registry structs "SoundEffect", "SoundLayer", "SoundEnvelope" and enum "SoundWave", registered by
// AssetPipeline's SoundEffectImporter::RegisterTypes; the Audio module includes only Core):
//   - A sound is the sum of its layers, times Volume, clamped to [-1, 1] and quantized to 48 kHz mono signed 16-bit PCM:
//     sample = floor(x * 32767 + 0.5) for x >= 0 and -floor(-x * 32767 + 0.5) below (round half away from zero).
//   - Timing is in whole frames (samples at 48 kHz). Every duration (a note's, Duration, Delay, Attack, Decay, Release) is
//     converted to frames once, from its float value as the description stores it (a note's decimal rounded to the nearest
//     float by ParseSoundNote), as F(s) = floor(double(s) * 48000 + 0.5); the product is exact in double. Nothing else is
//     timed in seconds, so a sum of decimal durations never lands one frame off ("A3:0.05", "E4:0.1" and the default
//     Release 0.01 are 2,400 + 4,800 + 480 = 7,680 frames).
//   - A layer starts at frame F(Delay) and plays either its Notes, back to back (a note starts at the sum of the earlier
//     notes' F, counted from the layer's start, and lasts F of its own seconds), or, without notes, one tone of N =
//     F(Duration) frames whose frequency at frame n of the tone moves exponentially from StartFrequency to EndFrequency
//     (f(n) = f0 * (f1 / f0)^(n / N)). A note is "<Name><Octave>:<seconds>": Name C, C#, Db, D, D#, Eb, E, F, F#, Gb, G, G#,
//     Ab, A, A#, Bb or B, Octave 0 to 8 (equal temperament, A4 = 440 Hz: f = 440 * 2^((midi - 69) / 12), midi = 12 *
//     (Octave + 1) + semitone), or "R:<seconds>" for a rest; seconds > 0 in plain decimal notation ("C5:0.06") with at
//     most 7 digits before and 8 after the decimal point, so the decimal rounds to the nearest float exactly through
//     double. Every layer lasts at least one frame (F of its notes or Duration, plus nR, is not 0). A tone sweeps only over
//     its N frames and holds EndFrequency during its release tail.
//   - Every note (and the one tone) gets its own ADSR envelope, linear in each segment, with nA = F(Attack), nD = F(Decay),
//     nR = F(Release), S = Sustain and the note's N frames: at frame n of the note the level is n / nA for n < nA, then
//     1 - (1 - S) (n - nA) / nD for n < nA + nD, then S, until the note's end at n = N; from there it falls linearly from L,
//     the level that rule gives at n = N, to 0: L (1 - (n - N) / nR) for N <= n < N + nR, after the note's end (a release
//     tail overlaps the next note and is summed with it), and 0 afterwards. So a note that ends during its attack or decay
//     releases from the level it reached, and a zero-length segment is skipped. A layer spans F(Delay) + the sum of its
//     notes' F (or F(Duration)) + nR frames; the sound's length in frames is the largest span of its layers.
//   - Waves at phase p in [0, 1), the phase advancing by f / 48000 per sample from 0 at each note's start: Sine sin(2 pi p),
//     Square +1 for p < DutyCycle else -1, Triangle 1 - 4 |p - 0.5|, Saw 2p - 1, and Noise a value drawn uniformly in
//     [-1, 1) from the layer's generator each time the phase wraps (sample and hold, so the frequency sets the noise's
//     pitch). Layer i's generator is a Core Random seeded with Hash64(Seed, i); Noise draws its first value at each note's
//     start, the phase's first wrap.
//   - LowPass > 0 runs the layer through a one-pole low-pass at that cutoff in Hz (y += a (x - y), a = 1 - exp(-2 pi fc /
//     48000), y = 0 at the layer's start) over the layer's span only, from F(Delay) to the end of its last release; 0
//     leaves it unfiltered. The layer's Volume applies after the filter.
// Determinism (§10.3: "synthesis is bit-identical across runs"; "SoundSynth: presets hash to committed values"): the
// synthesizer computes in double precision with Core/DetMath (Sin, Exp, Pow), never the C runtime's transcendental
// functions, iterates layers and notes in order and uses no threads, so the PCM, and the hashes committed for the presets,
// are identical in every configuration and on every platform.

namespace Engine {

	// Registry enum "SoundWave".
	enum class SoundWave : uint8_t
	{
		Sine,
		Square,
		Triangle,
		Saw,
		Noise
	};

	// Registry struct "SoundEnvelope": seconds, except Sustain, a level in [0, 1]. The default plays at full level with a
	// 5 ms fade-in and a 10 ms fade-out, so a layer without an envelope does not click.
	struct SoundEnvelope
	{
		float Attack = 0.005f;
		float Decay = 0.0f;
		float Sustain = 1.0f;
		float Release = 0.01f;

		bool operator==(const SoundEnvelope&) const = default;
	};

	// Registry struct "SoundLayer" (§6.6's layer keys plus LowPass and Delay).
	struct SoundLayer
	{
		SoundWave Wave = SoundWave::Square;
		float DutyCycle = 0.5f;           // Square only: the high fraction of a period, 0.01 to 0.99
		std::vector<std::string> Notes{}; // "C5:0.06", "R:0.1"; empty: the tone below
		float Duration = 0.1f;            // seconds, without Notes: the tone's length (> 0)
		float StartFrequency = 440.0f;    // Hz, without Notes: 20 to 20,000
		float EndFrequency = 440.0f;      // Hz, without Notes: 20 to 20,000
		float Volume = 1.0f;              // linear, 0 to 1
		SoundEnvelope Envelope{};
		float LowPass = 0.0f; // Hz: 0 (off) or 20 to 20,000
		float Delay = 0.0f;   // seconds from the sound's start, >= 0

		bool operator==(const SoundLayer&) const = default;
	};

	// Registry struct "SoundEffect": a .sfx file's fields after "Format": "SoundEffect" and "Version": 1 (§6.6).
	struct SoundEffectDescription
	{
		uint32_t Seed = 0;
		float Volume = 1.0f; // linear, 0 to 1
		std::vector<SoundLayer> Layers{};

		bool operator==(const SoundEffectDescription&) const = default;
	};

	// One parsed note.
	struct SoundNote
	{
		bool Rest = false;
		float Frequency = 0.0f; // Hz; 0 for a rest
		float Duration = 0.0f;  // seconds, > 0
		int32_t Midi = -1;      // the MIDI note number; -1 for a rest
	};

	// The limits ValidateSoundEffect enforces.
	inline constexpr uint32_t MaxSoundLayers = 16;
	inline constexpr uint32_t MaxSoundNotesPerLayer = 256;
	inline constexpr double MaxSoundEffectSeconds = 10.0;
	// The synthesizer's output format (§6.6: "48 kHz mono 16-bit PCM").
	inline constexpr uint32_t SoundSynthSampleRate = 48000;
	// MaxSoundEffectSeconds in frames: the longest sound ValidateSoundEffect accepts.
	inline constexpr uint64_t MaxSoundEffectFrames = 480000;

	// Parses one note (see the file comment). Errors: Validation naming the note and what is wrong (an unknown name, an
	// octave outside 0 to 8, a missing ':', a duration that is not a positive finite decimal).
	[[nodiscard]] Result<SoundNote> ParseSoundNote(std::string_view note);

	// Checks every rule of the file comment and the field comments: 1 to MaxSoundLayers layers; per layer the ranges above,
	// at most MaxSoundNotesPerLayer valid notes, or no notes and a Duration > 0; every float finite; a length
	// (ComputeSoundEffectFrameCount) of at most MaxSoundEffectFrames. Errors: Validation with one issue per violation, each
	// located by its JSON pointer into the .sfx document ("/Layers/1/Notes/3", "/Layers/0/Envelope/Attack", "/Volume").
	// A range compares the float with its bounds rounded to float, as the type registry does, so a file that spells
	// DutyCycle 0.99 is accepted. The span rules (at least one frame, at most MaxSoundEffectFrames) are checked per layer
	// and located at "/Layers/<i>", only for a layer whose fields are valid.
	[[nodiscard]] Status ValidateSoundEffect(const SoundEffectDescription& description);

	// The sound's length in frames at 48 kHz (the file comment's rule: the largest layer span), for a description
	// ValidateSoundEffect accepts.
	[[nodiscard]] uint64_t ComputeSoundEffectFrameCount(const SoundEffectDescription& description);

	// The sound's length in seconds: ComputeSoundEffectFrameCount / 48000, for a description ValidateSoundEffect accepts.
	[[nodiscard]] double ComputeSoundEffectDuration(const SoundEffectDescription& description);

	// Synthesizes the sound: ComputeSoundEffectFrameCount samples of 48 kHz mono 16-bit PCM. Bit-identical for equal
	// descriptions in every run, configuration and platform. Errors: those of ValidateSoundEffect.
	[[nodiscard]] Result<std::vector<int16_t>> SynthesizeSoundEffect(const SoundEffectDescription& description);

	// "Sine", "Square", "Triangle", "Saw", "Noise"; "Unknown" outside the enum (asserted).
	[[nodiscard]] std::string_view SoundWaveToString(SoundWave wave);

}
