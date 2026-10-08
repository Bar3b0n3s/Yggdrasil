#include "TestsPCH.h"

#include "Engine/Audio/AudioEngine.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AudioTestData.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"
#include "Support/WaitUntil.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The audio engine (Architecture §10.1, §10.4; Roadmap M12; Docs/Decisions/0015-m12-decisions.md). Every test runs a
// device-less engine (AudioDeviceKind::None) or miniaudio's Null backend as a real device (AudioDeviceKind::Null), never the
// machine's audio device (§15.1 T1), with deterministic decoding except the threaded-decoding case. The device cases wait
// for the Null device's thread with Test::WaitUntil, a bounded wait whose bound only limits a failure (ADR 0008 decision
// 15).

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

		// Counts the Warn entries logged, from any thread, while it is alive. Test::ExpectLog fails a test case whose
		// expectation never matched, so it cannot assert that nothing was logged. Not copyable or movable.
		class WarningCounter
		{
		public:
			WarningCounter()
				: m_ListenerId(Log::AddListener([this](const LogEntry& entry)
			{
				if (entry.Level == LogLevel::Warn)
					m_Count.fetch_add(1);
			}))
			{
			}

			~WarningCounter() { Log::RemoveListener(m_ListenerId); }

			WarningCounter(const WarningCounter&) = delete;
			WarningCounter& operator=(const WarningCounter&) = delete;

			[[nodiscard]] uint32_t GetCount() const { return m_Count.load(); }
		private:
			std::atomic<uint32_t> m_Count{ 0 }; // declared first: the listener may run as soon as it is registered
			uint64_t m_ListenerId = 0;
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

		// The bytes of Tests/Data/<relative>; fails the test case on error.
		Buffer ReadFixture(std::string_view relative)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(relative));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

		// The format fixtures of Tests/Data/Assets/Audio (ADR 0015 decisions 18 and 25), one per container the decoder knows.
		constexpr std::string_view AudioFixtures[] = {
			"Assets/Audio/Tone.wav",
			"Assets/Audio/Tone.flac",
			"Assets/Audio/Tone.mp3",
			"Assets/Audio/Tone.ogg",
		};

		// `bytes` registered as an Encoded clip under `name`, streamed or decoded at registration; fails the test case on error.
		AudioClipHandle RegisterEncodedClip(AudioEngine& engine, std::string name, const Buffer& bytes, bool stream)
		{
			const Result<AudioClipHandle> clip = engine.RegisterClip(
				{ .Name = std::move(name), .Format = AudioClipFormat::Encoded, .Bytes = CreateRef<const Buffer>(bytes), .Stream = stream });
			REQUIRE_MESSAGE(clip.has_value(), clip.error().ToString());
			return *clip;
		}

		// The current stats' device read and silent frame counts, for the device cases' bounded waits.
		uint64_t GetDeviceReadFrames(const AudioEngine& engine)
		{
			return engine.GetStats().DeviceReadFrames;
		}

		uint64_t GetDeviceSilentFrames(const AudioEngine& engine)
		{
			return engine.GetStats().DeviceSilentFrames;
		}

		// True when every sample is finite.
		bool AllFinite(std::span<const float> samples)
		{
			return std::ranges::all_of(samples, [](float sample)
			{
				return std::isfinite(sample);
			});
		}

		// How many clip frames a spatial voice of a 10 s, 48 kHz clip at (0, 0, -10) advances over 4,800 mixed frames, with
		// the voice moving at `sourceVelocity` and the listener (at the origin) at `listenerVelocity`, after 480 frames in
		// which miniaudio computes the voice's first Doppler pitch. 0 when the voice ended (a runaway pitch).
		uint64_t MeasureDopplerAdvance(const glm::vec3& sourceVelocity, const glm::vec3& listenerVelocity, float dopplerFactor = 1.0f)
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone(10.0));
			REQUIRE(engine->SetListener({ .Velocity = listenerVelocity }).has_value());
			const AudioVoiceHandle voice = PlayTestVoice(*engine,
				{ .Clip = clip,
					.Settings = { .Spatial = true, .Spatialization = { .Model = Attenuation::None, .DopplerFactor = dopplerFactor } },
					.Transform = { .Position = glm::vec3(0.0f, 0.0f, -10.0f), .Velocity = sourceVelocity } });
			PullFrames(*engine, 480);
			if (!engine->IsVoiceAlive(voice))
				return 0;
			const uint64_t before = GetCursor(*engine, voice);
			PullFrames(*engine, 4800);
			if (!engine->IsVoiceAlive(voice))
				return 0;
			return GetCursor(*engine, voice) - before;
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

		TEST_CASE("AudioEngine: a device-less engine starts with no device, host time and no voices")
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

		TEST_CASE("AudioEngine: a playing clip has non-zero RMS")
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

		TEST_CASE("AudioEngine: volume 0 is silent")
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

		TEST_CASE("AudioEngine: a source panned hard right has right RMS > left")
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

		TEST_CASE("AudioEngine: attenuation is monotonic with distance")
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

		TEST_CASE("AudioEngine: streamed and decoded playback of the same clip are sample-identical")
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

			// The same for every container: a decoded clip goes through DecodeEncodedAudio and a stream through the resource
			// manager's decoder over the AudioVfs, two code paths that must produce the same samples.
			for (const std::string_view fixture : AudioFixtures)
			{
				CAPTURE(std::string(fixture));
				const Buffer bytes = ReadFixture(fixture);
				std::vector<float> fromDecoded;
				std::vector<float> fromStream;
				for (const bool stream : { false, true })
				{
					const TestEngine engine = CreateTestEngine(vfs);
					const AudioClipHandle clip = RegisterEncodedClip(*engine, "0000000000000002", bytes, stream);
					PlayTestVoice(*engine, { .Clip = clip });
					// The whole clip at the mixing rate, and a stream page and a half past its end when it is shorter.
					(stream ? fromStream : fromDecoded) = PullFrames(*engine, 48000 * 5 + 24000);
				}
				CHECK(MeasureAudioLevels(fromDecoded).Peak > 0.01f);
				CHECK(fromStream == fromDecoded);
			}
		}

		TEST_CASE("AudioEngine: streamed playback captured twice is bit-identical")
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

			// Every container streams the same way twice.
			for (const std::string_view fixture : AudioFixtures)
			{
				CAPTURE(std::string(fixture));
				const Buffer bytes = ReadFixture(fixture);
				const auto captureFixture = [&bytes]()
				{
					VirtualFileSystem vfs;
					const TestEngine engine = CreateTestEngine(vfs);
					const AudioClipHandle clip = RegisterEncodedClip(*engine, "0000000000000002", bytes, true);
					engine->StartCapture();
					PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Volume = 0.8f, .Pitch = 1.25f } });
					for (int frame = 0; frame < 300; ++frame)
						PullFrames(*engine, 800);
					return engine->StopCapture();
				};
				const AudioCapture once = captureFixture();
				const AudioCapture again = captureFixture();
				CHECK(once.GetFrameCount() == 300 * 800);
				CHECK(MeasureAudioLevels(once.Samples).Peak > 0.01f);
				CHECK(once.Samples == again.Samples);
			}
		}

		TEST_CASE("AudioEngine: a stopped Null-backend device is re-created and the voice cursor is unchanged")
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
			CHECK(engine->GetStats().DeviceName.empty());
			// Time goes back to the engine, but nothing reads it while Recreating: no device exists to, and a non-zero delta
			// pulls nothing (the time source stays Device), so the cursor holds.
			engine->EndSimulationTime();
			CHECK(engine->GetTimeSource() == AudioTimeSource::Device);
			const uint64_t pulledBefore = engine->GetStats().PulledFrames;
			engine->Update(10.5, 0.5);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			CHECK(engine->GetStats().PulledFrames == pulledBefore);
			const uint64_t whileRecreating = GetCursor(*engine, voice);
			CHECK(whileRecreating == cursor);

			const uint64_t readBefore = GetDeviceReadFrames(*engine);
			engine->Update(11.0, 0.5);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().DeviceRecreations == 1);
			CHECK_FALSE(engine->GetStats().DeviceName.empty());

			// Only the device was re-created: the voice and its clip are untouched, and the new device plays the voice on from
			// where it was (its cursor moves once the device reads).
			REQUIRE(engine->IsVoiceAlive(voice));
			CHECK(engine->IsClipRegistered(clip));
			const uint64_t afterRecreation = GetCursor(*engine, voice);
			const bool deviceRead = Test::WaitUntil([&engine, readBefore]()
			{
				return GetDeviceReadFrames(*engine) > readBefore;
			});
			CHECK(deviceRead);
			const bool advanced = Test::WaitUntil([&engine, voice, afterRecreation]()
			{
				const Result<AudioVoiceInfo> info = engine->GetVoiceInfo(voice);
				return info.has_value() && info->CursorFrames != afterRecreation;
			});
			CHECK(advanced);
			CHECK(engine->IsVoiceAlive(voice));
		}

		TEST_CASE("AudioEngine: three failed re-creations leave the engine device-less with one warning")
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

		TEST_CASE("AudioEngine: lockstep pulls exactly 800 frames per tick and the device reads nothing")
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			REQUIRE(engine->GetDeviceState() == AudioDeviceState::Running);
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone());

			// Without simulation time the running Null device reads the engine (time source Device).
			CHECK(engine->GetTimeSource() == AudioTimeSource::Device);
			const bool deviceReads = Test::WaitUntil([&engine]()
			{
				return GetDeviceReadFrames(*engine) > 0;
			});
			CHECK(deviceReads);

			engine->BeginSimulationTime(60);
			CHECK(engine->IsSimulationTimeOwned());
			CHECK(engine->GetTimeSource() == AudioTimeSource::Simulation);
			// BeginSimulationTime waited for a device read in progress, so from here on the device must read nothing.
			const AudioEngineStats before = engine->GetStats();
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Loop = true } });
			for (int tick = 0; tick < 30; ++tick)
				CHECK(engine->AdvanceSimulationTick() == 800);
			// The device's callback ran (it filled frames with silence) while simulation time was owned, and read nothing.
			const bool deviceCalledBack = Test::WaitUntil([&engine, &before]()
			{
				return GetDeviceSilentFrames(*engine) > before.DeviceSilentFrames;
			});
			CHECK(deviceCalledBack);
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

		TEST_CASE("AudioEngine: simulation time pulls round(n x 48000 / FixedHz) frames after n ticks")
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

		TEST_CASE("AudioEngine: the host pull advances a device-less engine by the frame's delta")
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			// Two seconds, so one second of playback does not wrap the looping voice's cursor back to the start.
			const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone(2.0));
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

		TEST_CASE("AudioEngine: a full pool steals the lowest priority, then the farthest, then the oldest voice")
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

		TEST_CASE("AudioEngine: a full pool of higher-priority voices refuses a new voice")
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

		TEST_CASE("AudioEngine: a finished voice is released and its handle becomes stale")
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

		TEST_CASE("AudioEngine: paused voices hold their cursor")
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

		TEST_CASE("AudioEngine: stale and null handles are NotFound")
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

		TEST_CASE("AudioEngine: invalid clips, settings, listeners and volumes are InvalidArgument")
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

		TEST_CASE("AudioEngine: group and master volumes scale their voices")
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

		TEST_CASE("AudioEngine: capture records only the frames the engine pulls")
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

		TEST_CASE("AudioEngine: ReadFrames is refused while the device reads the engine")
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

		TEST_CASE("AudioEngine: a rerouted notification is logged and keeps the device")
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

		TEST_CASE("AudioEngine: a stop the backend follows with a reroute or a restart keeps the device")
		{
			// WASAPI reroutes a default-device change by stopping the device, rerouting it and starting it again, which posts
			// Stopped, Rerouted and Started: the device was never lost, so nothing is destroyed or re-created.
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			REQUIRE(engine->GetDeviceState() == AudioDeviceState::Running);
			const std::string deviceName = engine->GetStats().DeviceName;
			const Test::ExpectLog rerouted(LogLevel::Info, "rerouted");
			for (const AudioDeviceNotification notification :
				{ AudioDeviceNotification::Stopped, AudioDeviceNotification::Rerouted, AudioDeviceNotification::Started })
			{
				engine->InjectDeviceNotification(notification);
			}
			engine->Update(0.0, 0.0);
			CHECK(rerouted.GetMatchCount() == 1);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().DeviceName == deviceName);
			// No delayed re-creation either.
			engine->Update(5.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().DeviceRecreations == 0);

			// A stop followed by a restart alone (a backend that restarts without rerouting) keeps the device too.
			engine->InjectDeviceNotification(AudioDeviceNotification::Stopped);
			engine->InjectDeviceNotification(AudioDeviceNotification::Started);
			engine->Update(6.0, 0.0);
			engine->Update(10.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().DeviceRecreations == 0);
			// The device still plays: its callback keeps reading the engine.
			const uint64_t readBefore = GetDeviceReadFrames(*engine);
			const bool reading = Test::WaitUntil([&engine, readBefore]()
			{
				return GetDeviceReadFrames(*engine) > readBefore;
			});
			CHECK(reading);
		}

		TEST_CASE("AudioEngine: a device that cannot be created at startup leaves the engine device-less with one warning")
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

		TEST_CASE("AudioEngine: one failed re-creation is retried a second later without a warning")
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioDeviceKind::Null);
			engine->InjectDeviceCreationFailures(1);
			// Test::ExpectLog requires a match, so the absence of warnings is counted by a listener of its own.
			const WarningCounter warnings;
			engine->InjectDeviceNotification(AudioDeviceNotification::Stopped);
			engine->Update(0.0, 0.0);
			engine->Update(1.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Recreating);
			engine->Update(2.0, 0.0);
			CHECK(engine->GetDeviceState() == AudioDeviceState::Running);
			CHECK(engine->GetStats().FailedDeviceCreations == 1);
			CHECK(engine->GetStats().DeviceRecreations == 1);
			CHECK(warnings.GetCount() == 0);
		}

		TEST_CASE("AudioEngine: registering a clip twice shares one registration and an unregistered clip keeps its voices")
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

		TEST_CASE("AudioEngine: a hot-reloaded clip's new version plays its own data while the old version's voices keep theirs")
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

		TEST_CASE("AudioEngine: MeasureAudioLevels reports per-channel RMS and the peak")
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

		TEST_CASE("AudioEngine: a Pcm16 clip and the WAV file of the same samples play identically")
		{
			// Synthesized sound effects register their PCM directly; an Encoded clip is decoded once at registration. Both
			// reach the mixer as the same f32 samples.
			const Test::TestToneSpecification tone = MakeTone(0.5);
			VirtualFileSystem vfs;
			std::vector<float> fromWav;
			std::vector<float> fromPcm;
			{
				const TestEngine engine = CreateTestEngine(vfs);
				PlayTestVoice(*engine, { .Clip = RegisterToneClip(*engine, "0000000000000001", tone) });
				fromWav = PullFrames(*engine, 12000);
			}
			{
				const TestEngine engine = CreateTestEngine(vfs);
				const Result<AudioClipHandle> clip = engine->RegisterClip({ .Name = "0000000000000001",
					.Format = AudioClipFormat::Pcm16,
					.Bytes = CreateRef<const Buffer>(Test::MakeTonePcm16Bytes(tone)),
					.SampleRate = tone.SampleRate,
					.ChannelCount = tone.ChannelCount });
				REQUIRE_MESSAGE(clip.has_value(), clip.error().ToString());
				const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = *clip });
				const uint64_t length = GetInfo(*engine, voice).LengthFrames;
				CHECK(length == tone.FrameCount);
				fromPcm = PullFrames(*engine, 12000);
			}
			CHECK(MeasureAudioLevels(fromPcm).Peak > 0.1f);
			CHECK(fromPcm == fromWav);
		}

		TEST_CASE("AudioEngine: a clip at another sample rate plays resampled to the mixing rate")
		{
			// 24 kHz: every clip frame lasts two mixer frames, so after 4800 mixer frames the voice is 2400 frames in.
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const Test::TestToneSpecification tone{ .SampleRate = 24000, .ChannelCount = 2, .FrameCount = 24000, .Frequency = 440.0, .Amplitude = 0.5 };
			const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = RegisterToneClip(*engine, "0000000000000001", tone) });
			const AudioLevels levels = PullLevels(*engine, 4800);
			CHECK(levels.RmsLeft > 0.1f);
			CHECK(levels.RmsRight > 0.1f);
			const AudioVoiceInfo info = GetInfo(*engine, voice);
			CHECK(info.LengthFrames == 24000);
			CHECK(info.CursorFrames >= 2399);
			CHECK(info.CursorFrames <= 2401);
		}

		TEST_CASE("AudioEngine: threaded decoding plays decoded and streamed clips")
		{
			// Windowed runs keep miniaudio's job thread (AudioDecoding::Threaded): a stream's first pages load before
			// PlayVoice returns, later pages on the job thread.
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs, AudioEngineSpecification{ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Threaded });
			CHECK(engine->GetStats().Decoding == AudioDecoding::Threaded);
			for (const bool stream : { false, true })
			{
				CAPTURE(stream);
				const AudioClipHandle clip = RegisterToneClip(*engine, stream ? "0000000000000002" : "0000000000000001", MakeTone(), stream);
				const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip });
				const bool isStreamed = GetInfo(*engine, voice).Streamed;
				CHECK(isStreamed == stream);
				const AudioLevels levels = PullLevels(*engine, 4800);
				CHECK(levels.Peak > 0.1f);
				StopAllVoices(*engine);
				REQUIRE(engine->UnregisterClip(clip).has_value());
			}
			CHECK(engine->GetStats().RegisteredClips == 0);
		}

		TEST_CASE("AudioEngine: a clip registered again while a voice of its previous registration plays keeps playing")
		{
			// miniaudio keeps the data of a name while a voice uses it and gives a new registration of that name the same
			// data; the engine keeps that data alive for the new registration after the old voice ends.
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const Test::TestToneSpecification tone = MakeTone();
			const AudioClipHandle first = RegisterToneClip(*engine, "0000000000000001", tone);
			const AudioVoiceHandle oldVoice = PlayTestVoice(*engine, { .Clip = first, .Settings = { .Loop = true } });
			REQUIRE(engine->UnregisterClip(first).has_value());
			CHECK_FALSE(engine->IsClipRegistered(first));

			const AudioClipHandle second = RegisterToneClip(*engine, "0000000000000001", tone);
			CHECK(second != first);
			CHECK(engine->GetStats().RegisteredClips == 1);
			REQUIRE(engine->StopVoice(oldVoice).has_value());
			PlayTestVoice(*engine, { .Clip = second, .Settings = { .Loop = true } });
			const AudioLevels levels = PullLevels(*engine, 4800);
			CHECK(levels.Peak > 0.1f);
			CHECK(levels.Peak <= 0.51f);
		}

		TEST_CASE("AudioEngine: a streamed clip that cannot be decoded fails to play")
		{
			// A stream is not decoded at registration; its first voice is where the decoder fails.
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const Result<AudioClipHandle> clip = engine->RegisterClip(
				{ .Name = "0000000000000001", .Bytes = CreateRef<const Buffer>(Buffer(4096, std::byte{ 0x5a })), .Stream = true });
			REQUIRE_MESSAGE(clip.has_value(), clip.error().ToString());
			const Result<AudioVoiceHandle> voice = engine->PlayVoice({ .Clip = *clip });
			REQUIRE_FALSE(voice.has_value());
			CHECK((voice.error().GetCode() == ErrorCode::Io || voice.error().GetCode() == ErrorCode::Unsupported));
			CHECK(engine->GetVoices().empty());
			CHECK(engine->GetStats().StartedVoices == 0);
			REQUIRE(engine->UnregisterClip(*clip).has_value());
		}

		TEST_CASE("AudioEngine: the voices report their settings, owner and start order")
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			const AudioClipHandle clip = RegisterToneClip(*engine, "00000000000000a1", MakeTone());
			const AudioVoiceSettings settings{ .Group = AudioGroup::Music, .Volume = 0.25f, .Pitch = 1.5f, .Loop = true, .Spatial = true };
			const AudioVoiceTransform transform{ .Position = glm::vec3(1.0f, 2.0f, 3.0f) };
			const AudioVoiceHandle first = PlayTestVoice(*engine,
				{ .Clip = clip, .Settings = settings, .Transform = transform, .Priority = 3, .StartPaused = true, .Owner = 42 });
			const AudioVoiceHandle second = PlayTestVoice(*engine, { .Clip = clip });
			const std::vector<AudioVoiceInfo> voices = engine->GetVoices();
			REQUIRE(voices.size() == 2);
			CHECK(voices[0].Voice == first);
			CHECK(voices[1].Voice == second);
			CHECK(voices[0].StartSequence < voices[1].StartSequence);
			CHECK(voices[0].Clip == clip);
			CHECK(voices[0].ClipName == "00000000000000a1");
			CHECK(voices[0].Settings == settings);
			CHECK(voices[0].Transform == transform);
			CHECK(voices[0].Priority == 3);
			CHECK(voices[0].Owner == 42);
			CHECK(voices[0].Paused);
			CHECK_FALSE(voices[0].Streamed);
			CHECK(voices[0].LengthFrames == 48000);

			// Settings and transforms change while a voice plays.
			const AudioVoiceSettings changed{ .Group = AudioGroup::Ui, .Volume = 0.5f };
			REQUIRE(engine->SetVoiceSettings(second, changed).has_value());
			REQUIRE(engine->SetVoiceTransform(second, { .Position = glm::vec3(0.0f, 0.0f, -4.0f) }).has_value());
			const AudioVoiceInfo updated = GetInfo(*engine, second);
			CHECK(updated.Settings == changed);
			CHECK(updated.Transform.Position == glm::vec3(0.0f, 0.0f, -4.0f));
			const AudioEngineStats stats = engine->GetStats();
			CHECK(stats.LiveVoices == 2);
			CHECK(stats.StartedVoices == 2);
			CHECK(stats.RegisteredClips == 1);
		}

		TEST_CASE("AudioEngine: a clip with a non-finite sample is refused at registration and never reaches the output")
		{
			VirtualFileSystem vfs;
			const TestEngine engine = CreateTestEngine(vfs);
			std::vector<float> samples(4800, 0.5f);
			samples[100] = std::numeric_limits<float>::quiet_NaN();
			samples[200] = std::numeric_limits<float>::infinity();
			const Buffer wav = Test::MakeFloatWav(samples, 48000, 1);

			// A clip that decodes at registration is refused there (the decoder's Validation).
			const Result<AudioClipHandle> decoded =
				engine->RegisterClip({ .Name = "0000000000000001", .Format = AudioClipFormat::Encoded, .Bytes = CreateRef<const Buffer>(wav) });
			CHECK(ErrorCodeOf(decoded) == ErrorCode::Validation);

			// A stream is decoded while it plays (bytes that never went through an import, such as a hand-made pak entry): its
			// non-finite samples are silenced before the output and the capture.
			const AudioClipHandle streamed = RegisterEncodedClip(*engine, "0000000000000002", wav, true);
			engine->StartCapture();
			PlayTestVoice(*engine, { .Clip = streamed, .Settings = { .Pitch = 1.25f } });
			const std::vector<float> output = PullFrames(*engine, 4800);
			const AudioCapture capture = engine->StopCapture();
			CHECK(AllFinite(output));
			CHECK(AllFinite(capture.Samples));
			CHECK(MeasureAudioLevels(output).Peak > 0.1f);
			const AudioLevels levels = MeasureAudioLevels(output);
			CHECK(std::isfinite(levels.RmsLeft));
			CHECK(std::isfinite(levels.RmsRight));

			// Volumes whose product overflows are silenced the same way.
			StopAllVoices(*engine);
			const AudioClipHandle tone = RegisterToneClip(*engine, "0000000000000003", MakeTone());
			REQUIRE(engine->SetMasterVolume(3.0e38f).has_value());
			REQUIRE(engine->SetGroupVolume(AudioGroup::Sfx, 3.0e38f).has_value());
			PlayTestVoice(*engine, { .Clip = tone, .Settings = { .Volume = 3.0e38f } });
			CHECK(AllFinite(PullFrames(*engine, 4800)));
		}

		TEST_CASE("AudioEngine: a pitch above 16 plays at 16 and is reported as given")
		{
			// miniaudio converts a voice's resampling ratio to a 32-bit fixed-point rate, which a huge pitch overflows (it
			// then kept the old rate: a pitch of 1e30 played at 1).
			const auto pullAtPitch = [](float pitch)
			{
				VirtualFileSystem vfs;
				const TestEngine engine = CreateTestEngine(vfs);
				const AudioClipHandle clip = RegisterToneClip(*engine, "0000000000000001", MakeTone(4.0));
				const AudioVoiceHandle voice = PlayTestVoice(*engine, { .Clip = clip, .Settings = { .Pitch = pitch } });
				const float reported = GetInfo(*engine, voice).Settings.Pitch;
				CHECK(reported == pitch);
				std::vector<float> frames = PullFrames(*engine, 4800);
				const uint64_t cursor = GetCursor(*engine, voice);
				return std::pair(std::move(frames), cursor);
			};
			const auto [atSixteen, sixteenCursor] = pullAtPitch(16.0f);
			const auto [huge, hugeCursor] = pullAtPitch(1.0e30f);
			const auto [large, largeCursor] = pullAtPitch(5000.0f);
			CHECK(sixteenCursor >= 4800 * 16 - 16);
			CHECK(sixteenCursor <= 4800 * 16 + 16);
			CHECK(hugeCursor == sixteenCursor);
			CHECK(largeCursor == sixteenCursor);
			CHECK(huge == atSixteen);
			CHECK(large == atSixteen);
		}

		TEST_CASE("AudioEngine: the Doppler pitch stays within one third and three")
		{
			// The voice approaches (+Z, toward the listener at the origin) or recedes; miniaudio's Doppler pitch
			// (c - f vListener) / (c - f vSource) divides by zero at vSource = c / f. The engine keeps the factor times either
			// speed at or below c / 2, so a source at or above that speed plays at exactly twice its pitch.
			const glm::vec3 still(0.0f);
			const auto inRange = [](uint64_t advance, double pitch)
			{
				const double expected = 4800.0 * pitch;
				return static_cast<double>(advance) >= expected * 0.98 && static_cast<double>(advance) <= expected * 1.02;
			};
			CHECK(inRange(MeasureDopplerAdvance(still, still), 1.0));
			// 100 m/s toward the listener: c / (c - 100), unbounded.
			CHECK(inRange(MeasureDopplerAdvance(glm::vec3(0.0f, 0.0f, 100.0f), still), 343.3 / 243.3));
			// At and above the bound: twice the pitch (miniaudio ran away at 343 m/s and ignored 400 and 6,000).
			for (const float speed : { 343.0f, 343.3f, 400.0f, 6000.0f })
			{
				CAPTURE(speed);
				CHECK(inRange(MeasureDopplerAdvance(glm::vec3(0.0f, 0.0f, speed), still), 2.0));
			}
			// A large factor reaches the bound at a lower speed: 40 m/s with a factor of 10.
			CHECK(inRange(MeasureDopplerAdvance(glm::vec3(0.0f, 0.0f, 40.0f), still, 10.0f), 2.0));
			// The listener's speed is bounded too: rushing toward the source gives 3/2, rushing away 1/2 (miniaudio gave
			// 18.5 and a refused rate of 0).
			CHECK(inRange(MeasureDopplerAdvance(still, glm::vec3(0.0f, 0.0f, -6000.0f)), 1.5));
			CHECK(inRange(MeasureDopplerAdvance(still, glm::vec3(0.0f, 0.0f, 6000.0f)), 0.5));
			// A source receding at any speed stays at or above a third.
			CHECK(inRange(MeasureDopplerAdvance(glm::vec3(0.0f, 0.0f, -6000.0f), still), 2.0 / 3.0));
		}
	}

}
