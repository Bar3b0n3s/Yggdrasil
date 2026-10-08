#include "TestsPCH.h"

#include "Engine/Audio/SoundSynth.h"

#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Procedural sound effects (Architecture §6.6, §10.3, §10.4; Docs/Decisions/0015-m12-decisions.md decision 11): the note
// grammar, validation, whole-frame timing, the synthesis rules and the committed hashes of the ten presets
// (Resources/Audio/*.sfx).

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

		// A layer whose wave is +1 for most of its period: a 20 Hz square with a 0.99 duty cycle is high for the first 2,376
		// frames of every 2,400, so each sample there is the envelope level times the volumes.
		SoundLayer MakeEnvelopeProbe()
		{
			SoundLayer layer;
			layer.Wave = SoundWave::Square;
			layer.DutyCycle = 0.99f;
			layer.StartFrequency = 20.0f;
			layer.EndFrequency = 20.0f;
			layer.Volume = 1.0f;
			return layer;
		}

		// The PCM sample of a level in [0, 1] (SoundSynth.h's quantization).
		int16_t ToPcm(double level)
		{
			return static_cast<int16_t>(std::floor(level * 32767.0 + 0.5));
		}

		// How often consecutive samples change sign (zeros keep the previous sign).
		size_t CountSignChanges(std::span<const int16_t> samples)
		{
			size_t changes = 0;
			int previous = 0;
			for (const int16_t sample : samples)
			{
				const int sign = sample > 0 ? 1 : (sample < 0 ? -1 : 0);
				if (sign != 0 && previous != 0 && sign != previous)
					++changes;
				if (sign != 0)
					previous = sign;
			}
			return changes;
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

		TEST_CASE("SoundSynth: presets hash to committed values")
		{
			// The XXH64 of each preset's PCM. They hold in every configuration and on every platform (SoundSynth.h,
			// "Determinism"); a change of the synthesizer or a preset that alters them is reviewed and recommitted with the
			// reason.
			constexpr std::array<PresetHash, 10> Committed = { {
				{ "Click", 10343468374008523111ull },
				{ "Blip", 3371592557532933484ull },
				{ "Coin", 940577468926086399ull },
				{ "Jump", 8777064044265027176ull },
				{ "Hit", 4787386059123077636ull },
				{ "Explosion", 17045286280218857767ull },
				{ "PowerUp", 10052824013858761676ull },
				{ "LineClear", 12266015460515085166ull },
				{ "Win", 12743535354959533975ull },
				{ "Lose", 7720099442096561452ull },
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

		TEST_CASE("SoundSynth: synthesis is bit-identical across runs")
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

		TEST_CASE("SoundSynth: notes parse to equal-temperament frequencies")
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

		TEST_CASE("SoundSynth: invalid notes are Validation errors naming the note")
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

		TEST_CASE("SoundSynth: validation locates every violation by its JSON pointer")
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

		TEST_CASE("SoundSynth: decimal durations add up to whole frames")
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

		TEST_CASE("SoundSynth: the length is the notes plus the release, and the sample count follows it")
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

		TEST_CASE("SoundSynth: a sine layer's peak is its volume")
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

		TEST_CASE("SoundSynth: noise layers depend on the seed and nothing else")
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

		TEST_CASE("SoundSynth: the low-pass attenuates high frequencies")
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

		TEST_CASE("SoundSynth: the envelope is linear in whole frames and a release starts from the level reached")
		{
			// A tone that ends during its attack (480 of 960 attack frames) releases from 0.5 over 240 frames.
			SoundEffectDescription description;
			SoundLayer probe = MakeEnvelopeProbe();
			probe.Duration = 0.01f;
			probe.Envelope = { .Attack = 0.02f, .Decay = 0.0f, .Sustain = 1.0f, .Release = 0.005f };
			description.Layers = { probe };
			const Result<std::vector<int16_t>> attack = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(attack.has_value(), attack.error().ToString());
			REQUIRE(attack->size() == 720);
			CHECK((*attack)[0] == 0);
			CHECK((*attack)[240] == ToPcm(240.0 / 960.0));
			CHECK((*attack)[479] == ToPcm(479.0 / 960.0));
			CHECK((*attack)[480] == ToPcm(0.5));
			CHECK((*attack)[600] == ToPcm(0.5 * (1.0 - 120.0 / 240.0)));
			CHECK((*attack)[719] == ToPcm(0.5 * (1.0 - 239.0 / 240.0)));

			// A zero-length attack starts at full level; the decay falls to the sustain level, which holds to the end.
			description.Layers[0].Duration = 0.1f;
			description.Layers[0].Envelope = { .Attack = 0.0f, .Decay = 0.05f, .Sustain = 0.5f, .Release = 0.0f };
			const Result<std::vector<int16_t>> decay = SynthesizeSoundEffect(description);
			REQUIRE(decay.has_value());
			REQUIRE(decay->size() == 4800);
			CHECK((*decay)[0] == 32767);
			CHECK((*decay)[1200] == ToPcm(1.0 - 0.5 * 1200.0 / 2400.0));
			CHECK((*decay)[2300] == ToPcm(1.0 - 0.5 * 2300.0 / 2400.0));
			CHECK((*decay)[3000] == ToPcm(0.5));
			CHECK((*decay)[4700] == ToPcm(0.5));
		}

		TEST_CASE("SoundSynth: release tails overlap the next note and sum, and rests are silent")
		{
			// C0 (16.35 Hz) with a 0.99 duty cycle is +1 for its first 2,900 frames, so the samples show the envelope.
			SoundEffectDescription description;
			SoundLayer layer = MakeEnvelopeProbe();
			layer.Notes = { "C0:0.01", "R:0.01", "C0:0.01" };
			layer.Envelope = { .Attack = 0.0f, .Decay = 0.0f, .Sustain = 1.0f, .Release = 0.0f };
			description.Layers = { layer };
			const Result<std::vector<int16_t>> rest = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(rest.has_value(), rest.error().ToString());
			REQUIRE(rest->size() == 1440);
			CHECK((*rest)[0] == 32767);
			CHECK((*rest)[479] == 32767);
			CHECK(std::all_of(rest->begin() + 480, rest->begin() + 960, [](int16_t sample)
			{
				return sample == 0;
			}));
			CHECK((*rest)[960] == 32767);

			// The first note's 240-frame release runs under the second note.
			description.Layers[0].Notes = { "C0:0.01", "C0:0.01" };
			description.Layers[0].Envelope.Release = 0.005f;
			description.Layers[0].Volume = 0.25f;
			const Result<std::vector<int16_t>> overlap = SynthesizeSoundEffect(description);
			REQUIRE(overlap.has_value());
			REQUIRE(overlap->size() == 1200);
			CHECK((*overlap)[400] == ToPcm(0.25));
			CHECK((*overlap)[480] == ToPcm((1.0 + 1.0) * 0.25));
			CHECK((*overlap)[600] == ToPcm((1.0 + (1.0 - 120.0 / 240.0)) * 0.25));
			CHECK((*overlap)[800] == ToPcm(0.25));
			CHECK((*overlap)[1080] == ToPcm((1.0 - 120.0 / 240.0) * 0.25));
		}

		TEST_CASE("SoundSynth: layers start at their delay and sum, and the sum is clamped")
		{
			SoundEffectDescription description;
			SoundLayer first = MakeEnvelopeProbe();
			first.Duration = 0.01f;
			first.Volume = 0.75f;
			first.Envelope = { .Attack = 0.0f, .Decay = 0.0f, .Sustain = 1.0f, .Release = 0.0f };
			SoundLayer second = first;
			second.Delay = 0.005f;
			description.Layers = { first, second };
			CHECK(ComputeSoundEffectFrameCount(description) == 720);
			const Result<std::vector<int16_t>> samples = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(samples.has_value(), samples.error().ToString());
			REQUIRE(samples->size() == 720);
			CHECK((*samples)[100] == ToPcm(0.75));
			CHECK((*samples)[300] == 32767); // 1.5 clamped
			CHECK((*samples)[600] == ToPcm(0.75));

			// The sound's Volume scales the sum before the clamp; full-scale negative samples are -32767, never -32768.
			description.Volume = 0.5f;
			const Result<std::vector<int16_t>> halved = SynthesizeSoundEffect(description);
			REQUIRE(halved.has_value());
			CHECK((*halved)[300] == ToPcm(0.75));
			description.Volume = 1.0f;
			description.Layers[0].Wave = SoundWave::Saw;
			description.Layers[1].Wave = SoundWave::Saw;
			const Result<std::vector<int16_t>> saw = SynthesizeSoundEffect(description);
			REQUIRE(saw.has_value());
			CHECK(*std::ranges::min_element(*saw) == -32767);
		}

		TEST_CASE("SoundSynth: a sweep glides exponentially from its start to its end frequency")
		{
			SoundEffectDescription description;
			SoundLayer layer;
			layer.Wave = SoundWave::Square;
			layer.Duration = 1.0f;
			layer.StartFrequency = 200.0f;
			layer.EndFrequency = 2000.0f;
			layer.Envelope = { .Attack = 0.0f, .Decay = 0.0f, .Sustain = 1.0f, .Release = 0.0f };
			description.Layers = { layer };
			const Result<std::vector<int16_t>> rising = SynthesizeSoundEffect(description);
			REQUIRE_MESSAGE(rising.has_value(), rising.error().ToString());
			REQUIRE(rising->size() == 48000);
			// Over the first and the last quarter the frequency averages about 270 Hz and 1,500 Hz (two sign changes per
			// period).
			const size_t early = CountSignChanges(std::span<const int16_t>(*rising).subspan(0, 12000));
			const size_t late = CountSignChanges(std::span<const int16_t>(*rising).subspan(36000, 12000));
			CHECK(early == doctest::Approx(2.0 * 0.25 * 270.0).epsilon(0.05));
			CHECK(late == doctest::Approx(2.0 * 0.25 * 1500.0).epsilon(0.05));

			description.Layers[0].StartFrequency = 2000.0f;
			description.Layers[0].EndFrequency = 200.0f;
			const Result<std::vector<int16_t>> falling = SynthesizeSoundEffect(description);
			REQUIRE(falling.has_value());
			CHECK(CountSignChanges(std::span<const int16_t>(*falling).subspan(0, 12000)) > CountSignChanges(std::span<const int16_t>(*falling).subspan(36000, 12000)) * 4);
		}

		TEST_CASE("SoundSynth: note durations are plain decimals rounded to the nearest float")
		{
			const Result<SoundNote> decimal = ParseSoundNote("C5:0.06");
			REQUIRE(decimal.has_value());
			CHECK(decimal->Duration == 0.06f);
			const Result<SoundNote> whole = ParseSoundNote("Bb4:2");
			REQUIRE(whole.has_value());
			CHECK(whole->Midi == 70);
			CHECK(whole->Duration == 2.0f);
			const Result<SoundNote> lowest = ParseSoundNote("C0:0.00000001");
			REQUIRE(lowest.has_value());
			CHECK(lowest->Midi == 12);
			CHECK(lowest->Duration == 1e-8f);
			const Result<SoundNote> highest = ParseSoundNote("B8:1.5");
			REQUIRE(highest.has_value());
			CHECK(highest->Midi == 119);
			const Result<SoundNote> sharp = ParseSoundNote("C#4:0.1");
			const Result<SoundNote> flat = ParseSoundNote("Db4:0.1");
			REQUIRE(sharp.has_value());
			REQUIRE(flat.has_value());
			CHECK(sharp->Midi == 61);
			CHECK(sharp->Frequency == flat->Frequency);
			for (const std::string_view note : { "C4:0.123456789", "C4:.5", "C4:5.", "C4:12345678", "C4: 1", "Cb4:0.1", "E#4:0.1", "C10:0.1", "C4:1:2", "R4:0.1" })
			{
				CAPTURE(std::string(note));
				const Result<SoundNote> parsed = ParseSoundNote(note);
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::Validation);
				CHECK(parsed.error().ToString().contains(std::string(note)));
			}
		}

		TEST_CASE("SoundSynth: every layer lasts at least one frame and the sound lasts as long as its longest layer")
		{
			SoundEffectDescription description;
			SoundLayer instant;
			instant.Duration = 0.00001f; // 0.48 frames
			instant.Envelope.Release = 0.0f;
			description.Layers = { instant };
			const Status status = ValidateSoundEffect(description);
			REQUIRE_FALSE(status.has_value());
			REQUIRE(status.error().GetIssues().size() == 1);
			CHECK(status.error().GetIssues().front().JsonPointer == "/Layers/0");

			// A field error is reported alone: the length rules wait for valid fields.
			description.Layers[0].Duration = 0.0f;
			const Status invalid = ValidateSoundEffect(description);
			REQUIRE_FALSE(invalid.has_value());
			REQUIRE(invalid.error().GetIssues().size() == 1);
			CHECK(invalid.error().GetIssues().front().JsonPointer == "/Layers/0/Duration");

			SoundLayer shortLayer;
			shortLayer.Duration = 0.05f;
			SoundLayer longLayer;
			longLayer.Notes = { "C4:0.1", "D4:0.1" };
			longLayer.Delay = 0.1f;
			description.Layers = { shortLayer, longLayer };
			// The long layer: 4,800 + 9,600 + 480.
			CHECK(ComputeSoundEffectFrameCount(description) == 14880);
			CHECK(ComputeSoundEffectDuration(description) == 0.31);
		}
	}

}
