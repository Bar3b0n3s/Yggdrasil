#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <utility>

// The Audio category (Docs/Decisions/0006-m3-decisions.md, decision 9; Architecture §10.2). Distances are metres and
// strictly positive, so distance attenuation never divides zero by zero; pitch is strictly positive.

namespace Engine {

	namespace Utils {

		// The smallest attenuation distance, in metres.
		constexpr float MinAudioDistance = 0.01f;
		// The smallest playback pitch (a multiplier of the clip's speed).
		constexpr float MinAudioPitch = 0.01f;

		// The attenuation range runs from MinDistance out to MaxDistance (equal is allowed: no attenuation).
		static void ValidateAudioSource(const AudioSourceComponent& source, ValidationContext& context)
		{
			if (source.MinDistance > source.MaxDistance)
				context.Error("MinDistance", "must not exceed MaxDistance");
		}

		static void MakeAudioSourceValid(AudioSourceComponent& source, Random& /*random*/)
		{
			if (source.MinDistance > source.MaxDistance)
				std::swap(source.MinDistance, source.MaxDistance);
		}

	}

	void RegisterAudioComponents(TypeRegistry& registry)
	{
		registry.Enum<Attenuation>("Attenuation", "How a spatial sound's volume falls off with distance.")
			.Entry(Attenuation::None, "None", "No distance attenuation.")
			.Entry(Attenuation::Inverse, "Inverse", "Inverse-distance falloff, like sound in open air (the default).")
			.Entry(Attenuation::Linear, "Linear", "Linear falloff from MinDistance to silence at MaxDistance.")
			.Entry(Attenuation::Exponential, "Exponential", "Exponential falloff, steeper than inverse-distance.");

		registry.Enum<AudioGroup>("AudioGroup", "The mixer group a sound plays in.")
			.Entry(AudioGroup::Music, "Music", "Background music.")
			.Entry(AudioGroup::Sfx, "Sfx", "Sound effects (the default).")
			.Entry(AudioGroup::Ui, "Ui", "User-interface sounds.");

		RegisterComponent<AudioSourceComponent>(registry, "AudioSource", "Plays an audio clip, optionally positioned at the entity.")
			.Category("Audio")
			.Version(1)
			.Field("Clip", &AudioSourceComponent::Clip, "The audio clip to play; null plays nothing.")
			.Field("Volume", &AudioSourceComponent::Volume, "The linear volume multiplier (1 is the clip's own level).", { .Min = 0.0 })
			.Field("Pitch", &AudioSourceComponent::Pitch, "The playback speed and pitch multiplier (1 is unchanged).",
				{ .Min = Utils::MinAudioPitch })
			.Field("Loop", &AudioSourceComponent::Loop, "Whether the clip restarts when it ends.")
			.Field("PlayOnStart", &AudioSourceComponent::PlayOnStart, "Whether the clip starts playing when the play session starts.")
			.Field("Spatial", &AudioSourceComponent::Spatial, "Whether the sound is positioned at the entity and attenuated with distance.")
			.Field("MinDistance", &AudioSourceComponent::MinDistance, "The distance within which the sound plays at full volume, in metres.",
				{ .Min = Utils::MinAudioDistance, .Unit = "m" })
			.Field("MaxDistance", &AudioSourceComponent::MaxDistance, "The distance beyond which the sound stops getting quieter, in metres.",
				{ .Min = Utils::MinAudioDistance, .Unit = "m" })
			.Field("Attenuation", &AudioSourceComponent::Attenuation, "The distance attenuation model of a spatial sound.")
			.Field("Rolloff", &AudioSourceComponent::Rolloff, "How quickly the attenuation model falls off (1 is the model's own rate).",
				{ .Min = 0.0 })
			.Field("DopplerFactor", &AudioSourceComponent::DopplerFactor, "The strength of the Doppler pitch shift (0 disables it).",
				{ .Min = 0.0 })
			.Field("Group", &AudioSourceComponent::Group, "The mixer group the sound plays in.")
			.Validate(&Utils::ValidateAudioSource)
			.Generate(&Utils::MakeAudioSourceValid);

		RegisterComponent<AudioListenerComponent>(registry, "AudioListener", "Hears the scene from the entity's position and orientation.")
			.Category("Audio")
			.Version(1)
			.Field("Primary", &AudioListenerComponent::Primary,
				"Whether this listener is used; with several primaries the first in canonical order wins, without any the primary camera listens.");
	}

}
