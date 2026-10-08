#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Probing and decoding encoded audio files with miniaudio's decoders (Architecture §7.4 AudioImporter: "probe and validate
// decode with miniaudio"): WAV, FLAC and MP3 (miniaudio's built-in decoders) and Ogg Vorbis (stb_vorbis, built into the
// vendored miniaudio, Vendor/miniaudio/VENDOR.md). AssetPipeline's AudioImporter uses it, so miniaudio stays private to the
// Audio module (§3 rule 3), and the AudioEngine decodes the clips that decode at registration with DecodeEncodedAudio
// (AudioEngine.h "Clips"). Frozen by the M12 contract (Docs/Decisions/0015-m12-decisions.md). Pure functions of the bytes,
// thread-safe (each call has its own ma_decoder); never assert on data.

namespace Engine {

	// The container of an encoded clip, detected from its bytes (never from a file extension).
	enum class EncodedAudioFormat : uint8_t
	{
		Wav,
		Flac,
		Mp3,
		Vorbis // Ogg Vorbis
	};

	// What a probe found. FrameCount is exact: the probe decodes every frame once.
	struct EncodedAudioInfo
	{
		EncodedAudioFormat Format = EncodedAudioFormat::Wav;
		uint32_t SampleRate = 0;   // Hz, as stored in the file
		uint32_t ChannelCount = 0; // as stored in the file
		uint64_t FrameCount = 0;   // PCM frames at SampleRate

		bool operator==(const EncodedAudioInfo&) const = default;

		[[nodiscard]] double GetDurationSeconds() const
		{
			return SampleRate == 0 ? 0.0 : static_cast<double>(FrameCount) / static_cast<double>(SampleRate);
		}
	};

	// A whole clip decoded to interleaved f32 at its own sample rate and channel count.
	struct DecodedAudio
	{
		EncodedAudioInfo Info{};
		std::vector<float> Samples{}; // FrameCount * ChannelCount samples
	};

	// Identifies the container (RIFF/WAVE, "fLaC", an MP3 frame or ID3 tag, "OggS" with a Vorbis identification header),
	// opens miniaudio's decoder over the bytes in memory and decodes every frame once, so a file that would fail halfway
	// through playback fails here. Errors: Parse listing the four formats ("WAV, FLAC, MP3 or Ogg Vorbis") for bytes that
	// are none of them (Ogg Opus and other Ogg codecs included, with the hint to convert the file); Parse for a truncated
	// or corrupt file (with miniaudio's result description and the frame reached); Validation for a sample rate outside
	// 8,000 to 192,000 Hz, a channel count other than 1 or 2 (hint: convert to mono or stereo) or no frames.
	[[nodiscard]] Result<EncodedAudioInfo> ProbeEncodedAudio(std::span<const std::byte> bytes);

	// ProbeEncodedAudio's checks, keeping the decoded samples (tests compare decoders and playback with it). Errors: as
	// ProbeEncodedAudio.
	[[nodiscard]] Result<DecodedAudio> DecodeEncodedAudio(std::span<const std::byte> bytes);

	// "Wav", "Flac", "Mp3", "Vorbis"; "Unknown" outside the enum (asserted).
	[[nodiscard]] std::string_view EncodedAudioFormatToString(EncodedAudioFormat format);

}
