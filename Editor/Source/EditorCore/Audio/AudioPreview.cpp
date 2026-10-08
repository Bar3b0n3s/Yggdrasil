#include "EditorPCH.h"
#include "EditorCore/Audio/AudioPreview.h"

namespace Engine {

	struct AudioPreview::State
	{
		AudioEngine* Audio = nullptr;   // documented back-reference
		AssetManager* Assets = nullptr; // documented back-reference
	};

	AudioPreview::AudioPreview(AudioEngine& audio, AssetManager& assets)
		: m_State(CreateScope<State>())
	{
		m_State->Audio = &audio;
		m_State->Assets = &assets;
	}

	AudioPreview::~AudioPreview()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status AudioPreview::Play(AssetHandle /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio previews are not implemented yet (M12 stream C)");
	}

	void AudioPreview::Stop()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool AudioPreview::IsPlaying() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	AssetHandle AudioPreview::GetClip() const
	{
		ENGINE_CONTRACT_STUB();
		return AssetHandle();
	}

	void AudioPreview::Update()
	{
		ENGINE_CONTRACT_STUB();
	}

}
