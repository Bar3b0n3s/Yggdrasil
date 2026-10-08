#include "EnginePCH.h"
#include "Engine/Audio/AudioDecoder.h"

#include "Engine/Core/Assert.h"

#include <utility>

namespace Engine {

	Result<EncodedAudioInfo> ProbeEncodedAudio(std::span<const std::byte> /*bytes*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "probing encoded audio is not implemented yet (M12 stream A)");
	}

	Result<DecodedAudio> DecodeEncodedAudio(std::span<const std::byte> /*bytes*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "decoding encoded audio is not implemented yet (M12 stream A)");
	}

	std::string_view EncodedAudioFormatToString(EncodedAudioFormat format)
	{
		switch (format)
		{
			case EncodedAudioFormat::Wav:    return "Wav";
			case EncodedAudioFormat::Flac:   return "Flac";
			case EncodedAudioFormat::Mp3:    return "Mp3";
			case EncodedAudioFormat::Vorbis: return "Vorbis";
		}

		ENGINE_CORE_ASSERT(false, "Unknown EncodedAudioFormat {}", std::to_underlying(format));
		return "Unknown";
	}

}
