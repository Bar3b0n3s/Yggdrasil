#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/AudioImporter.h"

#include "Engine/Reflection/TypeRegistry.h"

#include <array>

namespace Engine {

	std::span<const std::string_view> AudioImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 4> Extensions = { ".wav", ".flac", ".mp3", ".ogg" };
		return Extensions;
	}

	Result<ImportResult> AudioImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "importing audio files is not implemented yet (M12 stream B)");
	}

	void AudioImporter::RegisterTypes(TypeRegistry& registry)
	{
		registry.Enum<AudioStreamMode>("AudioStreamMode", "Whether an imported audio clip decodes when it loads or streams while it plays.")
			.Entry(AudioStreamMode::Auto, "Auto", "Stream clips longer than 10 seconds and decode the shorter ones when they load.")
			.Entry(AudioStreamMode::Stream, "Stream", "Always stream the clip while it plays.")
			.Entry(AudioStreamMode::Decode, "Decode", "Always decode the whole clip when it loads.");

		registry.Struct<AudioImportSettings>("AudioImportSettings", "How a WAV, FLAC, MP3 or Ogg Vorbis file becomes an audio clip.")
			.Field("Stream", &AudioImportSettings::Stream, "Whether the clip decodes when it loads or streams while it plays.");
	}

}
