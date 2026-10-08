#include "EnginePCH.h"
#include "Engine/Audio/SoundSynth.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <utility>

namespace Engine {

	namespace {

		// One pitch name of the note grammar (SoundSynth.h) and its semitone above C.
		struct SoundNoteName
		{
			std::string_view Name{};
			int32_t Semitone = 0;
		};

		// A note as the synthesizer uses it: ParseSoundNote's fields with the frequency in double precision.
		struct ParsedSoundNote
		{
			bool Rest = false;
			int32_t Midi = -1;
			float Duration = 0.0f;  // seconds, > 0
			double Frequency = 0.0; // Hz; 0 for a rest
		};

		// One tone of a layer: a note (or the layer's sweep), placed in frames from the layer's start.
		struct SoundToneSpan
		{
			uint64_t Start = 0;  // frames from the layer's start (after its Delay)
			uint64_t Frames = 0; // N, the tone's own length; its release tail follows
			double StartFrequency = 0.0;
			double EndFrequency = 0.0;
		};

		// The ADSR envelope of a layer in frames (SoundSynth.h: nA, nD, nR and S).
		struct SoundEnvelopeFrames
		{
			uint64_t Attack = 0;
			uint64_t Decay = 0;
			uint64_t Release = 0;
			double Sustain = 1.0;
		};

	}

	namespace Utils {

		static constexpr std::array<SoundNoteName, 17> SoundNoteNames = { {
			{ "C", 0 },
			{ "C#", 1 },
			{ "Db", 1 },
			{ "D", 2 },
			{ "D#", 3 },
			{ "Eb", 3 },
			{ "E", 4 },
			{ "F", 5 },
			{ "F#", 6 },
			{ "Gb", 6 },
			{ "G", 7 },
			{ "G#", 8 },
			{ "Ab", 8 },
			{ "A", 9 },
			{ "A#", 10 },
			{ "Bb", 10 },
			{ "B", 11 },
		} };

		// The note grammar's octaves and the decimal digits a duration may have. With at most 8 decimal places and 15 digits
		// in all, the digits form an integer below 2^53 and the place value a power of ten, both exact in double, so their
		// quotient is the decimal correctly rounded to double, and converting that to float is the decimal correctly rounded
		// to float (no double-rounding error is possible at 8 decimal places).
		static constexpr int32_t MaxSoundNoteOctave = 8;
		static constexpr size_t MaxDurationIntegerDigits = 7;
		static constexpr size_t MaxDurationDecimalPlaces = 8;
		static constexpr std::array<double, MaxDurationDecimalPlaces + 1> PowersOfTen = { 1.0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8 };

		// The field ranges of SoundSynth.h (the registry's metadata, SoundEffectImporter::RegisterTypes, uses the same).
		static constexpr double MinDutyCycle = 0.01;
		static constexpr double MaxDutyCycle = 0.99;
		static constexpr double MinSoundFrequency = 20.0;
		static constexpr double MaxSoundFrequency = 20000.0;
		static constexpr double MaxSegmentSeconds = 10.0;

		// The largest PCM magnitude (SoundSynth.h's quantization).
		static constexpr double PcmScale = 32767.0;
		// F(s) of anything at least this many frames saturates; far above MaxSoundEffectFrames and exact in double.
		static constexpr double FrameSaturation = 4503599627370496.0; // 2^52

		[[nodiscard]] static uint64_t SaturatingAdd(uint64_t a, uint64_t b)
		{
			return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max() : a + b;
		}

		// F(s) = floor(double(s) * 48000 + 0.5) (SoundSynth.h). Defined for every float so that the length of an invalid
		// description can still be computed: 0 for NaN, zero and negative values, saturated for huge ones.
		[[nodiscard]] static uint64_t SecondsToFrames(float seconds)
		{
			if (!(seconds > 0.0f))
				return 0;
			const double frames = std::floor(static_cast<double>(seconds) * static_cast<double>(SoundSynthSampleRate) + 0.5);
			if (!(frames < FrameSaturation))
				return static_cast<uint64_t>(FrameSaturation);
			return static_cast<uint64_t>(frames);
		}

