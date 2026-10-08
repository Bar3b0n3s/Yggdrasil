#include "TestsPCH.h"

#include "Engine/Audio/AudioEngine.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AudioTestData.h"
#include "Support/ExpectLog.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The audio engine (Architecture §10.1, §10.4; Roadmap M12). Skipped skeletons of the M12 contract
// (Docs/Decisions/0015-m12-decisions.md): stream A implements the engine and removes the skips. Every test runs a
// device-less engine (AudioDeviceKind::None) or miniaudio's Null backend as a real device (AudioDeviceKind::Null), never the
// machine's audio device (§15.1 T1), with deterministic decoding.

namespace Engine {

	namespace {

		// A test's engine. It releases every live voice before the engine goes, so a test case that fails between starting a
		// voice and its end still tears the engine down cleanly (destroying an engine with live voices is a programmer error,
		// AudioEngine.h, §4.1). Not copyable or movable.
		class TestEngine
		{
		public:
			explicit TestEngine(Scope<AudioEngine> engine)
				: m_Engine(std::move(engine))
			{
			}

			~TestEngine()
			{
				for (const AudioVoiceInfo& voice : m_Engine->GetVoices())
					CHECK(m_Engine->StopVoice(voice.Voice).has_value());
			}

			TestEngine(const TestEngine&) = delete;
			TestEngine& operator=(const TestEngine&) = delete;

			[[nodiscard]] AudioEngine& operator*() const { return *m_Engine; }
			[[nodiscard]] AudioEngine* operator->() const { return m_Engine.get(); }
		private:
			Scope<AudioEngine> m_Engine;
		};

		// An engine of `specification` over `vfs`; fails the test case on error.
		TestEngine CreateTestEngine(const VirtualFileSystem& vfs, const AudioEngineSpecification& specification)
		{
			Result<Scope<AudioEngine>> engine = AudioEngine::Create(specification, vfs);
			REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
			return TestEngine(std::move(*engine));
		}

		// A device-less (or Null-backend) engine over `vfs` with deterministic decoding; fails the test case on error.
		TestEngine CreateTestEngine(const VirtualFileSystem& vfs, AudioDeviceKind device = AudioDeviceKind::None)
		{
			return CreateTestEngine(vfs, AudioEngineSpecification{ .Device = device, .Decoding = AudioDecoding::Deterministic });
		}

		// A WAV clip of `tone` registered under `name` (16 hex digits); fails the test case on error.
		AudioClipHandle RegisterToneClip(AudioEngine& engine, std::string name, const Test::TestToneSpecification& tone,
			bool stream = false)
		{
			const AudioClipSource source{
				.Name = std::move(name),
				.Version = 1,
				.Format = AudioClipFormat::Encoded,
				.Bytes = CreateRef<const Buffer>(Test::MakeToneWav(tone)),
				.SampleRate = tone.SampleRate,
				.ChannelCount = tone.ChannelCount,
				.Stream = stream,
			};
			Result<AudioClipHandle> clip = engine.RegisterClip(source);
			REQUIRE_MESSAGE(clip.has_value(), clip.error().ToString());
			return *clip;
		}

		// Starts a voice; fails the test case on error.
		AudioVoiceHandle PlayTestVoice(AudioEngine& engine, const AudioVoiceDescription& description)
		{
			Result<AudioVoiceHandle> voice = engine.PlayVoice(description);
			REQUIRE_MESSAGE(voice.has_value(), voice.error().ToString());
			return *voice;
		}

		// Pulls `frames` frames from a device-less engine; fails the test case on error.
		std::vector<float> PullFrames(AudioEngine& engine, uint64_t frames)
		{
			std::vector<float> samples(static_cast<size_t>(frames) * AudioChannelCount);
			const Status read = engine.ReadFrames(samples);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			return samples;
		}

		// The levels of `frames` frames pulled from a device-less engine; fails the test case on error.
		AudioLevels PullLevels(AudioEngine& engine, uint64_t frames)
		{
			const std::vector<float> samples = PullFrames(engine, frames);
			return MeasureAudioLevels(samples);
		}

		// The snapshot of a live voice; fails the test case for a stale one.
		AudioVoiceInfo GetInfo(const AudioEngine& engine, AudioVoiceHandle voice)
		{
			Result<AudioVoiceInfo> info = engine.GetVoiceInfo(voice);
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			return std::move(*info);
		}

		// The cursor of a live voice; fails the test case for a stale one.
		uint64_t GetCursor(const AudioEngine& engine, AudioVoiceHandle voice)
		{
			return GetInfo(engine, voice).CursorFrames;
		}

