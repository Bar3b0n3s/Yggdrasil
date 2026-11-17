#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The clips one owner registered with the audio engine (Architecture §10.1, §10.2): a scene's AudioSystem, or the play
// session's music (Scene/SessionMusic.h; M40, Docs/Decisions/0057-m40-music.md decision 4), each with its own registry so
// that neither owner's end drops the other's clips. Private to the Scene module. Main thread only; not copyable or movable.

namespace Engine {

	class AssetManager;

	// The clip a voice plays: the handle and version it is registered under (the placeholder's when the requested clip is
	// unavailable) and its data.
	struct ResolvedAudioClip
	{
		AssetHandle Handle{};
		uint64_t Version = 1;
		AssetRef<AudioClipData> Data{};
	};

	class AudioClipRegistry
	{
	public:
		// `audio` and `assets` are documented back-references that outlive the registry. `audio` may be null (a play session
		// without an audio engine, whose music records its requests only): the registry then resolves clips and registers
		// none. `assets` may be null: every clip is then missing and plays the silent clip, with one warning per clip.
		// `owner` names the registry's owner in its logs ("AudioSystem", "SessionMusic").
		AudioClipRegistry(AudioEngine* audio, AssetManager* assets, std::string_view owner);
		// Clear.
		~AudioClipRegistry();

		AudioClipRegistry(const AudioClipRegistry&) = delete;
		AudioClipRegistry& operator=(const AudioClipRegistry&) = delete;

		// The clip `handle` as a voice plays it: GetOrPlaceholder, which serves the silent clip for a missing, failed or
		// mistyped clip and records its diagnostic once (§7.2, §10.4), registered as the placeholder then.
		[[nodiscard]] ResolvedAudioClip Resolve(AssetHandle handle);
		// The engine registration of `handle`'s current version (Resolve's), registered on first use and kept until Clear.
		// Only with an audio engine (verified). Errors: the engine's (AudioEngine::RegisterClip), with the context naming the
		// clip.
		[[nodiscard]] Result<AudioClipHandle> Acquire(AssetHandle handle);
		// The registration `handle` resolves to (Resolve's handle and version), without registering it; nullopt when it has
		// none.
		[[nodiscard]] std::optional<AudioClipHandle> Find(AssetHandle handle);
		// Drops the registrations this registry made that are not among `kept`; their voices keep their data until they end
		// (AudioEngine.h).
		void Retain(std::span<const AudioClipHandle> kept);
		// Drops every registration this registry made; their voices keep their data until they end (AudioEngine.h).
		void Clear();
		// Whether `clip` is the placeholder a missing, failed or mistyped clip resolves to (Resolve).
		[[nodiscard]] static bool IsPlaceholder(const ResolvedAudioClip& clip);
		// `handle`'s reference path for messages (its asset manager's, or its 16 hex digits without one).
		[[nodiscard]] std::string GetName(AssetHandle handle) const;
	private:
		AudioEngine* m_Audio = nullptr;   // documented back-reference, may be null
		AssetManager* m_Assets = nullptr; // documented back-reference, may be null
		std::string m_Owner;
		// One registration reference per (handle, version).
		std::map<std::pair<AssetHandle, uint64_t>, AudioClipHandle> m_Clips;
		// Without an asset manager: the silent clip every voice plays, and the clips already warned about.
		AssetRef<AudioClipData> m_Silence{};
		std::set<AssetHandle> m_WarnedClips;
	};

}
