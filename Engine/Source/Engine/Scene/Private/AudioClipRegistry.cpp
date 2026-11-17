#include "EnginePCH.h"
#include "Engine/Scene/Private/AudioClipRegistry.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/AudioSystem.h"

#include <algorithm>
#include <format>

namespace Engine {

	AudioClipRegistry::AudioClipRegistry(AudioEngine* audio, AssetManager* assets, std::string_view owner)
		: m_Audio(audio), m_Assets(assets), m_Owner(owner)
	{
	}

	AudioClipRegistry::~AudioClipRegistry()
	{
		Clear();
	}

	ResolvedAudioClip AudioClipRegistry::Resolve(AssetHandle handle)
	{
		const AssetHandle placeholder = GetPlaceholderHandle(AssetType::AudioClip);
		if (m_Assets == nullptr)
		{
			if (handle != placeholder && m_WarnedClips.insert(handle).second)
				ENGINE_CORE_WARN("{}: the play session has no asset manager, so audio clip {} plays silence", m_Owner, handle.ToString());
			if (m_Silence == nullptr)
				m_Silence = CreateRef<AudioClipData>(CreateSilentAudioClip());
			return ResolvedAudioClip{ .Handle = placeholder, .Version = 1, .Data = m_Silence };
		}

		// GetOrPlaceholder serves the silent clip for a missing, failed or mistyped clip and records its diagnostic once
		// (§7.2, §10.4); the voice is then registered as the placeholder, which is what plays.
		AssetRef<AudioClipData> data = m_Assets->GetOrPlaceholder<AudioClipData>(handle);
		AssetHandle effective = handle;
		if (handle != placeholder && static_cast<const Asset*>(data.get()) == m_Assets->GetPlaceholder(AssetType::AudioClip).get())
			effective = placeholder;
		return ResolvedAudioClip{ .Handle = effective, .Version = std::max<uint64_t>(1, m_Assets->GetVersion(effective)), .Data = std::move(data) };
	}

	Result<AudioClipHandle> AudioClipRegistry::Acquire(AssetHandle handle)
	{
		ENGINE_CORE_VERIFY(m_Audio != nullptr, "{} registers audio clip {} without an audio engine", m_Owner, handle.ToString());
		const ResolvedAudioClip clip = Resolve(handle);
		const std::pair<AssetHandle, uint64_t> key{ clip.Handle, clip.Version };
		if (const auto found = m_Clips.find(key); found != m_Clips.end())
			return found->second;
		ENGINE_TRY_ASSIGN(const AudioClipHandle registered,
			WithContext(m_Audio->RegisterClip(MakeAudioClipSource(clip.Handle, clip.Version, clip.Data)), std::format("registering audio clip {}", clip.Handle.ToString())));
		m_Clips.emplace(key, registered);
		return registered;
	}

	bool AudioClipRegistry::IsPlaceholder(const ResolvedAudioClip& clip)
	{
		return clip.Handle == GetPlaceholderHandle(AssetType::AudioClip);
	}

	std::string AudioClipRegistry::GetName(AssetHandle handle) const
	{
		return m_Assets != nullptr ? m_Assets->GetReferencePath(handle) : handle.ToString();
	}

	void AudioClipRegistry::Clear()
	{
		// Without an engine nothing was registered.
		if (m_Audio == nullptr)
			return;
		for (const auto& [key, clip] : m_Clips)
		{
			if (const Status unregistered = m_Audio->UnregisterClip(clip); !unregistered)
				ENGINE_CORE_WARN("{}: unregistering audio clip {} failed: {}", m_Owner, key.first.ToString(), unregistered.error().ToString());
		}
		m_Clips.clear();
	}

}