		// The equal-temperament frequency of a MIDI note: 440 * 2^((midi - 69) / 12).
		[[nodiscard]] static double GetMidiFrequency(int32_t midi)
		{
			return 440.0 * DetMath::Pow(2.0, static_cast<double>(midi - 69) / 12.0);
		}

		[[nodiscard]] static bool IsDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		[[nodiscard]] static std::unexpected<Error> MakeNoteError(std::string_view note, std::string_view problem)
		{
			return std::unexpected(Error(ErrorCode::Validation, std::format("invalid note '{}': {}", note, problem))
					.WithHint("write \"<Name><Octave>:<seconds>\" such as \"C5:0.06\" (names C, C#, Db, D, D#, Eb, E, F, F#, Gb, G, G#, Ab, A, A#, Bb, B; "
							  "octaves 0 to 8), or \"R:<seconds>\" for a rest"));
		}

		// The duration of a note: a positive plain decimal ("0.06", "2", "0.125").
		[[nodiscard]] static Result<float> ParseNoteDuration(std::string_view note, std::string_view text)
		{
			const size_t point = text.find('.');
			const std::string_view integerDigits = text.substr(0, point);
			const std::string_view decimalDigits = point == std::string_view::npos ? std::string_view() : text.substr(point + 1);
			const bool wellFormed = !integerDigits.empty() && std::ranges::all_of(integerDigits, IsDigit) && std::ranges::all_of(decimalDigits, IsDigit)
				&& (point == std::string_view::npos || !decimalDigits.empty());
			if (!wellFormed)
				return MakeNoteError(note, std::format("the duration '{}' is not a plain decimal number of seconds", text));
			if (integerDigits.size() > MaxDurationIntegerDigits || decimalDigits.size() > MaxDurationDecimalPlaces)
			{
				return MakeNoteError(note, std::format("the duration '{}' has more than {} digits before or {} after the decimal point", text, MaxDurationIntegerDigits, MaxDurationDecimalPlaces));
			}

			uint64_t digits = 0;
			for (const char character : integerDigits)
				digits = digits * 10 + static_cast<uint64_t>(character - '0');
			for (const char character : decimalDigits)
				digits = digits * 10 + static_cast<uint64_t>(character - '0');
			if (digits == 0)
				return MakeNoteError(note, std::format("the duration '{}' is not positive", text));
			const double seconds = static_cast<double>(digits) / PowersOfTen[decimalDigits.size()];
			return static_cast<float>(seconds);
		}

		[[nodiscard]] static Result<ParsedSoundNote> ParseNote(std::string_view note)
		{
			const size_t colon = note.find(':');
			if (colon == std::string_view::npos)
				return MakeNoteError(note, "a ':' must separate the pitch from the duration");
			const std::string_view pitch = note.substr(0, colon);
			ParsedSoundNote parsed;
			if (pitch == "R")
			{
				parsed.Rest = true;
			}
			else
			{
				if (pitch.empty())
					return MakeNoteError(note, "the pitch is missing");
				const size_t nameLength = pitch.size() >= 2 && (pitch[1] == '#' || pitch[1] == 'b') ? 2 : 1;
				const std::string_view name = pitch.substr(0, nameLength);
				const auto found = std::ranges::find(SoundNoteNames, name, &SoundNoteName::Name);
				if (found == SoundNoteNames.end())
					return MakeNoteError(note, std::format("'{}' is not a note name", name));
				const std::string_view octave = pitch.substr(nameLength);
				if (octave.empty())
					return MakeNoteError(note, "the octave is missing");
				if (!std::ranges::all_of(octave, IsDigit))
					return MakeNoteError(note, std::format("the octave '{}' is not a number", octave));
				if (octave.size() != 1 || octave[0] - '0' > MaxSoundNoteOctave)
					return MakeNoteError(note, std::format("the octave {} is outside 0 to {}", octave, MaxSoundNoteOctave));
				parsed.Midi = 12 * (octave[0] - '0' + 1) + found->Semitone;
				parsed.Frequency = GetMidiFrequency(parsed.Midi);
			}
			ENGINE_TRY_ASSIGN(parsed.Duration, ParseNoteDuration(note, note.substr(colon + 1)));
			return parsed;
		}