		// The error code of a failed result; nullopt for a success, so a CHECK against a code fails instead of reading an
		// error that is not there.
		template<typename T>
		std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// ReadFrames into a scratch buffer of `samples` samples.
		Status ReadScratch(AudioEngine& engine, size_t samples)
		{
			std::vector<float> scratch(samples);
			return engine.ReadFrames(scratch);
		}

		// Stops every live voice (a test that starts over within one engine).
		void StopAllVoices(AudioEngine& engine)
		{
			for (const AudioVoiceInfo& voice : engine.GetVoices())
				CHECK(engine.StopVoice(voice.Voice).has_value());
		}

		// One second of a 440 Hz tone at half scale, 48 kHz mono, unless told otherwise.
		Test::TestToneSpecification MakeTone(double seconds = 1.0, uint32_t channels = 1, double amplitude = 0.5)
		{
			return Test::TestToneSpecification{ .SampleRate = 48000,
				.ChannelCount = channels,
				.FrameCount = static_cast<uint64_t>(seconds * 48000.0),
				.Frequency = 440.0,
				.Amplitude = amplitude };
		}

		// A non-spatial-attenuation spatial voice of `clip` at `position` (only the panning changes with the position).
		AudioVoiceDescription MakePannedVoice(AudioClipHandle clip, const glm::vec3& position)
		{
			return AudioVoiceDescription{ .Clip = clip,
				.Settings = { .Spatial = true, .Spatialization = { .Model = Attenuation::None } },
				.Transform = { .Position = position } };
		}

