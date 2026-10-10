#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/AudioTestData.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>

namespace Engine {

	namespace {

		class AudioBindingFixture
		{
		public:
			explicit AudioBindingFixture(bool readOnly = false, RunModes mode = RunModes::Editor)
				: Script({ .Mode = mode, .TestMode = true, .ReadOnly = readOnly })
			{
				auto clip = CreateRef<AudioClipData>();
				clip->FrameCount = 4800;
				clip->Bytes = Test::MakeTonePcm16Bytes({});
				Script.GetAssetManager().Publish(AssetHandle(51), clip, "Assets/Tone.wav");
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
			}
			Status Start()
			{
				ENGINE_TRY_ASSIGN(Audio, AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, Vfs));
				System = CreateScope<AudioSystem>(Script.GetScene(), AudioSystemSpecification{ .Audio = Audio.get(), .Assets = Script.GetAssets() });
				System->Start();
				Script.Audio = System.get();
				return Script.Start();
			}
			VirtualFileSystem Vfs{};
			Test::ScriptTestFixture Script;
			Scope<AudioEngine> Audio{};
			Scope<AudioSystem> System{};
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
		for (const auto& member : coverage->Members)
			if (member.Owner == "Audio")
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
