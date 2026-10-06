#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <cstdint>

namespace Engine {

	// Registry enum "Attenuation" (Architecture §10.2): distance attenuation model.
	enum class Attenuation : uint8_t
	{
		None,
		Inverse,
		Linear,
		Exponential
	};

	// Registry enum "AudioGroup" (Architecture §10.1): the mixer group a voice plays in.
	enum class AudioGroup : uint8_t
	{
		Music,
		Sfx,
		Ui
	};

	// Registry name "AudioSource" (Architecture §5.3, §10.2): plays an audio clip, optionally spatialized at the entity.
	// MinDistance and MaxDistance in metres. The Attenuation member shares its enum's name, so the type is qualified with
	// the namespace.
	struct AudioSourceComponent
	{
		TypedAssetHandle<AssetType::AudioClip> Clip;
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool PlayOnStart = false;
		bool Spatial = true;
		float MinDistance = 1.0f;
		float MaxDistance = 50.0f;
		Engine::Attenuation Attenuation = Engine::Attenuation::Inverse;
		float Rolloff = 1.0f;
		float DopplerFactor = 1.0f;
		AudioGroup Group = AudioGroup::Sfx;
	};

}
