#include "TestsPCH.h"

#include "Engine/Audio/AudioTypes.h"

#include "Engine/Scene/Components/AudioSourceComponent.h"

#include <utility>

// The audio vocabulary (Architecture §10; Docs/Decisions/0015-m12-decisions.md). Header-only and implemented by the M12
// contract, so these tests run from the start.

namespace Engine {

	TEST_SUITE("Audio")
	{
		TEST_CASE("AudioTypes: the enums name themselves with their registry spelling")
		{
			static_assert(AttenuationToString(Attenuation::None) == "None");
			static_assert(AttenuationToString(Attenuation::Inverse) == "Inverse");
			static_assert(AttenuationToString(Attenuation::Linear) == "Linear");
			static_assert(AttenuationToString(Attenuation::Exponential) == "Exponential");
			static_assert(AudioGroupToString(AudioGroup::Music) == "Music");
			static_assert(AudioGroupToString(AudioGroup::Sfx) == "Sfx");
			static_assert(AudioGroupToString(AudioGroup::Ui) == "Ui");
			CHECK(AudioGroupToString(static_cast<AudioGroup>(AudioGroupCount)) == "Unknown");
			CHECK(std::to_underlying(AudioGroup::Ui) + 1u == AudioGroupCount);
		}

		TEST_CASE("AudioTypes: the mixing format and limits are those of the architecture")
		{
			static_assert(AudioSampleRate == 48000);
			static_assert(AudioChannelCount == 2);
			static_assert(MaxAudioVoices == 64);
			static_assert(MaxDecodedClipSeconds == 10.0);
			CHECK(MinAudioDistance > 0.0f);
			CHECK(MinAudioPitch > 0.0f);
		}

		TEST_CASE("AudioTypes: the spatialization defaults are the AudioSource component's")
		{
			const AudioSourceComponent source;
			const AudioSpatialization spatialization;
			CHECK(spatialization.Model == source.Attenuation);
			CHECK(spatialization.MinDistance == source.MinDistance);
			CHECK(spatialization.MaxDistance == source.MaxDistance);
			CHECK(spatialization.Rolloff == source.Rolloff);
			CHECK(spatialization.DopplerFactor == source.DopplerFactor);
			const AudioListenerPose pose;
			CHECK(pose.Forward == glm::vec3(0.0f, 0.0f, -1.0f));
			CHECK(pose.Up == glm::vec3(0.0f, 1.0f, 0.0f));
		}
	}

}
