#include "TestsPCH.h"

#include "Engine/Audio/SoundSynth.h"

#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Procedural sound effects (Architecture §6.6, §10.3, §10.4). Skipped skeletons of the M12 contract
// (Docs/Decisions/0015-m12-decisions.md): stream B implements the synthesizer and the ten presets (Resources/Audio/*.sfx),
// commits the presets' hashes below and removes the skips.

namespace Engine {

	namespace {

		// The error code of a failed result; nullopt for a success.
		template<typename T>
		std::optional<ErrorCode> GetErrorCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// §6.6's LineClear example: a square arpeggio and a noise sweep.
		SoundEffectDescription MakeLineClear()
		{
			SoundEffectDescription description;
			description.Seed = 7;
			description.Volume = 0.8f;
			SoundLayer notes;
			notes.Wave = SoundWave::Square;
			notes.DutyCycle = 0.5f;
			notes.Notes = { "C5:0.06", "E5:0.06", "G5:0.06", "C6:0.18" };
			notes.Envelope = { .Attack = 0.005f, .Decay = 0.05f, .Sustain = 0.6f, .Release = 0.08f };
			SoundLayer noise;
			noise.Wave = SoundWave::Noise;
			noise.Duration = 0.05f;
			noise.StartFrequency = 4000.0f;
			noise.EndFrequency = 800.0f;
			noise.Volume = 0.3f;
			noise.Envelope = { .Attack = 0.0f, .Decay = 0.05f, .Sustain = 0.0f, .Release = 0.0f };
			description.Layers = { notes, noise };
			return description;
		}

		// XXH64 (seed 0) of the samples' little-endian bytes.
		uint64_t HashSamples(std::span<const int16_t> samples)
		{
			return XXH64(std::as_bytes(samples));
		}

		// A committed preset hash (SoundSynth: presets hash to committed values).
		struct PresetHash
		{
			std::string_view Name{};
			uint64_t Hash = 0;
		};

	}

	TEST_SUITE("Audio")
	{
		TEST_CASE("SoundSynth: the waves name themselves")
		{
			CHECK(SoundWaveToString(SoundWave::Sine) == "Sine");
			CHECK(SoundWaveToString(SoundWave::Square) == "Square");
			CHECK(SoundWaveToString(SoundWave::Triangle) == "Triangle");
			CHECK(SoundWaveToString(SoundWave::Saw) == "Saw");
			CHECK(SoundWaveToString(SoundWave::Noise) == "Noise");
		}

		TEST_CASE("SoundSynth: presets hash to committed values" * doctest::skip(true))
		{
			// The XXH64 of each preset's PCM. They hold in every configuration and on every platform (SoundSynth.h,
			// "Determinism"); a change of the synthesizer or a preset that alters them is reviewed and recommitted with the
			// reason. Stream B commits the values with its implementation.
			constexpr std::array<PresetHash, 10> Committed = { {
				{ "Click", 0 },
				{ "Blip", 0 },
				{ "Coin", 0 },
				{ "Jump", 0 },
				{ "Hit", 0 },
				{ "Explosion", 0 },
				{ "PowerUp", 0 },
				{ "LineClear", 0 },
				{ "Win", 0 },
				{ "Lose", 0 },
			} };
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			for (const PresetHash& preset : Committed)
			{
				CAPTURE(std::string(preset.Name));
				const Result<std::string> text = FileSystem::ReadText(Test::GetRepositoryRoot() / "Resources" / "Audio" / (std::string(preset.Name) + ".sfx"));
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				SoundEffectLoadReport report;
				const Result<SoundEffectDescription> description = SoundEffectFromText(*text, *registry, report, true);
				REQUIRE_MESSAGE(description.has_value(), description.error().ToString());
				const Result<std::vector<int16_t>> samples = SynthesizeSoundEffect(*description);
				REQUIRE_MESSAGE(samples.has_value(), samples.error().ToString());
				CHECK_FALSE(samples->empty());
				CHECK(HashSamples(*samples) == preset.Hash);
			}
		}

		TEST_CASE("SoundSynth: synthesis is bit-identical across runs" * doctest::skip(true))
		{
			const SoundEffectDescription description = MakeLineClear();
			const Result<std::vector<int16_t>> first = SynthesizeSoundEffect(description);
			const Result<std::vector<int16_t>> second = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			REQUIRE(second.has_value());
			CHECK(*first == *second);
			CHECK(std::ranges::any_of(*first, [](int16_t sample)
			{
				return sample != 0;
			}));
		}

		TEST_CASE("SoundSynth: notes parse to equal-temperament frequencies" * doctest::skip(true))
		{
			const Result<SoundNote> a4 = ParseSoundNote("A4:0.5");
			REQUIRE_MESSAGE(a4.has_value(), a4.error().ToString());
			CHECK_FALSE(a4->Rest);
			CHECK(a4->Midi == 69);
			CHECK(a4->Frequency == doctest::Approx(440.0f));
			CHECK(a4->Duration == doctest::Approx(0.5f));
			const Result<SoundNote> c5 = ParseSoundNote("C5:0.06");
			REQUIRE(c5.has_value());
			CHECK(c5->Midi == 72);
			CHECK(c5->Frequency == doctest::Approx(523.2511f).epsilon(0.0001));
			const Result<SoundNote> sharp = ParseSoundNote("F#3:0.25");
			const Result<SoundNote> flat = ParseSoundNote("Gb3:0.25");
			REQUIRE(sharp.has_value());
			REQUIRE(flat.has_value());
			CHECK(sharp->Midi == flat->Midi);
			const Result<SoundNote> rest = ParseSoundNote("R:0.1");
			REQUIRE(rest.has_value());
			CHECK(rest->Rest);
			CHECK(rest->Midi == -1);
			CHECK(rest->Frequency == 0.0f);
		}

		TEST_CASE("SoundSynth: invalid notes are Validation errors naming the note" * doctest::skip(true))
		{
			for (const std::string_view note : { "H4:0.1", "C9:0.1", "C:0.1", "C4", "C4:0", "C4:-1", "C4:abc", "C4:1e3", "", "R", "c4:0.1" })
			{
				CAPTURE(std::string(note));
				const Result<SoundNote> parsed = ParseSoundNote(note);
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::Validation);
				CHECK(parsed.error().ToString().contains(std::string(note)));
			}
		}

