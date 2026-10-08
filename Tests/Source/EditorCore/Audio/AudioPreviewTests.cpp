#include "TestsPCH.h"

#include "EditorCore/Audio/AudioPreview.h"

#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Audio/AudioEngine.h"
#include "Support/EditorTestFixture.h"

#include <vector>

// The asset browser's audio preview (Architecture §10.2, §12.2; Docs/Decisions/0015-m12-decisions.md). The editor without an
// audio engine is checked from the start; the other tests are skipped skeletons that need the AudioEngine (stream A) and
// the preview (stream C), and stream C removes the skips.

namespace Engine {

	namespace {

		constexpr AudioEngineSpecification TestAudio{ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic };

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AudioPreview: an editor without an audio engine has no preview")
		{
			Test::EditorTestFixture fixture("AudioPreviewNone");
			CHECK(fixture.GetEngine().GetAudioEngine() == nullptr);
			CHECK(fixture.GetEditor().GetAudioPreview() == nullptr);
			// EditorContext::Update and CloseProject work without one.
			fixture.GetEditor().Update(0.0);
			fixture.CreateAndOpenProject();
			fixture.GetEditor().CloseProject();
		}

		TEST_CASE("AudioPreview: plays a clip once in the Ui group and replaces the previous preview" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("AudioPreviewPlay", {}, nullptr, TestAudio);
			fixture.CreateAndOpenProject();
			AudioPreview* preview = fixture.GetEditor().GetAudioPreview();
			REQUIRE(preview != nullptr);
			AudioEngine& audio = *fixture.GetEngine().GetAudioEngine();
			CHECK_FALSE(preview->IsPlaying());
			CHECK_FALSE(preview->GetClip().IsValid());

			REQUIRE(preview->Play(BuiltinAssetHandles::SilentClip).has_value());
			CHECK(preview->IsPlaying());
			CHECK(preview->GetClip() == BuiltinAssetHandles::SilentClip);
			std::vector<AudioVoiceInfo> voices = audio.GetVoices();
			REQUIRE(voices.size() == 1);
			CHECK(voices.front().Settings.Group == AudioGroup::Ui);
			CHECK_FALSE(voices.front().Settings.Spatial);
			CHECK_FALSE(voices.front().Settings.Loop);
			CHECK(voices.front().Priority == AudioPreviewPriority);
			CHECK(voices.front().Owner == 0);

			// A second Play replaces the first.
			REQUIRE(preview->Play(BuiltinAssetHandles::SilentClip).has_value());
			CHECK(audio.GetVoices().size() == 1);
			preview->Stop();
			CHECK_FALSE(preview->IsPlaying());
			CHECK(audio.GetVoices().empty());
		}

		TEST_CASE("AudioPreview: a finished preview is forgotten at the next update" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("AudioPreviewEnd", {}, nullptr, TestAudio);
			fixture.CreateAndOpenProject();
			AudioPreview* preview = fixture.GetEditor().GetAudioPreview();
			REQUIRE(preview != nullptr);
			AudioEngine& audio = *fixture.GetEngine().GetAudioEngine();
			REQUIRE(preview->Play(BuiltinAssetHandles::SilentClip).has_value());
			// The silent clip lasts 0.1 s; a device-less engine advances by the frame's delta.
			audio.Update(0.0, 0.2);
			fixture.GetEditor().Update(0.2);
			CHECK_FALSE(preview->IsPlaying());
			CHECK_FALSE(preview->GetClip().IsValid());
			CHECK(audio.GetStats().RegisteredClips == 0);
		}

		TEST_CASE("AudioPreview: a missing or non-clip asset is an error and plays nothing" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("AudioPreviewErrors", {}, nullptr, TestAudio);
			fixture.CreateAndOpenProject();
			AudioPreview* preview = fixture.GetEditor().GetAudioPreview();
			REQUIRE(preview != nullptr);
			const Status null = preview->Play(AssetHandle());
			REQUIRE_FALSE(null.has_value());
			CHECK(null.error().GetCode() == ErrorCode::InvalidArgument);
			const Status missing = preview->Play(AssetHandle(0x0123456789abcdefull));
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			const Status mesh = preview->Play(BuiltinAssetHandles::CubeMesh);
			REQUIRE_FALSE(mesh.has_value());
			CHECK(mesh.error().GetCode() == ErrorCode::Validation);
			CHECK_FALSE(preview->IsPlaying());
			CHECK(fixture.GetEngine().GetAudioEngine()->GetVoices().empty());
		}

		TEST_CASE("AudioPreview: closing the project stops the preview" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("AudioPreviewClose", {}, nullptr, TestAudio);
			fixture.CreateAndOpenProject();
			AudioPreview* preview = fixture.GetEditor().GetAudioPreview();
			REQUIRE(preview != nullptr);
			REQUIRE(preview->Play(BuiltinAssetHandles::SilentClip).has_value());
			fixture.GetEditor().CloseProject();
			CHECK_FALSE(preview->IsPlaying());
			CHECK(fixture.GetEngine().GetAudioEngine()->GetVoices().empty());
		}
	}

}