		// The frames a layer spans: F(Delay) + its notes' F (or F(Duration)) + F(Release). Invalid notes count 0 frames.
		[[nodiscard]] static uint64_t GetLayerSpan(const SoundLayer& layer)
		{
			uint64_t span = SecondsToFrames(layer.Delay);
			if (layer.Notes.empty())
			{
				span = SaturatingAdd(span, SecondsToFrames(layer.Duration));
			}
			else
			{
				for (const std::string& text : layer.Notes)
				{
					const Result<ParsedSoundNote> note = ParseNote(text);
					if (note.has_value())
						span = SaturatingAdd(span, SecondsToFrames(note->Duration));
				}
			}
			return SaturatingAdd(span, SecondsToFrames(layer.Envelope.Release));
		}

		// Whether `value` lies in [min, max] as the registry's field metadata compares it: against the bounds rounded to
		// float, so a file that spells a bound exactly (DutyCycle 0.99, just above 0.99 as a float) is accepted.
		[[nodiscard]] static bool IsInFloatRange(float value, double min, double max)
		{
			return value >= static_cast<float>(min) && value <= static_cast<float>(max);
		}

		// The issues ValidateSoundEffect collects, each located by its JSON pointer into the .sfx document.
		class SoundEffectIssues
		{
		public:
			void Add(std::string pointer, std::string message, std::string hint = {})
			{
				m_Issues.push_back(ErrorIssue{ .JsonPointer = std::move(pointer), .Message = std::move(message), .Hint = std::move(hint), .Suggestions = {} });
			}

			// A finite value in [min, max] (IsInFloatRange).
			void CheckRange(const std::string& pointer, std::string_view name, float value, double min, double max)
			{
				if (!std::isfinite(value))
					Add(pointer, std::format("{} must be a finite number", name));
				else if (!IsInFloatRange(value, min, max))
					Add(pointer, std::format("{} must be between {} and {} (got {})", name, min, max, value));
			}

			[[nodiscard]] size_t GetCount() const { return m_Issues.size(); }