		TEST_CASE("SoundSynth: validation locates every violation by its JSON pointer" * doctest::skip(true))
		{
			CHECK(ValidateSoundEffect(MakeLineClear()).has_value());
			CHECK(GetErrorCode(ValidateSoundEffect(SoundEffectDescription{})) == ErrorCode::Validation);

			SoundEffectDescription invalid = MakeLineClear();
			invalid.Volume = 2.0f;
			invalid.Layers[0].Notes[2] = "X5:0.06";
			invalid.Layers[0].Envelope.Sustain = std::numeric_limits<float>::quiet_NaN();
			invalid.Layers[1].Notes.clear();
			invalid.Layers[1].Duration = 0.0f;
			invalid.Layers[1].LowPass = 5.0f;
			const Status status = ValidateSoundEffect(invalid);
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			const std::string message = status.error().ToString();
			for (const std::string_view pointer : { "/Volume", "/Layers/0/Notes/2", "/Layers/0/Envelope/Sustain", "/Layers/1/Duration", "/Layers/1/LowPass" })
			{
				CAPTURE(std::string(pointer));
				CHECK(message.contains(std::string(pointer)));
			}

			// Too long and too many.
			SoundEffectDescription tooLong;
			SoundLayer layer;
			layer.Duration = 9.0f;
			layer.Delay = 2.0f;
			tooLong.Layers = { layer };
			CHECK(GetErrorCode(ValidateSoundEffect(tooLong)) == ErrorCode::Validation);
			SoundEffectDescription tooMany;
			tooMany.Layers.resize(MaxSoundLayers + 1);
			CHECK(GetErrorCode(ValidateSoundEffect(tooMany)) == ErrorCode::Validation);
		}

		TEST_CASE("SoundSynth: decimal durations add up to whole frames" * doctest::skip(true))
		{
			// The M12 acceptance sound (test_audio.py): "A3:0.05" and "E4:0.1" with the default 10 ms release. Summed in seconds
			// the floats land just above 0.16 s, and a ceiling would give 7,681 frames; each converted to frames once they are
			// 2,400 + 4,800 + 480.
			SoundEffectDescription description;
			SoundLayer layer;
			layer.Notes = { "A3:0.05", "E4:0.1" };
			description.Layers = { layer };
			CHECK(ComputeSoundEffectFrameCount(description) == 7680);
			const Result<std::vector<int16_t>> samples = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(samples.has_value(), samples.error().ToString());
			CHECK(samples->size() == 7680);

			// The longest sound is exactly MaxSoundEffectFrames; one frame more is refused.
			SoundEffectDescription longest;
			SoundLayer tone;
			tone.Duration = 9.99f;
			tone.Envelope.Release = 0.01f;
			longest.Layers = { tone };
			CHECK(ComputeSoundEffectFrameCount(longest) == MaxSoundEffectFrames);
			CHECK(ValidateSoundEffect(longest).has_value());
			longest.Layers[0].Delay = 1.0f / 48000.0f;
			CHECK(GetErrorCode(ValidateSoundEffect(longest)) == ErrorCode::Validation);
		}

