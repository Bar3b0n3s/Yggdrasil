#include "EnginePCH.h"
#include "Engine/Audio/AudioEngine.h"

#include "Engine/Core/Assert.h"

#include <format>
#include <utility>

namespace Engine {

	struct AudioEngine::State
	{
		AudioEngineSpecification Specification{};
	};

	AudioEngine::AudioEngine(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	AudioEngine::~AudioEngine()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<AudioEngine>> AudioEngine::Create(const AudioEngineSpecification& /*specification*/, const VirtualFileSystem& /*vfs*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the audio engine is not implemented yet (M12 stream A)");
	}

	const AudioEngineSpecification& AudioEngine::GetSpecification() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->Specification;
	}

	Result<AudioClipHandle> AudioEngine::RegisterClip(const AudioClipSource& /*source*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio clips are not implemented yet (M12 stream A)");
	}

	Status AudioEngine::UnregisterClip(AudioClipHandle /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio clips are not implemented yet (M12 stream A)");
	}

	bool AudioEngine::IsClipRegistered(AudioClipHandle /*clip*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<AudioVoiceHandle> AudioEngine::PlayVoice(const AudioVoiceDescription& /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio voices are not implemented yet (M12 stream A)");
	}

	Status AudioEngine::StopVoice(AudioVoiceHandle /*voice*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio voices are not implemented yet (M12 stream A)");
	}

	Status AudioEngine::SetVoicePaused(AudioVoiceHandle /*voice*/, bool /*paused*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio voices are not implemented yet (M12 stream A)");
	}

	Status AudioEngine::SetVoiceSettings(AudioVoiceHandle /*voice*/, const AudioVoiceSettings& /*settings*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio voices are not implemented yet (M12 stream A)");
	}

	Status AudioEngine::SetVoiceTransform(AudioVoiceHandle /*voice*/, const AudioVoiceTransform& /*transform*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio voices are not implemented yet (M12 stream A)");
	}

	bool AudioEngine::IsVoiceAlive(AudioVoiceHandle /*voice*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<AudioVoiceInfo> AudioEngine::GetVoiceInfo(AudioVoiceHandle /*voice*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio voices are not implemented yet (M12 stream A)");
	}

	std::vector<AudioVoiceInfo> AudioEngine::GetVoices() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status AudioEngine::SetListener(const AudioListenerPose& /*pose*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the audio listener is not implemented yet (M12 stream A)");
	}

	AudioListenerPose AudioEngine::GetListener() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status AudioEngine::SetGroupVolume(AudioGroup /*group*/, float /*volume*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio groups are not implemented yet (M12 stream A)");
	}

	float AudioEngine::GetGroupVolume(AudioGroup /*group*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 1.0f;
	}

	Status AudioEngine::SetMasterVolume(float /*volume*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio groups are not implemented yet (M12 stream A)");
	}

	float AudioEngine::GetMasterVolume() const
	{
		ENGINE_CONTRACT_STUB();
		return 1.0f;
	}

	void AudioEngine::Update(double /*nowSeconds*/, double /*deltaSeconds*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AudioEngine::BeginSimulationTime(uint32_t /*fixedHz*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AudioEngine::EndSimulationTime()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool AudioEngine::IsSimulationTimeOwned() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	uint32_t AudioEngine::AdvanceSimulationTick()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Status AudioEngine::ReadFrames(std::span<float> /*interleavedStereo*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "pulling audio frames is not implemented yet (M12 stream A)");
	}

	AudioTimeSource AudioEngine::GetTimeSource() const
	{
		ENGINE_CONTRACT_STUB();
		return AudioTimeSource::Host;
	}

	void AudioEngine::StartCapture()
	{
		ENGINE_CONTRACT_STUB();
	}

	AudioCapture AudioEngine::StopCapture()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	bool AudioEngine::IsCapturing() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	AudioDeviceState AudioEngine::GetDeviceState() const
	{
		ENGINE_CONTRACT_STUB();
		return AudioDeviceState::None;
	}

	AudioEngineStats AudioEngine::GetStats() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void AudioEngine::InjectDeviceNotification(AudioDeviceNotification /*notification*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AudioEngine::InjectDeviceCreationFailures(uint32_t /*count*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::string MakeAudioResourceName(std::string_view name, uint64_t version)
	{
		return std::format("{}@{}", name, version);
	}

	AudioLevels MeasureAudioLevels(std::span<const float> /*interleavedStereo*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string_view AudioDeviceKindToString(AudioDeviceKind kind)
	{
		switch (kind)
		{
			case AudioDeviceKind::None:   return "None";
			case AudioDeviceKind::System: return "System";
			case AudioDeviceKind::Null:   return "Null";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDeviceKind {}", std::to_underlying(kind));
		return "Unknown";
	}

	std::string_view AudioDecodingToString(AudioDecoding decoding)
	{
		switch (decoding)
		{
			case AudioDecoding::Deterministic: return "Deterministic";
			case AudioDecoding::Threaded:      return "Threaded";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDecoding {}", std::to_underlying(decoding));
		return "Unknown";
	}

	std::string_view AudioDeviceStateToString(AudioDeviceState state)
	{
		switch (state)
		{
			case AudioDeviceState::None:       return "None";
			case AudioDeviceState::Running:    return "Running";
			case AudioDeviceState::Recreating: return "Recreating";
			case AudioDeviceState::Failed:     return "Failed";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDeviceState {}", std::to_underlying(state));
		return "Unknown";
	}

	std::string_view AudioTimeSourceToString(AudioTimeSource source)
	{
		switch (source)
		{
			case AudioTimeSource::Device:     return "Device";
			case AudioTimeSource::Simulation: return "Simulation";
			case AudioTimeSource::Host:       return "Host";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioTimeSource {}", std::to_underlying(source));
		return "Unknown";
	}

	std::string_view AudioDeviceNotificationToString(AudioDeviceNotification notification)
	{
		switch (notification)
		{
			case AudioDeviceNotification::Started:           return "Started";
			case AudioDeviceNotification::Stopped:           return "Stopped";
			case AudioDeviceNotification::Rerouted:          return "Rerouted";
			case AudioDeviceNotification::InterruptionBegan: return "InterruptionBegan";
			case AudioDeviceNotification::InterruptionEnded: return "InterruptionEnded";
			case AudioDeviceNotification::Unlocked:          return "Unlocked";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDeviceNotification {}", std::to_underlying(notification));
		return "Unknown";
	}

}
