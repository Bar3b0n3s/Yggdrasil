#include "EnginePCH.h"
#include "Engine/Asset/AudioClipData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <utility>

namespace Engine {

	namespace Utils {

		// The silent clip's length (AudioClipData.h): 0.1 s at 48 kHz.
		constexpr uint64_t SilentClipFrameCount = 4800;
		// The sample rates and channel counts a clip may have (AudioClipData.h).
		constexpr uint32_t MinAudioClipSampleRate = 8000;
		constexpr uint32_t MaxAudioClipSampleRate = 192000;
		constexpr uint32_t MaxAudioClipChannelCount = 2;
		// The bytes of one Pcm16 sample.
		constexpr uint64_t Pcm16SampleBytes = 2;

		[[nodiscard]] static bool IsKnownEncoding(uint8_t encoding)
		{
			return encoding <= std::to_underlying(AudioClipEncoding::Vorbis);
		}

	}

	double GetAudioClipDuration(const AudioClipData& clip)
	{
		if (clip.SampleRate == 0)
			return 0.0;
		return static_cast<double>(clip.FrameCount) / static_cast<double>(clip.SampleRate);
	}

	Status ValidateAudioClipData(const AudioClipData& clip)
	{
		if (!Utils::IsKnownEncoding(std::to_underlying(clip.Encoding)))
			return MakeError(ErrorCode::Validation, "the audio clip encoding {} is unknown", std::to_underlying(clip.Encoding));
		if (clip.SampleRate < Utils::MinAudioClipSampleRate || clip.SampleRate > Utils::MaxAudioClipSampleRate)
		{
			return MakeError(ErrorCode::Validation, "the sample rate {} Hz is outside {} to {} Hz", clip.SampleRate, Utils::MinAudioClipSampleRate,
				Utils::MaxAudioClipSampleRate);
		}
		if (clip.ChannelCount == 0 || clip.ChannelCount > Utils::MaxAudioClipChannelCount)
			return MakeError(ErrorCode::Validation, "an audio clip has 1 or 2 channels (got {})", clip.ChannelCount);
		if (clip.FrameCount == 0)
			return MakeError(ErrorCode::Validation, "an audio clip has at least one frame");

		if (clip.Encoding == AudioClipEncoding::Pcm16)
		{
			if (clip.Stream)
				return MakeError(ErrorCode::Validation, "a Pcm16 clip is already decoded and cannot stream");
			// Compared by division, so a huge FrameCount cannot overflow the expected size.
			const uint64_t frameBytes = static_cast<uint64_t>(clip.ChannelCount) * Utils::Pcm16SampleBytes;
			const uint64_t byteCount = clip.Bytes.size();
			if (byteCount % frameBytes != 0 || byteCount / frameBytes != clip.FrameCount)
			{
				return MakeError(ErrorCode::Validation, "a Pcm16 clip of {} frames and {} channel(s) holds {} bytes instead of {} per frame", clip.FrameCount,
					clip.ChannelCount, byteCount, frameBytes);
			}
			return {};
		}

		if (clip.Bytes.empty())
			return MakeError(ErrorCode::Validation, "the {} clip holds no bytes", AudioClipEncodingToString(clip.Encoding));
		return {};
	}

	Buffer SerializeAudioClipPayload(const AudioClipData& clip)
	{
		ENGINE_CORE_ASSERT(ValidateAudioClipData(clip).has_value(), "SerializeAudioClipPayload needs a valid clip");
		BinaryWriter writer;
		writer.WriteU8(std::to_underlying(clip.Encoding));
		writer.WriteU8(static_cast<uint8_t>(clip.Stream ? 1 : 0));
		writer.WriteU16(0);
		writer.WriteU32(clip.SampleRate);
		writer.WriteU32(clip.ChannelCount);
		writer.WriteU64(clip.FrameCount);
		writer.WriteU64(clip.Bytes.size());
		writer.WriteBytes(clip.Bytes);
		return writer.TakeBuffer();
	}

	Result<AudioClipData> DeserializeAudioClipPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		AudioClipData clip;
		ENGINE_TRY_ASSIGN(const uint8_t encoding, reader.ReadU8());
		if (!Utils::IsKnownEncoding(encoding))
			return MakeError(ErrorCode::Parse, "audio clip payload: unknown encoding {}", encoding);
		clip.Encoding = static_cast<AudioClipEncoding>(encoding);
		ENGINE_TRY_ASSIGN(const uint8_t stream, reader.ReadU8());
		if (stream > 1)
			return MakeError(ErrorCode::Parse, "audio clip payload: the Stream byte is {} instead of 0 or 1", stream);
		clip.Stream = stream == 1;
		ENGINE_TRY_ASSIGN(const uint16_t reserved, reader.ReadU16());
		if (reserved != 0)
			return MakeError(ErrorCode::Parse, "audio clip payload: the reserved field is {} instead of 0", reserved);
		ENGINE_TRY_ASSIGN(clip.SampleRate, reader.ReadU32());
		ENGINE_TRY_ASSIGN(clip.ChannelCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(clip.FrameCount, reader.ReadU64());
		ENGINE_TRY_ASSIGN(const uint64_t byteCount, reader.ReadU64());

		// The size is checked against the bytes left before anything is allocated (§6.8); trailing bytes are an error.
		if (byteCount != reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "audio clip payload: {} bytes announced, but {} follow at offset {}", byteCount, reader.GetRemaining(),
				reader.GetPosition());
		}
		ENGINE_TRY_ASSIGN(const std::span<const std::byte> bytes, reader.ReadBytes(static_cast<size_t>(byteCount)));
		clip.Bytes.assign(bytes.begin(), bytes.end());
		ENGINE_TRY(ValidateAudioClipData(clip));
		return clip;
	}

	Buffer CookAudioClip(const AudioClipData& clip, uint32_t importerVersion)
	{
		return WriteCookedArtifact(AssetType::AudioClip, AudioClipData::FormatVersion, importerVersion, SerializeAudioClipPayload(clip));
	}

	Result<AssetRef<AudioClipData>> LoadCookedAudioClip(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::AudioClip, AudioClipData::FormatVersion));
		ENGINE_TRY_ASSIGN(AudioClipData clip, DeserializeAudioClipPayload(view.Payload));
		return AssetRef<AudioClipData>(CreateRef<AudioClipData>(std::move(clip)));
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