		TEST_CASE("SoundSynth: the length is the notes plus the release, and the sample count follows it" * doctest::skip(true))
		{
			SoundEffectDescription description;
			SoundLayer layer;
			layer.Notes = { "C5:0.1", "R:0.05", "E5:0.1" };
			layer.Envelope.Release = 0.02f;
			layer.Delay = 0.03f;
			description.Layers = { layer };
			// Whole frames, each duration converted once: delay 1,440, notes 4,800 + 2,400 + 4,800, release 960.
			const uint64_t frames = 1440 + 4800 + 2400 + 4800 + 960;
			CHECK(ComputeSoundEffectFrameCount(description) == frames);
			CHECK(ComputeSoundEffectDuration(description) == static_cast<double>(frames) / SoundSynthSampleRate);
			const Result<std::vector<int16_t>> samples = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(samples.has_value(), samples.error().ToString());
			CHECK(samples->size() == frames);
			// The delay is silent.
			CHECK(std::all_of(samples->begin(), samples->begin() + 1440, [](int16_t sample)
			{
				return sample == 0;
			}));
		}

		TEST_CASE("SoundSynth: a sine layer's peak is its volume" * doctest::skip(true))
		{
			SoundEffectDescription description;
			SoundLayer layer;
			layer.Wave = SoundWave::Sine;
			layer.Duration = 0.5f;
			layer.StartFrequency = 1000.0f;
			layer.EndFrequency = 1000.0f;
			layer.Volume = 0.5f;
			layer.Envelope = { .Attack = 0.0f, .Decay = 0.0f, .Sustain = 1.0f, .Release = 0.0f };
			description.Layers = { layer };
			const Result<std::vector<int16_t>> samples = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(samples.has_value(), samples.error().ToString());
			const int16_t peak = *std::ranges::max_element(*samples, {}, [](int16_t sample)
			{
				return std::abs(sample);
			});
			CHECK(std::abs(peak) == doctest::Approx(16384).epsilon(0.001));
		}

		TEST_CASE("SoundSynth: noise layers depend on the seed and nothing else" * doctest::skip(true))
		{
			SoundEffectDescription description;
			SoundLayer layer;
			layer.Wave = SoundWave::Noise;
			layer.Duration = 0.2f;
			layer.StartFrequency = 8000.0f;
			layer.EndFrequency = 8000.0f;
			description.Layers = { layer };
			description.Seed = 1;
			const Result<std::vector<int16_t>> first = SynthesizeSoundEffect(description);
			description.Seed = 2;
			const Result<std::vector<int16_t>> second = SynthesizeSoundEffect(description);
			description.Seed = 1;
			const Result<std::vector<int16_t>> again = SynthesizeSoundEffect(description);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			REQUIRE(again.has_value());
			CHECK(*first != *second);
			CHECK(*first == *again);
		}

		TEST_CASE("SoundSynth: the low-pass attenuates high frequencies" * doctest::skip(true))
		{
			SoundEffectDescription description;
			SoundLayer layer;
			layer.Wave = SoundWave::Square;
			layer.Duration = 0.5f;
			layer.StartFrequency = 4000.0f;
			layer.EndFrequency = 4000.0f;
			description.Layers = { layer };
			const auto energy = [&description]()
			{
				const Result<std::vector<int16_t>> samples = SynthesizeSoundEffect(description);
				REQUIRE(samples.has_value());
				double sum = 0.0;
				for (const int16_t sample : *samples)
					sum += static_cast<double>(sample) * static_cast<double>(sample);
				return sum;
			};
			const double unfiltered = energy();
			description.Layers[0].LowPass = 500.0f;
			const double filtered = energy();
			CHECK(filtered < unfiltered * 0.25);
		}
	}

}
