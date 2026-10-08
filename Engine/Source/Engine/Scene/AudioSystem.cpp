#include "EnginePCH.h"
#include "Engine/Scene/AudioSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <utility>

namespace Engine {

	struct AudioSystem::State
	{
		Scene* TargetScene = nullptr;   // documented back-reference
		AudioEngine* Audio = nullptr;   // documented back-reference
		AssetManager* Assets = nullptr; // documented back-reference, may be null
		AudioListenerSelection Listener{};
	};

	AudioListenerSelection SelectAudioListener(const Scene& /*scene*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AudioSceneIssue> FindAudioSceneIssues(const Scene& /*scene*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	AudioClipSource MakeAudioClipSource(AssetHandle /*handle*/, uint64_t /*version*/, const AssetRef<AudioClipData>& /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	AudioSystem::AudioSystem(Scene& scene, const AudioSystemSpecification& specification)
		: m_State(CreateScope<State>())
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(specification.Audio != nullptr, "AudioSystem needs the context's audio engine");
		m_State->TargetScene = &scene;
		m_State->Audio = specification.Audio;
		m_State->Assets = specification.Assets;
	}

	AudioSystem::~AudioSystem()
	{
		ENGINE_CONTRACT_STUB();
	}

	void AudioSystem::Start()
	{
		ENGINE_CONTRACT_STUB();
	}

	void AudioSystem::Update(double /*deltaSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AudioSystem::SetPaused(bool /*paused*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool AudioSystem::IsPaused() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Status AudioSystem::Play(Entity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio sources are not implemented yet (M12 stream C)");
	}

	Status AudioSystem::Stop(Entity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio sources are not implemented yet (M12 stream C)");
	}

	Status AudioSystem::Pause(Entity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio sources are not implemented yet (M12 stream C)");
	}

	Status AudioSystem::Resume(Entity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio sources are not implemented yet (M12 stream C)");
	}

	bool AudioSystem::IsPlaying(ConstEntity /*entity*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<AudioVoiceHandle> AudioSystem::PlayOneShot(AssetHandle /*clip*/, const std::optional<glm::vec3>& /*position*/, float /*volume*/,
		AudioGroup /*group*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio one-shots are not implemented yet (M12 stream C)");
	}

	Status AudioSystem::SetGroupVolume(AudioGroup /*group*/, float /*volume*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "session group volumes are not implemented yet (M12 stream C)");
	}

	float AudioSystem::GetGroupVolume(AudioGroup /*group*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 1.0f;
	}

	const AudioListenerSelection& AudioSystem::GetListener() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Listener;
	}

	std::string_view AudioListenerSourceToString(AudioListenerSource source)
	{
		switch (source)
		{
			case AudioListenerSource::None:     return "None";
			case AudioListenerSource::Listener: return "Listener";
			case AudioListenerSource::Camera:   return "Camera";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioListenerSource {}", std::to_underlying(source));
		return "Unknown";
	}

}