		// The left plus right RMS of `frames` frames pulled while only `description` plays.
		float MeasureVoiceRms(const AudioEngineSpecification& specification, const AudioVoiceDescription& description, uint64_t frames)
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, specification);
			AudioVoiceDescription voice = description;
			voice.Clip = RegisterToneClip(*engine, "00000000000000a1", MakeTone());
			PlayTestVoice(*engine, voice);
			const AudioLevels levels = PullLevels(*engine, frames);
			return levels.RmsLeft + levels.RmsRight;
		}

	}

	TEST_SUITE("Audio")
	{
		TEST_CASE("AudioEngine: the enumerations name themselves")
		{
			CHECK(AudioDeviceKindToString(AudioDeviceKind::None) == "None");
			CHECK(AudioDeviceKindToString(AudioDeviceKind::System) == "System");
			CHECK(AudioDeviceKindToString(AudioDeviceKind::Null) == "Null");
			CHECK(AudioDecodingToString(AudioDecoding::Deterministic) == "Deterministic");
			CHECK(AudioDecodingToString(AudioDecoding::Threaded) == "Threaded");
			CHECK(AudioDeviceStateToString(AudioDeviceState::None) == "None");
			CHECK(AudioDeviceStateToString(AudioDeviceState::Running) == "Running");
			CHECK(AudioDeviceStateToString(AudioDeviceState::Recreating) == "Recreating");
			CHECK(AudioDeviceStateToString(AudioDeviceState::Failed) == "Failed");
			CHECK(AudioTimeSourceToString(AudioTimeSource::Device) == "Device");
			CHECK(AudioTimeSourceToString(AudioTimeSource::Simulation) == "Simulation");
			CHECK(AudioTimeSourceToString(AudioTimeSource::Host) == "Host");
			CHECK(AudioDeviceNotificationToString(AudioDeviceNotification::Stopped) == "Stopped");
			CHECK(AudioDeviceNotificationToString(AudioDeviceNotification::Rerouted) == "Rerouted");
			CHECK(AudioDeviceNotificationToString(AudioDeviceNotification::InterruptionEnded) == "InterruptionEnded");
		}

		TEST_CASE("AudioEngine: a clip's resource name is its name and its version")
		{
			CHECK(MakeAudioResourceName("00000000000000a1", 1) == "00000000000000a1@1");
			CHECK(MakeAudioResourceName("00000000000000a1", 2) == "00000000000000a1@2");
			const uint64_t largest = std::numeric_limits<uint64_t>::max();
			CHECK(MakeAudioResourceName("0000000000000001", largest) == "0000000000000001@18446744073709551615");
		}

		TEST_CASE("AudioEngine: a device-less engine starts with no device, host time and no voices" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			CHECK(engine->GetDeviceState() == AudioDeviceState::None);
			CHECK(engine->GetTimeSource() == AudioTimeSource::Host);
			CHECK_FALSE(engine->IsSimulationTimeOwned());
			const AudioEngineStats stats = engine->GetStats();
			CHECK(stats.DeviceKind == AudioDeviceKind::None);
			CHECK(stats.Decoding == AudioDecoding::Deterministic);
			CHECK(stats.DeviceName.empty());
			CHECK(stats.LiveVoices == 0);
			CHECK(stats.VoiceCapacity == MaxAudioVoices);
			CHECK(engine->GetVoices().empty());
			CHECK(engine->GetMasterVolume() == 1.0f);
			CHECK(engine->GetGroupVolume(AudioGroup::Music) == 1.0f);
			CHECK(engine->GetListener() == AudioListenerPose{});
		}

		TEST_CASE("AudioEngine: a playing clip has non-zero RMS" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip });
			const AudioLevels levels = PullLevels(*engine, 4800);
			CHECK(levels.RmsLeft > 0.01f);
			CHECK(levels.RmsRight > 0.01f);
			CHECK(levels.Peak > 0.1f);
			CHECK(levels.Peak <= 1.0f);
			const uint64_t cursor = GetCursor(*engine, voice);
			CHECK(cursor == 4800);
		}

		TEST_CASE("AudioEngine: volume 0 is silent" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Volume = 0.0f } });
			const AudioLevels silent = PullLevels(*engine, 4800);
			CHECK(silent.Peak == 0.0f);
			// A silent voice still advances.
			const uint64_t cursor = GetCursor(*engine, voice);
			CHECK(cursor == 4800);

			// The same through a group and through the master volume.
			REQUIRE(engine->SetVoiceSettings(voice, { .Volume = 1.0f }).has_value());
			REQUIRE(engine->SetGroupVolume(AudioGroup::Sfx, 0.0f).has_value());
			const AudioLevels silentGroup = PullLevels(*engine, 4800);
			CHECK(silentGroup.Peak == 0.0f);
			REQUIRE(engine->SetGroupVolume(AudioGroup::Sfx, 1.0f).has_value());
			REQUIRE(engine->SetMasterVolume(0.0f).has_value());
			const AudioLevels silentMaster = PullLevels(*engine, 4800);
			CHECK(silentMaster.Peak == 0.0f);
		}

		TEST_CASE("AudioEngine: a source panned hard right has right RMS > left" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			// The default listener hears from the origin looking down -Z, so +X is its right.
			REQUIRE(engine->SetListener(AudioListenerPose{}).has_value());
			PlayTestVoice(*engine, MakePannedVoice(clip, glm::vec3(10.0f, 0.0f, 0.0f)));
			const AudioLevels right = PullLevels(*engine, 4800);
			CHECK(right.RmsRight > right.RmsLeft);
			StopAllVoices(*engine);

			PlayTestVoice(*engine, MakePannedVoice(clip, glm::vec3(-10.0f, 0.0f, 0.0f)));
			const AudioLevels left = PullLevels(*engine, 4800);
			CHECK(left.RmsLeft > left.RmsRight);
		}

		TEST_CASE("AudioEngine: attenuation is monotonic with distance" * doctest::skip(true))
		{
			const AudioEngineSpecification specification{ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic };
			for (const Attenuation model : { Attenuation::Inverse, Attenuation::Linear, Attenuation::Exponential })
			{
				CAPTURE(std::string(AttenuationToString(model)));
				float previous = 0.0f;
				bool first = true;
				for (const float distance : { 2.0f, 4.0f, 8.0f, 16.0f, 32.0f })
				{
					CAPTURE(distance);
					// In front of the listener, so panning does not change between distances.
					const float rms = MeasureVoiceRms(specification,
						{ .Settings = { .Spatial = true, .Spatialization = { .Model = model, .MinDistance = 1.0f, .MaxDistance = 64.0f } },
							.Transform = { .Position = glm::vec3(0.0f, 0.0f, -distance) } },
						4800);
					CHECK(rms > 0.0f);
					if (!first)
						CHECK(rms < previous);
					previous = rms;
					first = false;
				}
			}
		}

		TEST_CASE("AudioEngine: streamed and decoded playback of the same clip are sample-identical" * doctest::skip(true))
		{
			// Three seconds, so the stream crosses its first page; deterministic decoding processes the stream's jobs inline.
			const Test::TestToneSpecification tone = MakeTone(3.0, 2);
			VirtualFileSystem vfs;
			std::vector<float> decoded;
			std::vector<float> streamed;
			{
				const TestEngine engine = CreateTestEngine(vfs);
				const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", tone, false);
				const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip });
				const bool decodedStreamed = GetInfo(*engine, voice).Streamed;
				CHECK_FALSE(decodedStreamed);
				decoded = PullFrames(*engine, 48000 * 2 + 1000);
			}
			{
				const TestEngine engine = CreateTestEngine(vfs);
				const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", tone, true);
				const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip });
				const bool isStreamed = GetInfo(*engine, voice).Streamed;
				CHECK(isStreamed);
				streamed = PullFrames(*engine, 48000 * 2 + 1000);
			}
			CHECK(MeasureAudioLevels(decoded).Peak > 0.1f);
			CHECK(streamed == decoded);
		}

		TEST_CASE("AudioEngine: streamed playback captured twice is bit-identical" * doctest::skip(true))
		{
			const Test::TestToneSpecification tone = MakeTone(12.0, 2);
			const auto captureOnce = [&tone]()
			{
				VirtualFileSystem vfs;
				const TestEngine engine = CreateTestEngine(vfs);
				const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", tone, true);
				engine->StartCapture();
				PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Volume = 0.8f, .Pitch = 1.25f } });
				for (int frame = 0; frame < 150; ++frame)
					PullFrames(*engine, 800);
				return engine->StopCapture();
			};
			const AudioCapture first = captureOnce();
			const AudioCapture second = captureOnce();
			CHECK(first.GetFrameCount() == 150 * 800);
			CHECK_FALSE(first.Truncated);
			CHECK(MeasureAudioLevels(first.Samples).Peak > 0.1f);
			CHECK(first.Samples == second.Samples);
		}

		TEST_CASE("AudioEngine: a stopped Null-backend device is re-created and the voice cursor is unchanged" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			REQUIRE(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetTimeSource() == AudioTimeSource::Device);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			// Simulation time freezes the cursor between ticks while the Null device keeps running.
			engine->BeginSimulationTime(60);
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true } });
			for (int tick = 0; tick < 3; ++tick)
				CHECK(engine->AdvanceSimulationTick() == 800);
			const uint64_t cursor = GetCursor(*engine, voice);
			CHECK(cursor == 2400);

			engine->InjectDeviceNotification(AudioDeviceNotification::Stopped);
			engine->Update(10.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			engine->Update(10.5, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			engine->Update(11.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().DeviceRecreations == 1);

			// Only the device was re-created: the voice, its clip and its cursor are untouched.
			REQUIRE(engine->IsVoiceAlive(voice));
			CHECK(engine->IsClipRegistered(clip));
			const uint64_t afterRecreation = GetCursor(*engine, voice);
			CHECK(afterRecreation == cursor);
			CHECK(engine->AdvanceSimulationTick() == 800);
			const uint64_t afterTick = GetCursor(*engine, voice);
			CHECK(afterTick == cursor + 800);
			engine->EndSimulationTime();
		}

		TEST_CASE("AudioEngine: three failed re-creations leave the engine device-less with one warning" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			REQUIRE(engine->GetDeviceState() == AudioDeviceState::Running);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true } });

			// Every warning of the device handling counts: there must be exactly one, when the engine gives up.
			const Test::ExpectLog warnings(LogLevel::Warn, "");
			engine->InjectDeviceCreationFailures(AudioEngine::MaxDeviceRecreationAttempts);
			engine->InjectDeviceNotification(AudioDeviceNotification::Stopped);
			engine->Update(0.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			engine->Update(1.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			engine->Update(2.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			CHECK(warnings.GetMatchCount() == 0);
			engine->Update(3.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Failed);
			CHECK(engine->GetTimeSource() == AudioTimeSource::Host);
			CHECK(engine->GetStats().FailedDeviceCreations == AudioEngine::MaxDeviceRecreationAttempts);
			CHECK(engine->GetStats().DeviceName.empty());

			// No further attempt and no further warning; the engine keeps running device-less and the host pull advances the
			// voice by the frame's delta.
			const uint64_t cursor = GetCursor(*engine, voice);
			engine->Update(10.0, 1.0 / 60.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Failed);
			const uint64_t pulled = GetCursor(*engine, voice);
			CHECK(pulled == cursor + 800);
			CHECK(warnings.GetMatchCount() == 1);
		}

		TEST_CASE("AudioEngine: lockstep pulls exactly 800 frames per tick and the device reads nothing" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			REQUIRE(engine->GetDeviceState() == AudioDeviceState::Running);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());

			engine->BeginSimulationTime(60);
			CHECK(engine->IsSimulationTimeOwned());
			CHECK(engine->GetTimeSource() == AudioTimeSource::Simulation);
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true } });
			const AudioEngineStats before = engine->GetStats();
			for (int tick = 0; tick < 30; ++tick)
				CHECK(engine->AdvanceSimulationTick() == 800);
			const AudioEngineStats after = engine->GetStats();
			CHECK(after.DeviceReadFrames == before.DeviceReadFrames);
			CHECK(after.PulledFrames - before.PulledFrames == 30 * 800);
			const uint64_t cursor = GetCursor(*engine, voice);
			CHECK(cursor == 30 * 800);
			// Pulls are the engine's own while simulation time is owned; the device outputs silence meanwhile.
			CHECK(ReadScratch(*engine, 2 * 800).has_value());

			engine->EndSimulationTime();
			CHECK_FALSE(engine->IsSimulationTimeOwned());
			CHECK(engine->GetTimeSource() == AudioTimeSource::Device);
			CHECK(engine->AdvanceSimulationTick() == 0);
		}

		TEST_CASE("AudioEngine: simulation time pulls round(n x 48000 / FixedHz) frames after n ticks" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			for (const uint32_t fixedHz : { 60u, 50u, 144u, 30u, 7u })
			{
				CAPTURE(fixedHz);
				engine->BeginSimulationTime(fixedHz);
				uint64_t total = 0;
				for (uint64_t tick = 1; tick <= 2 * fixedHz; ++tick)
				{
					total += engine->AdvanceSimulationTick();
					// round half up of tick x 48000 / fixedHz
					CHECK(total == (tick * 48000 * 2 + fixedHz) / (2 * fixedHz));
				}
				CHECK(total == 96000);
				engine->EndSimulationTime();
			}
		}

		TEST_CASE("AudioEngine: the host pull advances a device-less engine by the frame's delta" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true } });
			engine->Update(0.0, 0.0);
			const uint64_t start = GetCursor(*engine, voice);
			CHECK(start == 0);
			double now = 0.0;
			for (int frame = 0; frame < 60; ++frame)
			{
				now += 1.0 / 60.0;
				engine->Update(now, 1.0 / 60.0);
			}
			// One second of frames, give or take the accumulator's rounding of the last frame.
			const uint64_t cursor = GetCursor(*engine, voice);
			CHECK(cursor >= 47999);
			CHECK(cursor <= 48001);
		}

		TEST_CASE("AudioEngine: a full pool steals the lowest priority, then the farthest, then the oldest voice" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			const AudioVoiceSettings spatial{ .Loop = true, .Spatial = true };
			// Two low-priority spatial voices at different distances, then 62 higher-priority voices.
			const AudioVoiceHandle nearLow = PlayTestVoice(*engine,
				{ .Clip = clip, .Settings = spatial, .Transform = { .Position = glm::vec3(0.0f, 0.0f, -5.0f) }, .Priority = 0 });
			const AudioVoiceHandle farLow = PlayTestVoice(*engine,
				{ .Clip = clip, .Settings = spatial, .Transform = { .Position = glm::vec3(0.0f, 0.0f, -20.0f) }, .Priority = 0 });
			const AudioVoiceDescription looping{ .Clip = clip, .Settings = { .Loop = true }, .Priority = 1 };
			std::vector<AudioVoiceHandle> high;
			for (uint32_t index = 2; index < MaxAudioVoices; ++index)
				high.push_back(PlayTestVoice(*engine, looping));
			REQUIRE(engine->GetStats().LiveVoices == MaxAudioVoices);

			// The lowest priority first, the farthest of those first.
			const AudioVoiceHandle first = PlayTestVoice(*engine, looping);
			CHECK_FALSE(engine->IsVoiceAlive(farLow));
			CHECK(engine->IsVoiceAlive(nearLow));
			const AudioVoiceHandle second = PlayTestVoice(*engine, looping);
			CHECK_FALSE(engine->IsVoiceAlive(nearLow));
			// Equal priorities and distances: the oldest.
			const AudioVoiceHandle third = PlayTestVoice(*engine, looping);
			CHECK_FALSE(engine->IsVoiceAlive(high.front()));
			CHECK(engine->IsVoiceAlive(high[1]));
			CHECK(engine->IsVoiceAlive(first));
			CHECK(engine->IsVoiceAlive(second));
			CHECK(engine->IsVoiceAlive(third));
			CHECK(engine->GetStats().LiveVoices == MaxAudioVoices);
			CHECK(engine->GetStats().StolenVoices == 3);
			// A stolen voice's handle is stale.
			CHECK(ErrorCodeOf(engine->StopVoice(farLow)) == ErrorCode::NotFound);
		}

		TEST_CASE("AudioEngine: a full pool of higher-priority voices refuses a new voice" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			for (uint32_t index = 0; index < MaxAudioVoices; ++index)
				PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true }, .Priority = 5 });
			const Result<AudioVoiceHandle> refused = engine->PlayVoice({ .Clip = clip, .Priority = 4 });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			CHECK(engine->GetStats().LiveVoices == MaxAudioVoices);
			CHECK(engine->GetStats().StolenVoices == 0);
		}

		TEST_CASE("AudioEngine: a finished voice is released and its handle becomes stale" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone(0.1));
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip });
			const uint64_t length = GetInfo(*engine, voice).LengthFrames;
			CHECK(length == 4800);
			PullFrames(*engine, 4000);
			CHECK(engine->IsVoiceAlive(voice));
			PullFrames(*engine, 1600);
			CHECK_FALSE(engine->IsVoiceAlive(voice));
			CHECK(ErrorCodeOf(engine->GetVoiceInfo(voice)) == ErrorCode::NotFound);
			CHECK(engine->GetVoices().empty());

			// A looping voice never ends.
			const AudioVoiceHandle looping = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true } });
			PullFrames(*engine, 48000);
			CHECK(engine->IsVoiceAlive(looping));
		}

		TEST_CASE("AudioEngine: paused voices hold their cursor" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip });
			PullFrames(*engine, 1000);
			REQUIRE(engine->SetVoicePaused(voice, true).has_value());
			const bool paused = GetInfo(*engine, voice).Paused;
			CHECK(paused);
			PullFrames(*engine, 1000);
			const uint64_t held = GetCursor(*engine, voice);
			CHECK(held == 1000);
			REQUIRE(engine->SetVoicePaused(voice, false).has_value());
			PullFrames(*engine, 1000);
			const uint64_t resumed = GetCursor(*engine, voice);
			CHECK(resumed == 2000);

			const AudioVoiceHandle startsPaused = PlayTestVoice(*engine, { .Clip = clip, .StartPaused = true });
			PullFrames(*engine, 1000);
			const uint64_t notStarted = GetCursor(*engine, startsPaused);
			CHECK(notStarted == 0);
		}

		TEST_CASE("AudioEngine: stale and null handles are NotFound" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip });
			REQUIRE(engine->StopVoice(voice).has_value());
			CHECK_FALSE(engine->IsVoiceAlive(voice));
			CHECK(ErrorCodeOf(engine->StopVoice(voice)) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(engine->SetVoicePaused(voice, true)) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(engine->SetVoiceSettings(voice, {})) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(engine->SetVoiceTransform(voice, {})) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(engine->StopVoice(AudioVoiceHandle())) == ErrorCode::NotFound);
			REQUIRE(engine->UnregisterClip(clip).has_value());
			CHECK_FALSE(engine->IsClipRegistered(clip));
			CHECK(ErrorCodeOf(engine->UnregisterClip(clip)) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(engine->PlayVoice({ .Clip = clip })) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(engine->PlayVoice({ .Clip = AudioClipHandle() })) == ErrorCode::NotFound);
		}

		TEST_CASE("AudioEngine: invalid clips, settings, listeners and volumes are InvalidArgument" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const Ref<const Buffer> wav = CreateRef<const Buffer>(Test::MakeToneWav(MakeTone()));
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "", .Bytes = wav })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "project://Assets/A.wav", .Bytes = wav })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "0000000000000001@1", .Bytes = wav })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "0000000000000001", .Bytes = nullptr })) == ErrorCode::InvalidArgument);
			const Ref<const Buffer> oddPcm = CreateRef<const Buffer>(Buffer(3));
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "0000000000000002", .Format = AudioClipFormat::Pcm16, .Bytes = oddPcm }))
				== ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "0000000000000003", .Format = AudioClipFormat::Pcm16, .Bytes = wav, .ChannelCount = 3 }))
				== ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "0000000000000004", .Format = AudioClipFormat::Pcm16, .Bytes = wav, .Stream = true }))
				== ErrorCode::InvalidArgument);
			// An Encoded clip that decodes at registration fails there, with the decoder's error.
			const Ref<const Buffer> garbage = CreateRef<const Buffer>(Buffer(64, std::byte{ 0x5a }));
			CHECK(ErrorCodeOf(engine->RegisterClip({ .Name = "0000000000000005", .Bytes = garbage })) == ErrorCode::Parse);

			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000006", MakeTone());
			const float nan = std::numeric_limits<float>::quiet_NaN();
			CHECK(ErrorCodeOf(engine->PlayVoice({ .Clip = clip, .Settings = { .Volume = -1.0f } })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->PlayVoice({ .Clip = clip, .Settings = { .Pitch = 0.0f } })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->PlayVoice({ .Clip = clip, .Settings = { .Volume = nan } })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->PlayVoice({ .Clip = clip, .Settings = { .Spatialization = { .MinDistance = 10.0f, .MaxDistance = 5.0f } } }))
				== ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->PlayVoice({ .Clip = clip, .Transform = { .Position = glm::vec3(nan) } })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->SetListener({ .Forward = glm::vec3(0.0f) })) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->SetListener({ .Forward = glm::vec3(0.0f, 1.0f, 0.0f), .Up = glm::vec3(0.0f, 2.0f, 0.0f) }))
				== ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->SetGroupVolume(AudioGroup::Music, -0.5f)) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(engine->SetMasterVolume(std::numeric_limits<float>::infinity())) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(ReadScratch(*engine, 3)) == ErrorCode::InvalidArgument);
			CHECK(engine->GetVoices().empty());
		}

		TEST_CASE("AudioEngine: group and master volumes scale their voices" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Group = AudioGroup::Music, .Loop = true } });
			const float full = PullLevels(*engine, 4800).RmsLeft;
			REQUIRE(engine->SetGroupVolume(AudioGroup::Music, 0.5f).has_value());
			CHECK(engine->GetGroupVolume(AudioGroup::Music) == 0.5f);
			const float half = PullLevels(*engine, 4800).RmsLeft;
			CHECK(half == doctest::Approx(full * 0.5f).epsilon(0.02));
			// Another group's volume does not touch it; the master scales everything.
			REQUIRE(engine->SetGroupVolume(AudioGroup::Sfx, 0.0f).has_value());
			const float otherGroup = PullLevels(*engine, 4800).RmsLeft;
			CHECK(otherGroup == doctest::Approx(half).epsilon(0.02));
			REQUIRE(engine->SetMasterVolume(0.5f).has_value());
			CHECK(engine->GetMasterVolume() == 0.5f);
			const float quarter = PullLevels(*engine, 4800).RmsLeft;
			CHECK(quarter == doctest::Approx(full * 0.25f).epsilon(0.02));
		}

		TEST_CASE("AudioEngine: capture records only the frames the engine pulls" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());
			PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true } });
			CHECK_FALSE(engine->IsCapturing());
			CHECK(engine->StopCapture().Samples.empty());
			PullFrames(*engine, 100);
			engine->StartCapture();
			CHECK(engine->IsCapturing());
			const std::vector<float> pulled = PullFrames(*engine, 1000);
			engine->BeginSimulationTime(60);
			engine->AdvanceSimulationTick();
			engine->EndSimulationTime();
			const AudioCapture capture = engine->StopCapture();
			CHECK_FALSE(engine->IsCapturing());
			REQUIRE(capture.GetFrameCount() == 1800);
			CHECK(std::equal(pulled.begin(), pulled.end(), capture.Samples.begin()));
		}

		TEST_CASE("AudioEngine: ReadFrames is refused while the device reads the engine" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			REQUIRE(engine->GetTimeSource() == AudioTimeSource::Device);
			const Status refused = ReadScratch(*engine, 2 * 480);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			engine->BeginSimulationTime(60);
			CHECK(ReadScratch(*engine, 2 * 480).has_value());
			engine->EndSimulationTime();
		}

		TEST_CASE("AudioEngine: a rerouted notification is logged and keeps the device" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			const Test::ExpectLog rerouted(LogLevel::Info, "rerouted");
			engine->InjectDeviceNotification(AudioDeviceNotification::Rerouted);
			engine->Update(0.0, 0.0);
			CHECK(rerouted.GetMatchCount() == 1);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().DeviceRecreations == 0);
			for (const AudioDeviceNotification other : { AudioDeviceNotification::Started, AudioDeviceNotification::InterruptionBegan,
					 AudioDeviceNotification::InterruptionEnded, AudioDeviceNotification::Unlocked })
			{
				engine->InjectDeviceNotification(other);
				engine->Update(1.0, 0.0);
				CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			}
		}

		TEST_CASE("AudioEngine: a device that cannot be created at startup leaves the engine device-less with one warning"
			* doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const Test::ExpectLog warnings(LogLevel::Warn, "");
			const AudioEngineSpecification failing{
				.Device = AudioDeviceKind::Null, .Decoding = AudioDecoding::Deterministic, .InjectedDeviceCreationFailures = 1
			};
			const TestEngine engine = CreateTestEngine(vfs, failing);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Failed);
			CHECK(engine->GetTimeSource() == AudioTimeSource::Host);
			CHECK(engine->GetStats().FailedDeviceCreations == 1);
			CHECK(warnings.GetMatchCount() == 1);
			// Device-less from then on: nothing is retried, and the host pulls work.
			engine->Update(5.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Failed);
			CHECK(ReadScratch(*engine, 2 * 480).has_value());
			CHECK(warnings.GetMatchCount() == 1);
		}

		TEST_CASE("AudioEngine: one failed re-creation is retried a second later without a warning" * doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			engine->InjectDeviceCreationFailures(1);
			const Test::ExpectLog warnings(LogLevel::Warn, "");
			engine->InjectDeviceNotification(AudioDeviceNotification::Stopped);
			engine->Update(0.0, 0.0);
			engine->Update(1.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			engine->Update(2.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().FailedDeviceCreations == 1);
			CHECK(engine->GetStats().DeviceRecreations == 1);
			CHECK(warnings.GetMatchCount() == 0);
		}

		TEST_CASE("AudioEngine: registering a clip twice shares one registration and an unregistered clip keeps its voices"
			* doctest::skip(true))
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const Test::TestToneSpecification tone = MakeTone();
			const AudioClipHandle first = RegisterToneClip(*engine, "0000000000000001", tone);
			const AudioClipHandle second = RegisterToneClip(*engine, "0000000000000001", tone);
			CHECK(first == second);
			CHECK(engine->GetStats().RegisteredClips == 1);
			REQUIRE(engine->UnregisterClip(first).has_value());
			CHECK(engine->IsClipRegistered(first));
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = first, .Settings = { .Loop = true } });
			REQUIRE(engine->UnregisterClip(first).has_value());
			CHECK_FALSE(engine->IsClipRegistered(first));
			CHECK(engine->GetStats().RegisteredClips == 0);
			// The voice keeps the clip's data until it ends.
			const AudioLevels kept = PullLevels(*engine, 4800);
			CHECK(kept.Peak > 0.1f);
			CHECK(engine->IsVoiceAlive(voice));
			const AudioVoiceInfo info = GetInfo(*engine, voice);
			CHECK(info.ClipName == "0000000000000001");
			CHECK(info.ClipVersion == 1);
		}

		TEST_CASE("AudioEngine: a hot-reloaded clip's new version plays its own data while the old version's voices keep theirs"
			* doctest::skip(true))
		{
			// miniaudio never replaces the data of a registered name, so each version is a resource of its own
			// (MakeAudioResourceName). Version 1 is a tone and version 2 silence: a version-2 voice that played version 1's data
			// would be audible.
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			for (const bool stream : { false, true })
			{
				CAPTURE(stream);
				const AudioClipHandle versionOne = RegisterToneClip(*engine, "0000000000000001", MakeTone(), stream);
				const AudioVoiceHandle oldVoice = PlayTestVoice(*engine, { .Clip = versionOne, .Settings = { .Loop = true } });
				PullFrames(*engine, 800);

				const AudioClipSource versionTwo{ .Name = "0000000000000001",
					.Version = 2,
					.Format = AudioClipFormat::Encoded,
					.Bytes = CreateRef<const Buffer>(Test::MakeToneWav(MakeTone(1.0, 1, 0.0))),
					.Stream = stream };
				const Result<AudioClipHandle> reloaded = engine->RegisterClip(versionTwo);
				REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
				CHECK(*reloaded != versionOne);
				CHECK(engine->GetStats().RegisteredClips == 2);
				const AudioVoiceHandle newVoice = PlayTestVoice(*engine, { .Clip = *reloaded, .Settings = { .Loop = true } });
				const AudioVoiceInfo newInfo = GetInfo(*engine, newVoice);
				CHECK(newInfo.ClipName == "0000000000000001");
				CHECK(newInfo.ClipVersion == 2);
				const uint64_t oldVersion = GetInfo(*engine, oldVoice).ClipVersion;
				CHECK(oldVersion == 1);

				// Version 2 alone is silent: its voice plays version 2's samples.
				REQUIRE(engine->SetVoicePaused(oldVoice, true).has_value());
				const AudioLevels newLevels = PullLevels(*engine, 4800);
				CHECK(newLevels.Peak == 0.0f);
				// Version 1's voice still plays version 1's tone, also once its registration is gone.
				REQUIRE(engine->SetVoicePaused(oldVoice, false).has_value());
				REQUIRE(engine->SetVoicePaused(newVoice, true).has_value());
				REQUIRE(engine->UnregisterClip(versionOne).has_value());
				const AudioLevels oldLevels = PullLevels(*engine, 4800);
				CHECK(oldLevels.Peak > 0.1f);

				StopAllVoices(*engine);
				REQUIRE(engine->UnregisterClip(*reloaded).has_value());
				CHECK(engine->GetStats().RegisteredClips == 0);
			}
		}

		TEST_CASE("AudioEngine: MeasureAudioLevels reports per-channel RMS and the peak" * doctest::skip(true))
		{
			CHECK(MeasureAudioLevels({}).Peak == 0.0f);
			const std::vector<float> frames = { 0.5f, 0.0f, -0.5f, 0.0f, 0.5f, -1.0f, -0.5f, 0.0f };
			const AudioLevels levels = MeasureAudioLevels(frames);
			CHECK(levels.RmsLeft == doctest::Approx(0.5f));
			CHECK(levels.RmsRight == doctest::Approx(0.5f));
			CHECK(levels.Peak == 1.0f);
			// A trailing odd sample is ignored.
			const std::vector<float> odd = { 1.0f, 1.0f, 1.0f };
			CHECK(MeasureAudioLevels(odd).RmsLeft == doctest::Approx(1.0f));
		}
	}

}
