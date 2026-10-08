#include "TestsPCH.h"

#include "Engine/Automation/Methods/AudioMethods.h"

#include "EditorCore/Audio/AudioPreview.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <string>

// audio.stats in process, through the editor's server (Architecture §13.5; ADR 0008: "an in-process round trip for every
// method"). Skipped skeletons of the M12 contract (Docs/Decisions/0015-m12-decisions.md): stream C implements and registers
// audio.stats and removes the skips; the Runtime's side is tested in RuntimeAutomationServerTests.cpp and test_runtime.py.
// The fixtures' engine contexts get a device-less, deterministic AudioEngine (stream A).

namespace Engine {

	namespace {

		constexpr AudioEngineSpecification TestAudio{ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic };

		// An entity named `name` with a looping AudioSource of the built-in silent clip.
		void CreateSpeaker(Test::AutomationFixture& fixture, const std::string& name, bool playOnStart)
		{
			const Json components = {
				{ "AudioSource",
					{ { "Clip", BuiltinAssetHandles::SilentClip.ToString() }, { "Loop", true }, { "PlayOnStart", playOnStart }, { "Spatial", false } } },
			};
			const Result<Json> created = fixture.Call("entity.create", Json{ { "name", name }, { "components", components } });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("AudioMethods: audio.stats reports the device, the groups and no voices in Edit mode" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("AudioStatsEdit", true, TestAudio);
			const Result<Json> stats = fixture.Call("audio.stats", Json::object());
			REQUIRE_MESSAGE(stats.has_value(), stats.error().ToString());
			CHECK((*stats)["deviceState"] == "None");
			CHECK((*stats)["deviceName"] == "");
			CHECK((*stats)["timeSource"] == "Host");
			CHECK((*stats)["sampleRate"] == 48000);
			CHECK((*stats)["masterVolume"] == 1.0);
			REQUIRE((*stats)["groups"].size() == 3);
			CHECK((*stats)["groups"][0]["group"] == "Music");
			CHECK((*stats)["groups"][1]["group"] == "Sfx");
			CHECK((*stats)["groups"][2]["group"] == "Ui");
			CHECK((*stats)["voiceCount"] == 0);
			CHECK((*stats)["voiceCapacity"] == 64);
			CHECK((*stats)["voices"].empty());
		}

		TEST_CASE("AudioMethods: audio.stats lists a lockstep session's voices with their clip and entity" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("AudioStatsPlay", true, TestAudio);
			CreateSpeaker(fixture, "Speaker", true);
			CreateSpeaker(fixture, "Quiet", false);
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			REQUIRE(fixture.Call("play.step", Json{ { "ticks", 3 } }).has_value());
			const Result<Json> stats = fixture.Call("audio.stats", Json::object());
			REQUIRE_MESSAGE(stats.has_value(), stats.error().ToString());
			CHECK((*stats)["timeSource"] == "Simulation");
			REQUIRE((*stats)["voiceCount"] == 1);
			const Json& voice = (*stats)["voices"][0];
			CHECK(JsonReader(voice["voice"]).ReadString().value_or(std::string()).size() == 16);
			CHECK(voice["clip"]["id"] == BuiltinAssetHandles::SilentClip.ToString());
			CHECK(voice["clip"]["path"] == "engine://Audio/Silence");
			CHECK(voice["clip"]["type"] == "AudioClip");
			CHECK(voice["entity"]["name"] == "Speaker");
			CHECK(voice["entity"]["path"] == "/Speaker");
			CHECK(voice["group"] == "Sfx");
			CHECK(voice["volume"] == 1.0);
			CHECK(voice["loop"] == true);
			CHECK(voice["spatial"] == false);
			CHECK(voice["position"].empty());
			CHECK(voice["paused"] == false);
			// Three lockstep ticks of the 4,800-frame looping clip.
			CHECK(voice["cursorFrames"] == 2400);
			CHECK(voice["lengthFrames"] == 4800);

			REQUIRE(fixture.Call("play.stop", Json::object()).has_value());
			const Result<Json> stopped = fixture.Call("audio.stats", Json::object());
			REQUIRE(stopped.has_value());
			CHECK((*stopped)["voiceCount"] == 0);
			CHECK((*stopped)["timeSource"] == "Host");
		}

		TEST_CASE("AudioMethods: audio.stats reports an asset-browser preview without an entity" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("AudioStatsPreview", true, TestAudio);
			AudioPreview* preview = fixture.GetEditor().GetAudioPreview();
			REQUIRE(preview != nullptr);
			REQUIRE(preview->Play(BuiltinAssetHandles::SilentClip).has_value());
			const Result<Json> stats = fixture.Call("audio.stats", Json::object());
			REQUIRE_MESSAGE(stats.has_value(), stats.error().ToString());
			REQUIRE((*stats)["voiceCount"] == 1);
			const Json& voice = (*stats)["voices"][0];
			CHECK(voice["group"] == "Ui");
			CHECK(voice["entity"]["id"] == "");
			CHECK(voice["priority"] == AudioPreviewPriority);
			preview->Stop();
		}

		TEST_CASE("AudioMethods: audio.stats is Unsupported without an audio engine and takes no params" * doctest::skip(true))
		{
			Test::AutomationFixture silent("AudioStatsNone");
			const Json response = silent.Request("audio.stats", Json::object());
			REQUIRE(response.contains("error"));
			CHECK(response["error"]["data"]["errorCode"] == Json("Unsupported"));

			Test::AutomationFixture fixture("AudioStatsParams", true, TestAudio);
			const Json unknown = fixture.Request("audio.stats", Json{ { "voices", true } });
			REQUIRE(unknown.contains("error"));
			CHECK(unknown["error"]["code"] == Json(-32602));
		}
	}

}
