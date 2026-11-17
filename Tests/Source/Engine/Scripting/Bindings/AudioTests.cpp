#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SessionMusic.h"
#include "Engine/Scripting/ScriptTestHost.h"
#include "Support/AudioTestData.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		// A test run's host for Test.GetMusic, which needs one; the music bindings use none of its services.
		class MusicTestHost final : public IScriptTestHost
		{
		public:
			void Report(const ScriptTestReport& /*report*/) override {}
			void OnScriptError(const ScriptError& /*error*/, bool /*fatal*/) override {}
			void OnExpectedScriptError(const ScriptError& /*error*/) override {}
			[[nodiscard]] Status InjectInput(const Json& /*event*/) override { return {}; }
			[[nodiscard]] Status CaptureScreenshot(std::string_view /*name*/) override { return {}; }
			[[nodiscard]] Status BeginAudioCapture(uint32_t /*ticks*/) override { return {}; }
			[[nodiscard]] bool IsAudioCaptureReady() const override { return false; }
			[[nodiscard]] Result<AudioLevels> EndAudioCapture() override { return AudioLevels{}; }
			void CancelAudioCapture() override {}
			[[nodiscard]] Status ReloadScript(AssetHandle /*script*/) override { return {}; }
			[[nodiscard]] uint64_t ComputeStateHash() const override { return 0; }
			[[nodiscard]] ScriptExtractionState GetLastExtraction() const override { return {}; }
			[[nodiscard]] std::optional<glm::vec3> GetExtractedPosition(UUID /*entity*/) const override { return std::nullopt; }
			void OnDebugBreak() override {}
		};

		class AudioBindingFixture
		{
		public:
			explicit AudioBindingFixture(bool readOnly = false, RunModes mode = RunModes::Editor)
				: Script({ .Mode = mode, .TestMode = true, .ReadOnly = readOnly, .TestHost = &Host })
			{
				auto clip = CreateRef<AudioClipData>();
				clip->FrameCount = 4800;
				clip->Bytes = Test::MakeTonePcm16Bytes({});
				Script.GetAssetManager().Publish(AssetHandle(51), clip, "Assets/Tone.wav");
				// M40: an intro of the tone's format, and one of another rate.
				auto intro = CreateRef<AudioClipData>();
				intro->FrameCount = 2400;
				intro->Bytes = Test::MakeTonePcm16Bytes({ .FrameCount = 2400 });
				Script.GetAssetManager().Publish(AssetHandle(52), intro, "Assets/Intro.sfx");
				auto other = CreateRef<AudioClipData>();
				other->SampleRate = 44100;
				other->FrameCount = 4410;
				other->Bytes = Test::MakeTonePcm16Bytes({ .SampleRate = 44100, .FrameCount = 4410 });
				Script.GetAssetManager().Publish(AssetHandle(53), other, "Assets/Other.sfx");
				const auto entity = Script.GetScene().CreateEntity("Source");
				AudioSourceComponent source;
				source.Clip.SetHandle(AssetHandle(51));
				source.Spatial = false;
				source.Loop = true;
				entity.AddComponent<AudioSourceComponent>(source);
			}
			~AudioBindingFixture()
			{
				Script.Stop();
				Script.Audio = nullptr;
				Script.Music = nullptr;
			}
			Status Start()
			{
				ENGINE_TRY_ASSIGN(Audio, AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, Vfs));
				System = CreateScope<AudioSystem>(Script.GetScene(), AudioSystemSpecification{ .Audio = Audio.get(), .Assets = Script.GetAssets() });
				System->Start();
				Script.Audio = System.get();
				// M40: the session's music, which a play session makes beside its AudioSystem.
				Music = CreateScope<SessionMusic>(SessionMusicSpecification{ .Audio = Audio.get(), .Assets = Script.GetAssets() });
				Script.Music = Music.get();
				return Script.Start();
			}
			VirtualFileSystem Vfs{};
			MusicTestHost Host{};
			Test::ScriptTestFixture Script;
			Scope<AudioEngine> Audio{};
			Scope<AudioSystem> System{};
			Scope<SessionMusic> Music{};
		};

	}

	static void CheckAudioBinding(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		if (!result)
			INFO(result.error().ToString());
		REQUIRE(result);
		CHECK(result->Value.Get() == true);
	}

	static void AudioMixerCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		AudioBindingFixture fixture(false, mode);
		REQUIRE(fixture.Start());
		CheckAudioBinding(fixture.Script, R"(
local clip = Assets.Load("Assets/Tone.wav")
assert(clip)
Audio.PlayOneShot(clip)
for _, group in {"Music", "Sfx", "Ui"} do
assert(Audio.GetGroupVolume(group) == 1)
Audio.SetGroupVolume(group, 0.5)
assert(Audio.GetGroupVolume(group) == 0.5)
Audio.PlayOneShot(clip, vector.create(1,2,3), 0.25, group)
end
assert(Audio.GetGroupVolume("sfx") == 0.5)
return true
)");
		const auto voices = fixture.Audio->GetVoices();
		REQUIRE(voices.size() == 4);
		CHECK(std::ranges::count_if(voices, [](const auto& voice)
		{
			return !voice.Settings.Spatial && voice.Settings.Group == AudioGroup::Sfx && voice.Settings.Volume == 1;
		}) == 1);
		CHECK(std::ranges::count_if(voices, [](const auto& voice)
		{
			return voice.Settings.Spatial && voice.Settings.Volume == 0.25f && voice.Transform.Position == glm::vec3(1, 2, 3);
		}) == 3);
		std::array<float, 1600> samples{};
		REQUIRE(fixture.Audio->ReadFrames(samples));
		CHECK(std::ranges::any_of(samples, [](float sample)
		{
			return sample != 0;
		}));
		CHECK(fixture.Audio->GetStats().DeviceKind == AudioDeviceKind::None);
		CHECK(fixture.Script.ExternalMutations.size() == 1);
		const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		// The mixer's members; the music's are AudioMusicCoverage's.
		constexpr std::array<std::string_view, 3> MixerMembers = { "PlayOneShot", "SetGroupVolume", "GetGroupVolume" };
		for (const auto& member : coverage->Members)
			if (member.Owner == "Audio" && std::ranges::find(MixerMembers, member.Member) != MixerMembers.end())
			{
				CAPTURE(member.Member);
				CHECK(member.Calls > 0);
				for (const auto& value : member.EnumValues)
				{
					CAPTURE(value.ValueName);
					CHECK(value.Count > 0);
				}
			}
		snapshots.push_back(*coverage);
	}

	// M40 (Docs/Decisions/0057-m40-music.md decision 6): the session's music through the Audio module, and Test.GetMusic.
	static void AudioMusicCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		AudioBindingFixture fixture(false, mode);
		REQUIRE(fixture.Start());
		CheckAudioBinding(fixture.Script, R"(
local clip = Assets.Load("Assets/Tone.wav")
assert(Audio.GetMusic() == nil)
assert(Test.GetMusic() == nil)
Audio.PlayMusic(clip, { Volume = 0.5, FadeSeconds = 0.05, Loop = false })
local music = Audio.GetMusic()
assert(music.Clip == clip and music.Volume == 0.5 and music.Loop == false)
-- The same clip keeps its voice: the request follows, the voice is not restarted.
Audio.PlayMusic(clip, { Volume = 0.75 })
music = Audio.GetMusic()
assert(music.Volume == 0.75 and music.Loop == true)
Audio.StopMusic(0.025)
assert(Audio.GetMusic() == nil)
-- Asked for again while it fades out, the clip's voice is taken back rather than restarted.
Audio.PlayMusic(clip)
return true
)");
		// One voice all along: kept, faded out, reclaimed at its volume.
		const std::vector<AudioVoiceInfo> voices = fixture.Audio->GetVoices();
		REQUIRE(voices.size() == 1);
		CHECK(fixture.Audio->GetStats().StartedVoices == 1);
		const AudioVoiceInfo& voice = voices.front();
		CHECK(voice.Settings.Group == AudioGroup::Music);
		CHECK(voice.Priority == AudioSystemMusicPriority);
		CHECK(voice.Settings.Loop);
		CHECK(voice.FadeVolume == 1.0f);
		CHECK(voice.FadeEnd == AudioFadeEnd::Hold);
		// Test.GetMusic reads the snapshot of the last AudioUpdate phase.
		CheckAudioBinding(fixture.Script, "return Test.GetMusic() == nil");
		fixture.Music->Update();
		CheckAudioBinding(fixture.Script, std::format(R"(
local voice = Test.GetMusic()
assert(voice.Voice == "{:016x}", voice.Voice)
assert(voice.Clip == Assets.Load("Assets/Tone.wav"))
assert(voice.CursorFrames == 0 and voice.LengthFrames == 4800)
assert(voice.FadeVolume == 1 and voice.FadingOut == 0)
return true
)",
											  voice.Voice.GetValue()));
		CHECK(fixture.Script.ExternalMutations.size() == 1);
		const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		for (const auto& member : coverage->Members)
			if ((member.Owner == "Audio" && member.Member.ends_with("Music")) || (member.Owner == "Test" && member.Member == "GetMusic"))
			{
				CAPTURE(member.Owner);
				CAPTURE(member.Member);
				CHECK(member.Calls > 0);
			}
		snapshots.push_back(*coverage);
	}

	static void AudioSourceCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		AudioBindingFixture fixture(false, mode);
		REQUIRE(fixture.Start());
		CheckAudioBinding(fixture.Script, R"(
local source = Scene.FindByName("Source").AudioSource
assert(not source:IsPlaying())
source:Play()
assert(source:IsPlaying())
source:Pause()
assert(not source:IsPlaying())
source:Resume()
assert(source:IsPlaying())
source:Stop()
assert(not source:IsPlaying())
return true
)");
		CHECK(fixture.Audio->GetVoices().empty());
		CHECK(fixture.Audio->GetStats().StartedVoices == 1);
		const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		for (const auto& member : coverage->Members)
			if (member.Owner == "AudioSource" && member.Kind == ScriptApiMemberKind::Method)
			{
				CAPTURE(member.Member);
				CHECK(member.Calls > 0);
			}
		snapshots.push_back(*coverage);
	}

	namespace Test {

		void RunAudioBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			AudioMixerCoverage(mode, snapshots);
			AudioMusicCoverage(mode, snapshots);
			AudioSourceCoverage(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("AudioBindings: one-shots and enum group mixing drive a device-free mixer")
		{
			std::vector<ScriptApiCoverage> snapshots;
			AudioMixerCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("AudioBindings: source proxy controls playback pause resume and stop")
		{
			std::vector<ScriptApiCoverage> snapshots;
			AudioSourceCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("AudioBindings: PlayMusic, StopMusic and GetMusic drive the session's music and Test.GetMusic reads its voice")
		{
			std::vector<ScriptApiCoverage> snapshots;
			AudioMusicCoverage(RunModes::Editor, snapshots);
		}

		// M40 (Docs/Decisions/0057-m40-music.md decision 12): PlayMusic's Intro plays before its clip, GetMusic reports it as
		// asked, and Test.GetMusic reports the voice's intro and whether it still plays it.
		TEST_CASE("AudioBindings: PlayMusic's intro plays before its clip and GetMusic and Test.GetMusic report it")
		{
			AudioBindingFixture fixture;
			REQUIRE(fixture.Start());
			CheckAudioBinding(fixture.Script, R"(
local clip = Assets.Load("Assets/Tone.wav")
local intro = Assets.Load("Assets/Intro.sfx")
Audio.PlayMusic(clip, { Intro = intro })
local music = Audio.GetMusic()
assert(music.Clip == clip and music.Intro == intro)
return true
)");
			const std::vector<AudioVoiceInfo> voices = fixture.Audio->GetVoices();
			REQUIRE(voices.size() == 1);
			CHECK(voices.front().ClipName == AssetHandle(52).ToString());
			CHECK(voices.front().NextClipName == AssetHandle(51).ToString());
			fixture.Music->Update();
			CheckAudioBinding(fixture.Script, R"(
local voice = Test.GetMusic()
assert(voice.Clip == Assets.Load("Assets/Tone.wav") and voice.Intro == Assets.Load("Assets/Intro.sfx"))
assert(voice.InIntro and voice.LengthFrames == 2400)
return true
)");
			// Past the intro the voice plays its clip.
			std::vector<float> frames(2 * 2600);
			REQUIRE(fixture.Audio->ReadFrames(frames));
			fixture.Music->Update();
			CheckAudioBinding(fixture.Script, R"(
local voice = Test.GetMusic()
assert(not voice.InIntro and voice.LengthFrames == 4800 and voice.CursorFrames == 200, voice.CursorFrames)
-- Without an intro, GetMusic's Intro is nil.
Audio.StopMusic()
Audio.PlayMusic(Assets.Load("Assets/Intro.sfx"))
return Audio.GetMusic().Intro == nil
)");
		}

		TEST_CASE("AudioBindings: the music calls refuse a nil clip, unknown or out-of-range options and a host without music")
		{
			AudioBindingFixture fixture;
			REQUIRE(fixture.Start());
			CheckAudioBinding(fixture.Script, R"(
local clip = Assets.Load("Assets/Tone.wav")
local function refused(fn, expected)
	local ok, message = pcall(fn)
	assert(not ok and string.find(message, expected, 1, true), message)
end
refused(function() Audio.PlayMusic(nil) end, "expected AssetRef")
refused(function() Audio.PlayMusic("Assets/Tone.wav") end, "expected AssetRef")
refused(function() Audio.PlayMusic(clip, 1) end, "expected MusicOptions table")
refused(function() Audio.PlayMusic(clip, { Volum = 1 }) end, "unknown MusicOptions key 'Volum'")
refused(function() Audio.PlayMusic(clip, { [1] = 1 }) end, "MusicOptions keys are Volume, FadeSeconds, Loop and Intro")
refused(function() Audio.PlayMusic(clip, { Intro = 1 }) end, "options.Intro expected AssetRef")
refused(function() Audio.PlayMusic(clip, { Intro = Scene.FindByName("Source") }) end, "options.Intro expected AssetRef, got another engine value")
refused(function() Audio.PlayMusic(clip, { Intro = Assets.Load("Assets/Other.sfx") }) end, "differ")
refused(function() Audio.PlayMusic(clip, { Volume = "loud" }) end, "options.Volume expected number")
refused(function() Audio.PlayMusic(clip, { FadeSeconds = true }) end, "options.FadeSeconds expected number")
refused(function() Audio.PlayMusic(clip, { Loop = 1 }) end, "options.Loop expected boolean")
refused(function() Audio.PlayMusic(clip, { Volume = -1 }) end, "Volume must be finite and at least 0")
refused(function() Audio.PlayMusic(clip, { Volume = 0/0 }) end, "Volume must be finite and at least 0")
refused(function() Audio.PlayMusic(clip, { Volume = math.huge }) end, "Volume must be finite and at least 0")
refused(function() Audio.PlayMusic(clip, { Volume = 1e300 }) end, "Volume must be finite and at least 0")
refused(function() Audio.PlayMusic(clip, { FadeSeconds = 61 }) end, "fade must be finite and from 0 to 60 seconds")
refused(function() Audio.PlayMusic(clip, { FadeSeconds = -1 }) end, "fade must be finite and from 0 to 60 seconds")
refused(function() Audio.StopMusic(-0.5) end, "fade must be finite and from 0 to 60 seconds")
refused(function() Audio.StopMusic(math.huge) end, "NaN or infinity")
assert(Audio.GetMusic() == nil)
return true
)");
			CHECK(fixture.Audio->GetVoices().empty());
			CHECK(fixture.Script.ExternalMutations.empty());

			// A read-only evaluation refuses both writes before any effect; the read stays allowed.
			AudioBindingFixture readOnly(true);
			REQUIRE(readOnly.Start());
			CheckAudioBinding(readOnly.Script, R"(
local clip = Assets.Load("Assets/Tone.wav")
assert(not pcall(Audio.PlayMusic, clip))
assert(not pcall(Audio.StopMusic))
return Audio.GetMusic() == nil
)");
			CHECK(readOnly.Audio->GetVoices().empty());

			// A host without music (Simulate, the edit evaluation) raises "audio is unavailable" for every music call.
			Test::ScriptTestFixture withoutMusic({ .TestMode = true });
			REQUIRE(withoutMusic.Start());
			CheckAudioBinding(withoutMusic, R"(
for _, fn in { function() Audio.PlayMusic(nil) end, function() Audio.StopMusic() end, function() Audio.GetMusic() end } do
	local ok, message = pcall(fn)
	assert(not ok and string.find(message, "audio is unavailable", 1, true), message)
end
return true
)");
		}

		TEST_CASE("AudioBindings: invalid values fail without voices mix changes or invalidation")
		{
			AudioBindingFixture fixture;
			REQUIRE(fixture.Start());
			CheckAudioBinding(fixture.Script, R"(
local clip = Assets.Load("Assets/Tone.wav")
for _, fn in {
	function() Audio.PlayOneShot("Assets/Tone.wav") end,
	function() Audio.PlayOneShot(clip, Color.New(1,1,1)) end,
	function() Audio.PlayOneShot(clip, vector.create(math.huge,0,0)) end,
	function() Audio.PlayOneShot(clip, nil, -1) end,
	function() Audio.PlayOneShot(clip, nil, 0/0) end,
	function() Audio.PlayOneShot(clip, nil, 1, "missing") end,
	function() Audio.SetGroupVolume("missing", 1) end,
	function() Audio.SetGroupVolume("Music", -1) end,
	function() Audio.SetGroupVolume("Sfx", math.huge) end,
	function() Audio.GetGroupVolume(1) end,
} do
	local ok, message = pcall(fn)
	assert(not ok and type(message) == "string")
end
assert(Audio.GetGroupVolume("Music") == 1 and Audio.GetGroupVolume("Sfx") == 1)
return true
)");
			CHECK(fixture.Audio->GetStats().StartedVoices == 0);
			CHECK(fixture.Script.ExternalMutations.empty());
		}

		TEST_CASE("AudioBindings: one-shots past the per-frame limit raise a located error")
		{
			AudioBindingFixture fixture;
			REQUIRE(fixture.Start());
			// A loop well within the watchdog budget may not grow the host's one-shot requests without bound.
			CheckAudioBinding(fixture.Script, std::format(R"(
local clip = Assets.Load("Assets/Tone.wav")
for _ = 1, {0} do Audio.PlayOneShot(clip) end
for _ = 1, 100 do
	local ok, message = pcall(function() Audio.PlayOneShot(clip) end)
	assert(not ok and string.find(message, "at most {0} one-shots start per frame", 1, true), message)
end
return true
)",
												  MaxAudioOneShotsPerFrame));
			CHECK(fixture.Audio->GetStats().StartedVoices == MaxAudioOneShotsPerFrame);
			CHECK(fixture.Audio->GetStats().LiveVoices == MaxAudioVoices);

			// Unprotected, the refusal is a located script error of the calling line.
			const Test::ExpectLog logged(LogLevel::Error, std::format("at most {} one-shots start per frame", MaxAudioOneShotsPerFrame));
			const auto refused = fixture.Script.Evaluate("local clip = Assets.Load(\"Assets/Tone.wav\")\nAudio.PlayOneShot(clip)\nreturn true");
			REQUIRE_FALSE(refused.has_value());
			CAPTURE(refused.error().ToString());
			CHECK(refused.error().ToString().find(std::format("at most {} one-shots start per frame", MaxAudioOneShotsPerFrame)) != std::string::npos);
			CHECK(refused.error().GetLocation().Line == 2);

			// The next frame admits one-shots again.
			fixture.System->Update(0.0);
			CheckAudioBinding(fixture.Script, "Audio.PlayOneShot(Assets.Load(\"Assets/Tone.wav\"))\nreturn true");
			CHECK(fixture.Audio->GetStats().StartedVoices == MaxAudioOneShotsPerFrame + 1);
		}

		TEST_CASE("AudioBindings: read-only evaluation rejects playback and mix writes before effects")
		{
			AudioBindingFixture fixture(true);
			REQUIRE(fixture.Start());
			CheckAudioBinding(fixture.Script, R"(
local source = Scene.FindByName("Source").AudioSource
local clip = Assets.Load("Assets/Tone.wav")
for _, fn in {
	function() Audio.PlayOneShot(clip) end,
	function() Audio.SetGroupVolume("Sfx", 0.5) end,
	function() source:Play() end,
	function() source:Pause() end,
	function() source:Resume() end,
	function() source:Stop() end,
} do assert(not pcall(fn)) end
assert(not source:IsPlaying() and Audio.GetGroupVolume("Sfx") == 1)
return true
)");
			CHECK(fixture.Audio->GetStats().StartedVoices == 0);
			CHECK(fixture.Script.ExternalMutations.empty());
		}
	}

}
