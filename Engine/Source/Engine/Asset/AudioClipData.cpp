#include "EnginePCH.h"
#include "Engine/Asset/AudioClipData.h"

#include "Engine/Core/Assert.h"

#include <utility>

namespace Engine {

	namespace Utils {

		// The silent clip's length (AudioClipData.h): 0.1 s at 48 kHz.
		constexpr uint64_t SilentClipFrameCount = 4800;

	}

	double GetAudioClipDuration(const AudioClipData& clip)
	{
		if (clip.SampleRate == 0)
			return 0.0;
		return static_cast<double>(clip.FrameCount) / static_cast<double>(clip.SampleRate);
	}

	Status ValidateAudioClipData(const AudioClipData& /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio clip validation is not implemented yet (M12 stream B)");
	}

	Buffer SerializeAudioClipPayload(const AudioClipData& /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AudioClipData> DeserializeAudioClipPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "audio clip payloads are not implemented yet (M12 stream B)");
	}

	Buffer CookAudioClip(const AudioClipData& /*clip*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<AudioClipData>> LoadCookedAudioClip(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "loading cooked audio clips is not implemented yet (M12 stream B)");
	}

	AudioClipData CreateSilentAudioClip()
	{
		AudioClipData clip;
		clip.Encoding = AudioClipEncoding::Pcm16;
		clip.SampleRate = 48000;
		clip.ChannelCount = 1;
		clip.FrameCount = Utils::SilentClipFrameCount;
		clip.Stream = false;
		clip.Bytes.assign(static_cast<size_t>(Utils::SilentClipFrameCount) * sizeof(int16_t), std::byte{ 0 });
		return clip;
	}

	std::string_view AudioClipEncodingToString(AudioClipEncoding encoding)
	{
		switch (encoding)
		{
			case AudioClipEncoding::Pcm16:  return "Pcm16";
			case AudioClipEncoding::Wav:    return "Wav";
			case AudioClipEncoding::Flac:   return "Flac";
			case AudioClipEncoding::Mp3:    return "Mp3";
			case AudioClipEncoding::Vorbis: return "Vorbis";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioClipEncoding {}", std::to_underlying(encoding));
		return "Unknown";
	}

}
