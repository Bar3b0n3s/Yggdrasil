#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

// The runtime component of an AudioSource during play (Architecture §5.3 "AudioSourceRuntime", §10.2): what
// Scene/AudioSystem keeps per source entity of its scene. Runtime-only like the components of RuntimeComponents.h (never
// registered, never serialized, invisible to scripts and automation, outside the state hash); private to AudioSystem
// (Docs/Decisions/0015-m12-decisions.md decisions 1 and 12), which adds it to every AudioSourceComponent's entity of its
// scene through the registry (the construct signal, Start, Update and the AudioSource methods), releases its voice when it
// is removed (the destroy signal of either component) and removes every one when the system goes.

namespace Engine {

	struct AudioSourceRuntime
	{
		// The source's voice; null when none plays (never started, stopped, or released when the entity was disabled). A
		// voice that reached the end of a non-looping clip leaves the handle stale until the next Update clears it.
		AudioVoiceHandle Voice{};
		// The AudioSource Clip the voice was started for: a different Clip restarts a playing voice with the new clip.
		AssetHandle Clip{};
		// The settings last given to the voice (PlayVoice or SetVoiceSettings), so Update sends only changes.
		AudioVoiceSettings Settings{};
		// AudioSystem::Pause (script level): the voice stays paused across the session's SetPaused(false) until Resume or Play.
		bool Paused = false;
		// The entity was effectively enabled at the last Start, Update or Play. False for a source the system has not seen
		// yet (created during play, through the construct signal), so the next Update starts it when it is PlayOnStart and
		// enabled, exactly as it starts a PlayOnStart source whose entity is enabled again.
		bool Enabled = false;
		// The world position at the last Start or Update, for the velocity of a spatial voice; HasPosition is false before
		// the first one and while the entity is disabled.
		glm::vec3 Position{ 0.0f };
		bool HasPosition = false;
	};

}
