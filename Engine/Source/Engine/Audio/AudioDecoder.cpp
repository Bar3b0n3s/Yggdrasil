#include "EnginePCH.h"
#include "Engine/Audio/AudioDecoder.h"

#include "Engine/Audio/Private/MiniaudioSupport.h"
#include "Engine/Core/Assert.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace Engine {

	namespace {

		// The formats a decode accepts, for the messages of bytes that are none of them.
		constexpr std::string_view SupportedFormats = "WAV, FLAC, MP3 or Ogg Vorbis";
		constexpr uint32_t MinSampleRate = 8000;
		constexpr uint32_t MaxSampleRate = 192000;
		// Frames decoded per ma_decoder_read_pcm_frames call.
		constexpr uint64_t DecodeChunkFrames = 4096;

		// An initialized ma_decoder, uninitialized on destruction. Not copyable or movable (miniaudio keeps its address).
		class ScopedDecoder
		{
		public:
			ScopedDecoder() = default;
			~ScopedDecoder()
			{
				if (m_Initialized)
					ma_decoder_uninit(&m_Decoder);
			}

			ScopedDecoder(const ScopedDecoder&) = delete;
			ScopedDecoder& operator=(const ScopedDecoder&) = delete;

			[[nodiscard]] ma_result Initialize(std::span<const std::byte> bytes, ma_encoding_format format)
			{
				ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
				config.encodingFormat = format;
				const ma_result result = ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &m_Decoder);
				m_Initialized = result == MA_SUCCESS;
				return result;
			}

			[[nodiscard]] ma_decoder& Get() { return m_Decoder; }
		private:
			ma_decoder m_Decoder{};
			bool m_Initialized = false;
		};

	}

	namespace Utils {

		static bool HasPrefix(std::span<const std::byte> bytes, size_t offset, std::string_view prefix)
		{
			if (bytes.size() < offset || bytes.size() - offset < prefix.size())
				return false;
			return std::memcmp(bytes.data() + offset, prefix.data(), prefix.size()) == 0;
		}

		static uint8_t ByteAt(std::span<const std::byte> bytes, size_t offset)
		{
			return static_cast<uint8_t>(bytes[offset]);
		}

		// An MPEG audio frame header at `offset`: the 11-bit sync, a defined version and layer, and a bitrate and sample rate
		// index that are not reserved.
		static bool IsMpegFrameHeader(std::span<const std::byte> bytes, size_t offset)
		{
			if (bytes.size() < offset || bytes.size() - offset < 4)
				return false;
			const uint8_t first = ByteAt(bytes, offset);
			const uint8_t second = ByteAt(bytes, offset + 1);
			const uint8_t third = ByteAt(bytes, offset + 2);
			const bool sync = first == 0xFF && (second & 0xE0) == 0xE0;
			const bool version = ((second >> 3) & 0x03) != 0x01;
			const bool layer = ((second >> 1) & 0x03) != 0x00;
			const bool bitrate = (third >> 4) != 0x0F;
			const bool sampleRate = ((third >> 2) & 0x03) != 0x03;
			return sync && version && layer && bitrate && sampleRate;
		}

		// The size of an ID3v2 tag at the start of `bytes` (header, synchsafe size and optional footer); 0 without one.
		static size_t GetId3TagSize(std::span<const std::byte> bytes)
		{
			if (!HasPrefix(bytes, 0, "ID3") || bytes.size() < 10)
				return 0;
			size_t size = 0;
			for (size_t index = 6; index < 10; ++index)
				size = (size << 7) | (ByteAt(bytes, index) & 0x7F);
			const bool footer = (ByteAt(bytes, 5) & 0x10) != 0;
			return 10 + size + (footer ? 10 : 0);
		}

		// The first packet of an Ogg stream's first page (the codec's identification header), or an empty span when the page
		// is cut short.
		static std::span<const std::byte> GetFirstOggPacket(std::span<const std::byte> bytes)
		{
			constexpr size_t PageHeaderSize = 27;
			if (bytes.size() < PageHeaderSize)
				return {};
			const size_t segmentCount = ByteAt(bytes, 26);
			if (bytes.size() < PageHeaderSize + segmentCount)
				return {};
			size_t packetSize = 0;
			for (size_t segment = 0; segment < segmentCount; ++segment)
			{
				const uint8_t lacing = ByteAt(bytes, PageHeaderSize + segment);
				packetSize += lacing;
				if (lacing < 255)
					break;
			}
			const size_t start = PageHeaderSize + segmentCount;
			return bytes.subspan(start, std::min(packetSize, bytes.size() - start));
		}

		// The codec of an Ogg stream that is not Vorbis, from its identification header, for the refusal's message.
		static std::string_view DescribeOggCodec(std::span<const std::byte> packet)
		{
			if (HasPrefix(packet, 0, "OpusHead"))
				return "Opus";
			if (HasPrefix(packet, 0, "\177FLAC")) // 0x7F, then "FLAC"
				return "FLAC";
			if (HasPrefix(packet, 0, "Speex   "))
				return "Speex";
			return "an unknown codec";
		}

		// The container of `bytes`, from its magic numbers (never from a file extension). Errors: Parse naming the supported
		// formats, or the Ogg codec that is not Vorbis with a conversion hint.
		static Result<EncodedAudioFormat> DetectFormat(std::span<const std::byte> bytes)
		{
			if ((HasPrefix(bytes, 0, "RIFF") || HasPrefix(bytes, 0, "RIFX") || HasPrefix(bytes, 0, "RF64")) && HasPrefix(bytes, 8, "WAVE"))
				return EncodedAudioFormat::Wav;
			if (HasPrefix(bytes, 0, "fLaC"))
				return EncodedAudioFormat::Flac;
			if (HasPrefix(bytes, 0, "OggS"))
			{
				const std::span<const std::byte> packet = GetFirstOggPacket(bytes);
				if (HasPrefix(packet, 0, "\x01vorbis"))
					return EncodedAudioFormat::Vorbis;
				return std::unexpected(Error(ErrorCode::Parse, std::format("the Ogg file holds {}, not Vorbis", DescribeOggCodec(packet)))
						.WithHint(std::format("convert the file to {}", SupportedFormats)));
			}
			if (const size_t tag = GetId3TagSize(bytes); tag != 0)
				return HasPrefix(bytes, tag, "fLaC") ? EncodedAudioFormat::Flac : EncodedAudioFormat::Mp3;
			if (IsMpegFrameHeader(bytes, 0))
				return EncodedAudioFormat::Mp3;
			return std::unexpected(
				Error(ErrorCode::Parse, std::format("the data is not a supported audio file: expected a {} file", SupportedFormats)));
		}

		static ma_encoding_format ToMiniaudioFormat(EncodedAudioFormat format)
		{
			switch (format)
			{
				case EncodedAudioFormat::Wav:    return ma_encoding_format_wav;
				case EncodedAudioFormat::Flac:   return ma_encoding_format_flac;
				case EncodedAudioFormat::Mp3:    return ma_encoding_format_mp3;
				case EncodedAudioFormat::Vorbis: return ma_encoding_format_vorbis;
			}

			ENGINE_CORE_ASSERT(false, "Unknown EncodedAudioFormat {}", std::to_underlying(format));
			return ma_encoding_format_unknown;
		}

		// An Ogg stream is complete when its pages tile the bytes exactly and the last one ends the stream (the
		// end-of-stream flag). stb_vorbis decodes up to the last complete page of a truncated file without an error, so a
		// file cut at a page boundary or inside a page is caught here. Errors: Parse with the byte offset.
		static Status CheckOggPages(std::span<const std::byte> bytes)
		{
			constexpr size_t PageHeaderSize = 27;
			constexpr uint8_t EndOfStream = 0x04;
			size_t offset = 0;
			uint8_t lastFlags = 0;
			while (offset < bytes.size())
			{
				if (!HasPrefix(bytes, offset, "OggS"))
					return MakeError(ErrorCode::Parse, "corrupt Ogg file: no page starts at byte {}", offset);
				if (bytes.size() - offset < PageHeaderSize)
					return MakeError(ErrorCode::Parse, "truncated Ogg file: the page at byte {} is cut short", offset);
				const size_t segmentCount = ByteAt(bytes, offset + 26);
				if (bytes.size() - offset < PageHeaderSize + segmentCount)
					return MakeError(ErrorCode::Parse, "truncated Ogg file: the page at byte {} is cut short", offset);
				size_t bodySize = 0;
				for (size_t segment = 0; segment < segmentCount; ++segment)
					bodySize += ByteAt(bytes, offset + PageHeaderSize + segment);
				const size_t pageSize = PageHeaderSize + segmentCount + bodySize;
				if (bytes.size() - offset < pageSize)
				{
					return MakeError(ErrorCode::Parse, "truncated Ogg file: the page at byte {} needs {} bytes, {} are left", offset, pageSize,
						bytes.size() - offset);
				}
				lastFlags = ByteAt(bytes, offset + 5);
				offset += pageSize;
			}
			if ((lastFlags & EndOfStream) == 0)
				return MakeError(ErrorCode::Parse, "truncated Ogg file: the last page does not end the stream");
			return {};
		}

		// The checks of ProbeEncodedAudio and DecodeEncodedAudio; keeps the samples when `samples` is set.
		static Result<EncodedAudioInfo> Decode(std::span<const std::byte> bytes, std::vector<float>* samples)
		{
			ENGINE_TRY_ASSIGN(const EncodedAudioFormat format, DetectFormat(bytes));
			if (format == EncodedAudioFormat::Vorbis)
				ENGINE_TRY(CheckOggPages(bytes));

			ScopedDecoder decoder;
			if (const ma_result result = decoder.Initialize(bytes, ToMiniaudioFormat(format)); result != MA_SUCCESS)
				return MakeError(ErrorCode::Parse, "corrupt or truncated {} file: {}", EncodedAudioFormatToString(format), DescribeMiniaudioResult(result));

			ma_format sampleFormat = ma_format_unknown;
			ma_uint32 channels = 0;
			ma_uint32 sampleRate = 0;
			if (const ma_result result = ma_decoder_get_data_format(&decoder.Get(), &sampleFormat, &channels, &sampleRate, nullptr, 0);
				result != MA_SUCCESS)
			{
				return MakeError(ErrorCode::Parse, "corrupt {} file: {}", EncodedAudioFormatToString(format), DescribeMiniaudioResult(result));
			}
			if (channels != 1 && channels != 2)
			{
				return std::unexpected(Error(ErrorCode::Validation, std::format("the clip has {} channels; only mono and stereo clips are supported", channels))
						.WithHint("convert the file to mono or stereo"));
			}
			if (sampleRate < MinSampleRate || sampleRate > MaxSampleRate)
				return MakeError(ErrorCode::Validation, "the clip's sample rate of {} Hz is outside {} to {} Hz", sampleRate, MinSampleRate, MaxSampleRate);

			// The length a WAV or FLAC header states, which a complete file decodes in full; 0 when unknown. MP3 and Vorbis
			// lengths come from scanning the data itself (a truncated Ogg file was caught by its pages above).
			ma_uint64 declaredFrames = 0;
			const bool statesLength = format == EncodedAudioFormat::Wav || format == EncodedAudioFormat::Flac;
			if (!statesLength || ma_decoder_get_length_in_pcm_frames(&decoder.Get(), &declaredFrames) != MA_SUCCESS)
				declaredFrames = 0;

			std::vector<float> scratch;
			std::vector<float>& output = samples != nullptr ? *samples : scratch;
			output.clear();
			uint64_t frames = 0;
			for (;;)
			{
				const size_t offset = samples != nullptr ? static_cast<size_t>(frames) * channels : 0;
				output.resize(offset + static_cast<size_t>(DecodeChunkFrames) * channels);
				ma_uint64 read = 0;
				const ma_result result = ma_decoder_read_pcm_frames(&decoder.Get(), output.data() + offset, DecodeChunkFrames, &read);
				// Every sample the chunk decoded must be finite: an IEEE-float WAV can hold NaN or an infinity, and a 64-bit one
				// values beyond the range of f32, which the mixer would carry to the device and the capture buffer.
				const std::span<const float> decoded(output.data() + offset, static_cast<size_t>(read) * channels);
				const auto nonFinite = std::ranges::find_if(decoded, [](float sample)
				{
					return !std::isfinite(sample);
				});
				if (nonFinite != decoded.end())
				{
					const uint64_t frame = frames + static_cast<uint64_t>(std::distance(decoded.begin(), nonFinite)) / channels;
					return std::unexpected(Error(ErrorCode::Validation,
						std::format("the {} file holds a non-finite sample (NaN or infinity) at frame {}", EncodedAudioFormatToString(format), frame))
							.WithHint("re-export the file with finite samples, for example as 16-bit PCM"));
				}
				frames += read;
				if (result == MA_AT_END || (result == MA_SUCCESS && read == 0))
					break;
				if (result != MA_SUCCESS)
				{
					return MakeError(ErrorCode::Parse, "corrupt {} file: decoding failed at frame {}: {}", EncodedAudioFormatToString(format), frames,
						DescribeMiniaudioResult(result));
				}
			}
			output.resize(samples != nullptr ? static_cast<size_t>(frames) * channels : 0);

			if (declaredFrames != 0 && frames < declaredFrames)
			{
				return MakeError(ErrorCode::Parse, "truncated {} file: decoding ended at frame {} of {}", EncodedAudioFormatToString(format), frames,
					declaredFrames);
			}
			if (frames == 0)
				return MakeError(ErrorCode::Validation, "the {} file holds no audio frames", EncodedAudioFormatToString(format));
			return EncodedAudioInfo{ .Format = format, .SampleRate = sampleRate, .ChannelCount = channels, .FrameCount = frames };
		}

	}

	Result<EncodedAudioInfo> ProbeEncodedAudio(std::span<const std::byte> bytes)
	{
		return Utils::Decode(bytes, nullptr);
	}

	Result<DecodedAudio> DecodeEncodedAudio(std::span<const std::byte> bytes)
	{
		DecodedAudio decoded;
		ENGINE_TRY_ASSIGN(decoded.Info, Utils::Decode(bytes, &decoded.Samples));
		return decoded;
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
