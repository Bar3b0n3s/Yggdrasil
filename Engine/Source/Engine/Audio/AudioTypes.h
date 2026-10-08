#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

// The vocabulary of the audio module (Architecture §10), shared by the AudioEngine, the AudioSource component (Scene), the
// AudioSystem and automation: the mixing format, the voice limit, the mixer groups, the distance-attenuation models and the
// poses a listener and a spatial voice are heard at. Header-only; frozen by the M12 contract
// (Docs/Decisions/0015-m12-decisions.md). No miniaudio type appears here or in any other public audio header (§3 rule 3).

namespace Engine {

	// The engine's mixing format (§10.1): ma_engine always runs at 48 kHz, 2 channels (f32), in every mode, and the
	// engine-owned device plays that format (miniaudio converts to the native one).
	inline constexpr uint32_t AudioSampleRate = 48000;
	inline constexpr uint32_t AudioChannelCount = 2;
	// The voice pool (§10.1: a HandlePool<Voice> of 64 ma_sounds).
	inline constexpr uint32_t MaxAudioVoices = 64;
	// Clips up to this length decode at load (MA_SOUND_FLAG_DECODE); longer ones stream (MA_SOUND_FLAG_STREAM) through the
	// AudioVfs (§10.1). AudioImporter's Auto mode applies it (§7.4: "Stream setting defaults to true above 10 s").
	inline constexpr double MaxDecodedClipSeconds = 10.0;
	// The smallest attenuation distance, in metres (AudioSource MinDistance and MaxDistance, ADR 0006 decision 9), so a
	// distance model never divides zero by zero.
	inline constexpr float MinAudioDistance = 0.01f;
	// The smallest playback pitch, a multiplier of the clip's speed (AudioSource Pitch).
	inline constexpr float MinAudioPitch = 0.01f;

	// Registry enum "Attenuation" (§5.3, §10.2): how a spatial sound's volume falls off with distance. Moved here from
	// Scene/Components/AudioSourceComponent.h by the M12 contract, so the ECS-agnostic engine and the component share one
	// type; names and values unchanged (the component and its registration still use it).
	enum class Attenuation : uint8_t
	{
		None,
		Inverse,
		Linear,
		Exponential
	};

	// Registry enum "AudioGroup" (§10.1): the mixer group a voice plays in. The groups are ma_sound_groups under the Master
	// group (Master -> Music, Sfx, Ui); Master is their common parent, not a group a voice plays in. Moved here from
	// Scene/Components/AudioSourceComponent.h like Attenuation.
	enum class AudioGroup : uint8_t
	{
		Music,
		Sfx,
		Ui
	};

	// The number of AudioGroup values, for per-group arrays indexed by std::to_underlying(group).
	inline constexpr uint32_t AudioGroupCount = 3;

	// "None", "Inverse", "Linear", "Exponential" (the registry spelling); "Unknown" for a value outside the enum.
	[[nodiscard]] constexpr std::string_view AttenuationToString(Attenuation attenuation)
	{
		switch (attenuation)
		{
			case Attenuation::None:        return "None";
			case Attenuation::Inverse:     return "Inverse";
			case Attenuation::Linear:      return "Linear";
			case Attenuation::Exponential: return "Exponential";
		}
		return "Unknown";
	}

	// "Music", "Sfx", "Ui" (the registry spelling); "Unknown" for a value outside the enum.
	[[nodiscard]] constexpr std::string_view AudioGroupToString(AudioGroup group)
	{
		switch (group)
		{
			case AudioGroup::Music: return "Music";
			case AudioGroup::Sfx:   return "Sfx";
			case AudioGroup::Ui:    return "Ui";
		}
		return "Unknown";
	}

	// How a spatial voice is attenuated and Doppler-shifted (§10.2): the AudioSource fields of the same names, applied with
	// ma_sound_set_attenuation_model, _set_min_distance, _set_max_distance, _set_rolloff and _set_doppler_factor. Valid
	// values (AudioEngine checks them, InvalidArgument otherwise): every float finite; MinAudioDistance <= MinDistance <=
	// MaxDistance; Rolloff >= 0; DopplerFactor >= 0 (0 disables the Doppler shift).
	struct AudioSpatialization
	{
		Attenuation Model = Attenuation::Inverse;
		float MinDistance = 1.0f;  // metres: full volume within it
		float MaxDistance = 50.0f; // metres: no further attenuation beyond it
		float Rolloff = 1.0f;      // how quickly the model falls off (1 is the model's own rate)
		float DopplerFactor = 1.0f;

		bool operator==(const AudioSpatialization&) const = default;
	};

	// Where the scene is heard from (§10.2; ma_engine_listener_set_position, _set_direction, _set_world_up and
	// _set_velocity of listener 0): world space, right-handed, +Y up, metres and metres per second (§5.2 conventions). Forward
	// and Up need not be normalized; both must be finite and non-zero and must not be parallel (AudioEngine::SetListener
	// checks it). The default hears from the origin looking down -Z, like a camera without a transform.
	struct AudioListenerPose
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 Forward{ 0.0f, 0.0f, -1.0f };
		glm::vec3 Up{ 0.0f, 1.0f, 0.0f };
		glm::vec3 Velocity{ 0.0f };

		bool operator==(const AudioListenerPose&) const = default;
	};

}
