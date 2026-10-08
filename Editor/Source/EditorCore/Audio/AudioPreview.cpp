#include "EditorPCH.h"
#include "EditorCore/Audio/AudioPreview.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/AudioSystem.h"

#include <algorithm>

namespace Engine {

	struct AudioPreview::State
	{
		AudioEngine* Audio = nullptr;   // documented back-reference
		AssetManager* Assets = nullptr; // documented back-reference
		// The preview's voice, the asset it plays and the engine registration it holds (one reference, dropped when the
		// preview ends); all null when none plays.
		AudioVoiceHandle Voice{};
		AssetHandle Clip{};
		AudioClipHandle Registration{};

		// Stops the voice when it still plays, drops the registration and forgets the preview.
		void Release();
	};

	void AudioPreview::State::Release()
	{
		if (Audio->IsVoiceAlive(Voice))
		{
			if (const Status stopped = Audio->StopVoice(Voice); !stopped)
				ENGINE_WARN("Audio preview: stopping the preview of {} failed: {}", Clip.ToString(), stopped.error().ToString());
		}
		if (!Registration.IsNull())
		{
			if (const Status unregistered = Audio->UnregisterClip(Registration); !unregistered)
				ENGINE_WARN("Audio preview: unregistering {} failed: {}", Clip.ToString(), unregistered.error().ToString());
		}
		Voice = AudioVoiceHandle();
		Clip = AssetHandle();
		Registration = AudioClipHandle();
	}

	AudioPreview::AudioPreview(AudioEngine& audio, AssetManager& assets)
		: m_State(CreateScope<State>())
	{
		m_State->Audio = &audio;
		m_State->Assets = &assets;
	}

	AudioPreview::~AudioPreview()
	{
		m_State->Release();
	}

	Status AudioPreview::Play(AssetHandle clip)
	{
		State& state = *m_State;
		state.Release();
		if (!clip.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "a preview needs an audio clip, not the null handle");

		// AssetManager::Load, never GetOrPlaceholder: a clip that cannot play is an error the panel shows (§7.2's placeholder
		// would play silence and hide it).
		ENGINE_TRY_ASSIGN(const AssetRef<Asset> asset, state.Assets->Load(clip));
		const AssetRef<AudioClipData> data = AssetCast<AudioClipData>(asset);
		if (data == nullptr)
		{
			return std::unexpected(Error(ErrorCode::Validation,
				std::format("'{}' is a {}, not an AudioClip", state.Assets->GetReferencePath(clip), AssetTypeToString(asset->GetAssetType())))
					.WithHint("preview an audio clip: an imported .wav, .flac, .mp3 or .ogg file, or a .sfx sound effect"));
		}

		const uint64_t version = std::max<uint64_t>(1, state.Assets->GetVersion(clip));
		ENGINE_TRY_ASSIGN(const AudioClipHandle registration, state.Audio->RegisterClip(MakeAudioClipSource(clip, version, data)));
		AudioVoiceDescription description;
		description.Clip = registration;
		description.Settings.Group = AudioGroup::Ui;
		description.Settings.Volume = 1.0f;
		description.Settings.Loop = false;
		description.Settings.Spatial = false;
		description.Priority = AudioPreviewPriority;
		Result<AudioVoiceHandle> voice = state.Audio->PlayVoice(description);
		if (!voice)
		{
			if (const Status unregistered = state.Audio->UnregisterClip(registration); !unregistered)
				ENGINE_WARN("Audio preview: unregistering {} failed: {}", clip.ToString(), unregistered.error().ToString());
			return std::unexpected(std::move(voice).error());
		}
		state.Voice = *voice;
		state.Clip = clip;
		state.Registration = registration;
		return {};
	}

	void AudioPreview::Stop()
	{
		m_State->Release();
	}

	bool AudioPreview::IsPlaying() const
	{
		return m_State->Audio->IsVoiceAlive(m_State->Voice);
	}

	AssetHandle AudioPreview::GetClip() const
	{
		return IsPlaying() ? m_State->Clip : AssetHandle();
	}

	void AudioPreview::Update()
	{
		State& state = *m_State;
		if (!state.Registration.IsNull() && !state.Audio->IsVoiceAlive(state.Voice))
			state.Release();
	}

}
