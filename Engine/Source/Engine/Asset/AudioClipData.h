#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Audio clip CPU data and its cooked payload (Architecture §6.8 "audio clip: original encoded bytes (WAV/FLAC/MP3) or
// synthesized PCM", §7.4 AudioImporter and SoundEffectImporter, §10.1 "Clips"). Produced by AudioImporter (the original
// file's bytes, probed and validated by Audio/AudioDecoder), SoundEffectImporter (Audio/SoundSynth's PCM) and the built-in
// silent clip; played by Scene/AudioSystem and EditorCore's AudioPreview, which register the bytes with the AudioEngine
// (MakeAudioClipSource, Scene/AudioSystem.h). The Asset module cannot include Audio (§3), so the encoding enum is its own.
// Frozen by the M12 contract (Docs/Decisions/0015-m12-decisions.md).

namespace Engine {

	// How a clip's bytes are encoded. The values are persisted (the cooked payload) and only ever appended to.
	enum class AudioClipEncoding : uint8_t
	{
		Pcm16, // interleaved little-endian signed 16-bit PCM (synthesized sound effects, the silent clip)
		Wav,   // the original files, decoded by miniaudio at play time
		Flac,
		Mp3,
		Vorbis // Ogg Vorbis (stb_vorbis inside miniaudio)
	};

	// A loaded audio clip (AssetType::AudioClip). Plain data; immutable once loaded (AssetRef<AudioClipData>).
	struct AudioClipData : Asset
	{
		static constexpr AssetType StaticType = AssetType::AudioClip;
		// The cooked payload layout below (CookedHeader::FormatVersion).
		static constexpr uint16_t FormatVersion = 1;

		AudioClipData()
			: Asset(StaticType)
		{
		}

		AudioClipEncoding Encoding = AudioClipEncoding::Pcm16;
		uint32_t SampleRate = 48000; // Hz: 8,000 to 192,000 (an encoded file's own rate, as probed)
		uint32_t ChannelCount = 1;   // 1 or 2
		uint64_t FrameCount = 0;     // PCM frames at SampleRate (probed for encoded files), >= 1
		// Stream the clip through the AudioVfs instead of decoding it at load (§10.1; AudioImporter's Stream setting). Always
		// false for Pcm16 clips, which are already decoded.
		bool Stream = false;
		// Pcm16: exactly FrameCount * ChannelCount samples (2 bytes each). Encoded: the original file, byte for byte.
		Buffer Bytes{};
	};

	// FrameCount / SampleRate in seconds (0 for a SampleRate of 0).
	[[nodiscard]] double GetAudioClipDuration(const AudioClipData& clip);

	// Checks: a known encoding; SampleRate 8,000 to 192,000; ChannelCount 1 or 2; FrameCount >= 1; Pcm16 with exactly
	// FrameCount * ChannelCount * 2 bytes and Stream false; an encoded clip with at least one byte. The encoded bytes
	// themselves are not decoded here (the Asset module has no decoder; AudioImporter probed them, and the AudioEngine reports
	// a clip it cannot decode). Pure. Errors: Validation naming the first violation.
	[[nodiscard]] Status ValidateAudioClipData(const AudioClipData& clip);

	// The cooked audio clip payload (FormatVersion 1), little-endian:
	//     uint8 Encoding; uint8 Stream (0 or 1); uint16 Reserved (0); uint32 SampleRate; uint32 ChannelCount;
	//     uint64 FrameCount; uint64 ByteCount; ByteCount bytes
	// Asserts ValidateAudioClipData. Pure; identical clips give identical bytes.
	[[nodiscard]] Buffer SerializeAudioClipPayload(const AudioClipData& clip);

	// Reads a payload written by SerializeAudioClipPayload, then ValidateAudioClipData. Never asserts on data (fuzzed like
	// the other payload readers, §15.2). Errors: Parse for truncation, trailing bytes, an unknown encoding, a Stream byte
	// other than 0 or 1 or a non-zero Reserved; Validation from ValidateAudioClipData.
	[[nodiscard]] Result<AudioClipData> DeserializeAudioClipPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload) of `clip`.
	[[nodiscard]] Buffer CookAudioClip(const AudioClipData& clip, uint32_t importerVersion);

	// The clip of a cooked artifact (the AudioClip loader of RegisterBuiltinLoaders). Errors: as ReadCookedArtifact and
	// DeserializeAudioClipPayload.
	[[nodiscard]] Result<AssetRef<AudioClipData>> LoadCookedAudioClip(std::span<const std::byte> cooked);

	// The built-in silent clip (BuiltinAssetHandles::SilentClip, engine://Audio/Silence; §7.2's placeholder for audio clips):
	// 0.1 s of 48 kHz mono Pcm16 zeros (4,800 frames). Pure; the asset managers generate it as a procedural built-in.
	[[nodiscard]] AudioClipData CreateSilentAudioClip();

	// "Pcm16", "Wav", "Flac", "Mp3", "Vorbis"; "Unknown" outside the enum (asserted).
	[[nodiscard]] std::string_view AudioClipEncodingToString(AudioClipEncoding encoding);

}
