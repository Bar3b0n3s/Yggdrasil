#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

// Frozen by the M12 contract (Docs/Decisions/0015-m12-decisions.md).

namespace Engine {

	class TypeRegistry;

	// Registry enum "AudioStreamMode": whether a clip decodes at load or streams (§10.1 "Clips").
	enum class AudioStreamMode : uint8_t
	{
		Auto,   // stream when the clip is longer than MaxDecodedClipSeconds (10 s), decode otherwise (§7.4)
		Stream, // always stream through the AudioVfs (MA_SOUND_FLAG_STREAM)
		Decode  // always decode the whole clip at load (MA_SOUND_FLAG_DECODE)
	};

	// Registry struct "AudioImportSettings". §7.4 describes the setting as "Stream defaults to true above 10 s"; a .meta is
	// written before the import knows the clip's length, so the default is the mode Auto, which the import resolves.
	struct AudioImportSettings
	{
		AudioStreamMode Stream = AudioStreamMode::Auto;
	};

	// WAV, FLAC, MP3 and Ogg Vorbis (Architecture §7.4; .ogg through the stb_vorbis decoder built into miniaudio, approved in
	// Docs/Decisions/0001-approvals.md, which replaces Appendix C's "OGG rejected with a conversion hint"). The import probes
	// and validates the whole file with Audio/AudioDecoder (ProbeEncodedAudio: format detected from the bytes, every frame
	// decoded once) and cooks one AudioClipData (CookAudioClip) holding the original bytes unchanged, the probed format,
	// sample rate, channel count and frame count, and Stream resolved from the settings. A file whose content is another
	// format than its extension says is imported as what it is. A file the decoder rejects (corrupt, truncated, Ogg Opus,
	// more than two channels, an unsupported sample rate) is ImportFailed with the decoder's message and hint. Pure function
	// of (bytes, settings, version); no dependency files.
	class AudioImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Audio";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::AudioClip; }
		// ".wav", ".flac", ".mp3", ".ogg".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return "AudioImportSettings"; }

		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// Registers AudioStreamMode and AudioImportSettings (RegisterAssetPipelineTypes calls it).
		static void RegisterTypes(TypeRegistry& registry);
	};

}