			[[nodiscard]] Status ToStatus() const
			{
				if (m_Issues.empty())
					return {};
				std::string message = m_Issues.size() == 1 ? m_Issues.front().Message : std::format("{} invalid fields in the sound effect", m_Issues.size());
				ErrorLocation location;
				location.JsonPointer = std::string();
				return std::unexpected(Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location)).WithIssues(m_Issues));
			}
		private:
			std::vector<ErrorIssue> m_Issues;
		};

		static void ValidateLayer(const SoundLayer& layer, const std::string& base, SoundEffectIssues& issues)
		{
			const size_t issuesBefore = issues.GetCount();
			if (std::to_underlying(layer.Wave) > std::to_underlying(SoundWave::Noise))
				issues.Add(base + "/Wave", std::format("Wave {} is not a SoundWave (Sine, Square, Triangle, Saw or Noise)", std::to_underlying(layer.Wave)));
			issues.CheckRange(base + "/DutyCycle", "DutyCycle", layer.DutyCycle, MinDutyCycle, MaxDutyCycle);

			if (layer.Notes.size() > MaxSoundNotesPerLayer)
				issues.Add(base + "/Notes", std::format("a layer has at most {} notes (got {})", MaxSoundNotesPerLayer, layer.Notes.size()));
			for (size_t index = 0; index < layer.Notes.size() && index < MaxSoundNotesPerLayer; ++index)
			{
				const Result<ParsedSoundNote> note = ParseNote(layer.Notes[index]);
				if (!note.has_value())
					issues.Add(std::format("{}/Notes/{}", base, index), note.error().GetMessageText(), note.error().GetHint());
			}

			issues.CheckRange(base + "/Duration", "Duration", layer.Duration, 0.0, MaxSegmentSeconds);
			if (layer.Notes.empty() && layer.Duration == 0.0f)
				issues.Add(base + "/Duration", "a layer without notes needs a Duration > 0", "give the sweep a length in seconds, or list notes");
			issues.CheckRange(base + "/StartFrequency", "StartFrequency", layer.StartFrequency, MinSoundFrequency, MaxSoundFrequency);
			issues.CheckRange(base + "/EndFrequency", "EndFrequency", layer.EndFrequency, MinSoundFrequency, MaxSoundFrequency);
			issues.CheckRange(base + "/Volume", "Volume", layer.Volume, 0.0, 1.0);
			issues.CheckRange(base + "/Envelope/Attack", "Attack", layer.Envelope.Attack, 0.0, MaxSegmentSeconds);
			issues.CheckRange(base + "/Envelope/Decay", "Decay", layer.Envelope.Decay, 0.0, MaxSegmentSeconds);
			issues.CheckRange(base + "/Envelope/Sustain", "Sustain", layer.Envelope.Sustain, 0.0, 1.0);
			issues.CheckRange(base + "/Envelope/Release", "Release", layer.Envelope.Release, 0.0, MaxSegmentSeconds);
			if (!std::isfinite(layer.LowPass))
				issues.Add(base + "/LowPass", "LowPass must be a finite number");
			else if (layer.LowPass != 0.0f && !IsInFloatRange(layer.LowPass, MinSoundFrequency, MaxSoundFrequency))
				issues.Add(base + "/LowPass", std::format("LowPass must be 0 (off) or between {} and {} Hz (got {})", MinSoundFrequency, MaxSoundFrequency, layer.LowPass));
			issues.CheckRange(base + "/Delay", "Delay", layer.Delay, 0.0, MaxSegmentSeconds);

			// The length rules apply to a layer whose fields are valid (an invalid duration already has its own issue).
			if (issues.GetCount() != issuesBefore)
				return;
			const uint64_t span = GetLayerSpan(layer);
			if (span == 0)
			{
				issues.Add(base, "the layer lasts no frame at 48 kHz", "lengthen its notes, Duration or Release");
			}
			else if (span > MaxSoundEffectFrames)
			{
				issues.Add(base, std::format("the layer lasts {} frames ({} s), longer than the {} s limit of a sound effect", span, static_cast<double>(span) / SoundSynthSampleRate, MaxSoundEffectSeconds),
					"shorten its Delay, notes, Duration or Release");
			}
		}

		[[nodiscard]] static SoundEnvelopeFrames GetEnvelopeFrames(const SoundEnvelope& envelope)
		{
			return SoundEnvelopeFrames{
				.Attack = SecondsToFrames(envelope.Attack),
				.Decay = SecondsToFrames(envelope.Decay),
				.Release = SecondsToFrames(envelope.Release),
				.Sustain = static_cast<double>(envelope.Sustain),
			};
		}

		// The level of a note's attack, decay and sustain at frame n (SoundSynth.h); zero-length segments are skipped.
		[[nodiscard]] static double GetSustainLevel(const SoundEnvelopeFrames& envelope, uint64_t frame)
		{
			if (frame < envelope.Attack)
				return static_cast<double>(frame) / static_cast<double>(envelope.Attack);
			if (frame - envelope.Attack < envelope.Decay)
				return 1.0 - (1.0 - envelope.Sustain) * static_cast<double>(frame - envelope.Attack) / static_cast<double>(envelope.Decay);
			return envelope.Sustain;
		}

		// The frequency at frame n of a tone of N = tone.Frames frames: the exponential sweep f0 * (f1 / f0)^(n / N), held at
		// f1 for the release tail after the tone's end. A constant tone skips Pow, whose result for a base of 1 is exactly 1.
		[[nodiscard]] static double GetToneFrequency(const SoundToneSpan& tone, uint64_t frame)
		{
			if (tone.StartFrequency == tone.EndFrequency)
				return tone.StartFrequency;
			if (frame >= tone.Frames)
				return tone.EndFrequency;
			return tone.StartFrequency
				* DetMath::Pow(tone.EndFrequency / tone.StartFrequency, static_cast<double>(frame) / static_cast<double>(tone.Frames));
		}

		// The wave at phase p in [0, 1) (SoundSynth.h); `held` is the Noise wave's current sample-and-hold value.
		[[nodiscard]] static double GetWaveValue(const SoundLayer& layer, double phase, double held)
		{
			switch (layer.Wave)
			{
				case SoundWave::Sine:     return DetMath::Sin(2.0 * std::numbers::pi * phase);
				case SoundWave::Square:   return phase < static_cast<double>(layer.DutyCycle) ? 1.0 : -1.0;
				case SoundWave::Triangle: return 1.0 - 4.0 * std::abs(phase - 0.5);
				case SoundWave::Saw:      return 2.0 * phase - 1.0;
				case SoundWave::Noise:    return held;
			}
			ENGINE_CORE_ASSERT(false, "Unknown SoundWave {}", std::to_underlying(layer.Wave));
			return 0.0;
		}

		// Adds one tone and its release tail, enveloped, to `samples` (the layer from its start, after its Delay).
		static void RenderTone(const SoundLayer& layer, const SoundEnvelopeFrames& envelope, const SoundToneSpan& tone, Random& random,
			std::vector<double>& samples)
		{
			const bool noise = layer.Wave == SoundWave::Noise;
			const double releaseLevel = GetSustainLevel(envelope, tone.Frames);
			const uint64_t length = tone.Frames + envelope.Release;
			double phase = 0.0;
			double held = noise ? random.RangeDouble(-1.0, 1.0) : 0.0;
			for (uint64_t frame = 0; frame < length; ++frame)
			{
				double level = 0.0;
				if (frame < tone.Frames)
					level = GetSustainLevel(envelope, frame);
				else
					level = releaseLevel * (1.0 - static_cast<double>(frame - tone.Frames) / static_cast<double>(envelope.Release));
				samples[static_cast<size_t>(tone.Start + frame)] += GetWaveValue(layer, phase, held) * level;

				phase += GetToneFrequency(tone, frame) / static_cast<double>(SoundSynthSampleRate);
				if (phase >= 1.0)
				{
					phase -= 1.0;
					if (noise)
						held = random.RangeDouble(-1.0, 1.0);
				}
			}
		}

		// Synthesizes layer `index` of a valid description and adds it, times its Volume, to `mix`.
		static void RenderLayer(const SoundEffectDescription& description, size_t index, std::vector<double>& mix)
		{
			const SoundLayer& layer = description.Layers[index];
			const SoundEnvelopeFrames envelope = GetEnvelopeFrames(layer.Envelope);
			Random random(Hash64(description.Seed, static_cast<uint64_t>(index)));

			std::vector<SoundToneSpan> tones;
			uint64_t toneFrames = 0;
			if (layer.Notes.empty())
			{
				toneFrames = SecondsToFrames(layer.Duration);
				tones.push_back(SoundToneSpan{ .Start = 0, .Frames = toneFrames, .StartFrequency = layer.StartFrequency, .EndFrequency = layer.EndFrequency });
			}
			else
			{
				for (const std::string& text : layer.Notes)
				{
					const Result<ParsedSoundNote> note = ParseNote(text);
					ENGINE_CORE_ASSERT(note.has_value(), "RenderLayer needs a validated description");
					if (!note.has_value())
						continue;
					const uint64_t frames = SecondsToFrames(note->Duration);
					if (!note->Rest)
						tones.push_back(SoundToneSpan{ .Start = toneFrames, .Frames = frames, .StartFrequency = note->Frequency, .EndFrequency = note->Frequency });
					toneFrames += frames;
				}
			}

			std::vector<double> samples(static_cast<size_t>(toneFrames + envelope.Release), 0.0);
			for (const SoundToneSpan& tone : tones)
				RenderTone(layer, envelope, tone, random, samples);

			if (layer.LowPass > 0.0f)
			{
				const double coefficient = 1.0 - DetMath::Exp(-2.0 * std::numbers::pi * static_cast<double>(layer.LowPass) / static_cast<double>(SoundSynthSampleRate));
				double filtered = 0.0;
				for (double& sample : samples)
				{
					filtered += coefficient * (sample - filtered);
					sample = filtered;
				}
			}

			const size_t delay = static_cast<size_t>(SecondsToFrames(layer.Delay));
			const double volume = static_cast<double>(layer.Volume);
			for (size_t frame = 0; frame < samples.size(); ++frame)
				mix[delay + frame] += samples[frame] * volume;
		}

		// x clamped to [-1, 1] and quantized half away from zero (SoundSynth.h).
		[[nodiscard]] static int16_t QuantizeSample(double value)
		{
			const double clamped = std::clamp(value, -1.0, 1.0);
			if (clamped >= 0.0)
				return static_cast<int16_t>(std::floor(clamped * PcmScale + 0.5));
			return static_cast<int16_t>(-std::floor(-clamped * PcmScale + 0.5));
		}

	}

	Result<SoundNote> ParseSoundNote(std::string_view note)
	{
		ENGINE_TRY_ASSIGN(const ParsedSoundNote parsed, Utils::ParseNote(note));
		return SoundNote{
			.Rest = parsed.Rest,
			.Frequency = static_cast<float>(parsed.Frequency),
			.Duration = parsed.Duration,
			.Midi = parsed.Midi,
		};
	}

	Status ValidateSoundEffect(const SoundEffectDescription& description)
	{
		Utils::SoundEffectIssues issues;
		issues.CheckRange("/Volume", "Volume", description.Volume, 0.0, 1.0);
		if (description.Layers.empty())
			issues.Add("/Layers", "a sound effect needs at least one layer", "add a layer with notes or a sweep");
		else if (description.Layers.size() > MaxSoundLayers)
			issues.Add("/Layers", std::format("a sound effect has at most {} layers (got {})", MaxSoundLayers, description.Layers.size()));
		for (size_t index = 0; index < description.Layers.size(); ++index)
			Utils::ValidateLayer(description.Layers[index], std::format("/Layers/{}", index), issues);
		return issues.ToStatus();
	}

	uint64_t ComputeSoundEffectFrameCount(const SoundEffectDescription& description)
	{
		uint64_t frames = 0;
		for (const SoundLayer& layer : description.Layers)
			frames = std::max(frames, Utils::GetLayerSpan(layer));
		return frames;
	}

	double ComputeSoundEffectDuration(const SoundEffectDescription& description)
	{
		return static_cast<double>(ComputeSoundEffectFrameCount(description)) / SoundSynthSampleRate;
	}

	Result<std::vector<int16_t>> SynthesizeSoundEffect(const SoundEffectDescription& description)
	{
		ENGINE_TRY(ValidateSoundEffect(description));
		const auto frameCount = static_cast<size_t>(ComputeSoundEffectFrameCount(description));
		std::vector<double> mix(frameCount, 0.0);
		for (size_t index = 0; index < description.Layers.size(); ++index)
			Utils::RenderLayer(description, index, mix);

		const double volume = static_cast<double>(description.Volume);
		std::vector<int16_t> samples(frameCount);
		for (size_t frame = 0; frame < frameCount; ++frame)
			samples[frame] = Utils::QuantizeSample(mix[frame] * volume);
		return samples;
	}

	std::string_view SoundWaveToString(SoundWave wave)
	{
		switch (wave)
		{
			case SoundWave::Sine:     return "Sine";
			case SoundWave::Square:   return "Square";
			case SoundWave::Triangle: return "Triangle";
			case SoundWave::Saw:      return "Saw";
			case SoundWave::Noise:    return "Noise";
		}

		ENGINE_CORE_ASSERT(false, "Unknown SoundWave {}", std::to_underlying(wave));
		return "Unknown";
	}

}
