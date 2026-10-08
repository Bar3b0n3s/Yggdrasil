#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// audio.stats (Architecture §13.5 observe domain: "voices with clip, position, volume, group"; in the Runtime subset), shared
// by the Editor and the Runtime (Engine/Automation/Methods, ADR 0012 decision 12). Frozen by the M12 contract
// (Docs/Decisions/0015-m12-decisions.md). Conventions as in MethodRegistry.h; the params are the shared NoParams.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "AudioVoiceSummary": one live voice of the host's AudioEngine.
	struct AudioVoiceSummary
	{
		std::string Voice{}; // the voice handle's value (Handle::GetValue) as 16 lowercase hex digits
		// The clip the voice plays: {id, path, type} through the host's asset manager when its registration name is an asset
		// handle (always, for AudioSystem and previews); the id alone otherwise.
		AssetSummary Clip{};
		// The AudioSource entity of the play scene that owns the voice (AudioVoiceInfo::Owner); empty for one-shots, previews
		// and an owner the play scene no longer has.
		EntitySummary Entity{};
		AudioGroup Group = AudioGroup::Sfx; // registry enum "AudioGroup"
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool Spatial = false;
		std::vector<float> Position{}; // [x, y, z] in metres for a spatial voice; empty for a non-spatial one
		bool Paused = false;
		bool Streamed = false;
		int32_t Priority = 0;
		uint32_t CursorFrames = 0; // the clip's own frames played so far (saturating, AutomationTypes.h)
		uint32_t LengthFrames = 0; // the clip's length in its own frames; 0 when not known yet
	};

	// Registry struct "AudioGroupVolume": one mixer group's linear volume.
	struct AudioGroupVolume
	{
		AudioGroup Group = AudioGroup::Sfx;
		float Volume = 1.0f;
	};

	// audio.stats {} (NoParams): the host's audio engine as AudioEngine::GetStats, GetVoices and the group volumes report it.
	struct AudioStatsResult
	{
		AudioDeviceState DeviceState = AudioDeviceState::None; // registry enum "AudioDeviceState"
		std::string DeviceName{};                              // empty without a device
		AudioTimeSource TimeSource = AudioTimeSource::Host;    // registry enum "AudioTimeSource"
		uint32_t SampleRate = AudioSampleRate;
		float MasterVolume = 1.0f;
		std::vector<AudioGroupVolume> Groups{}; // Music, Sfx, Ui
		uint32_t VoiceCount = 0;
		uint32_t VoiceCapacity = MaxAudioVoices;
		uint32_t StolenVoices = 0;               // since the engine started (saturating)
		uint32_t PulledFrames = 0;               // frames the engine pulled itself (saturating)
		uint32_t DeviceReadFrames = 0;           // frames the device read from the engine (saturating)
		std::vector<AudioVoiceSummary> Voices{}; // by start order (oldest first)
	};

	namespace Automation {

		// audio.stats. Works in Edit mode (previews only) and while playing (the session's voices with their entities, the
		// play scene's). Errors: Unsupported "this host has no audio engine" when AutomationMethodContext::GetAudioEngine is
		// null.
		[[nodiscard]] Result<AudioStatsResult> AudioStats(AutomationMethodContext& context, const NoParams& params);

	}

	// Registers the enums AudioDeviceState and AudioTimeSource and the structs above (AudioGroup is the components' enum,
	// registered with them).
	void RegisterAudioMethodTypes(TypeRegistry& registry);

	// Registers audio.stats: read-only, AllowedInBatch, available in the Runtime (§13.5 Runtime subset), not a tool (§13.8:
	// reached through engine_call).
	void RegisterAudioMethods(MethodRegistry& methods);

}
