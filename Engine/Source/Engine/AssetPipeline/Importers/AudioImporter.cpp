#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/AudioImporter.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"
#include "Engine/Audio/AudioDecoder.h"
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// The import settings: the importer's defaults for a null object (an import without settings), otherwise read
		// strictly through the registry.
		[[nodiscard]] static Result<AudioImportSettings> ReadAudioSettings(const Json& settings, const TypeRegistry& registry)
		{
			AudioImportSettings result;
			if (settings.is_null())
				return result;
			const StructInfo* type = registry.FindStruct<AudioImportSettings>();
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no AudioImportSettings (RegisterAssetPipelineTypes was not called)");
			ReadContext context;
			context.Strict = true;
			ENGINE_TRY(type->FromJson(&result, JsonReader(settings), context));
			return result;
		}

		// The clip encoding of a detected container (the Asset module has its own enum, AudioClipData.h).
		[[nodiscard]] static AudioClipEncoding ToClipEncoding(EncodedAudioFormat format)
		{
			switch (format)
			{
				case EncodedAudioFormat::Wav:    return AudioClipEncoding::Wav;
				case EncodedAudioFormat::Flac:   return AudioClipEncoding::Flac;
				case EncodedAudioFormat::Mp3:    return AudioClipEncoding::Mp3;
				case EncodedAudioFormat::Vorbis: return AudioClipEncoding::Vorbis;
			}
			ENGINE_CORE_ASSERT(false, "Unknown EncodedAudioFormat {}", std::to_underlying(format));
			return AudioClipEncoding::Wav;
		}

		// Whether the clip streams (AudioStreamMode): Auto streams a clip longer than MaxDecodedClipSeconds. The comparison
		// is FrameCount > SampleRate * 10 in double, exact for every frame count and rate a clip can have.
		[[nodiscard]] static bool ResolveStream(AudioStreamMode mode, const EncodedAudioInfo& info)
		{
			switch (mode)
			{
				case AudioStreamMode::Auto:   return static_cast<double>(info.FrameCount) > static_cast<double>(info.SampleRate) * MaxDecodedClipSeconds;
				case AudioStreamMode::Stream: return true;
				case AudioStreamMode::Decode: return false;
			}
			ENGINE_CORE_ASSERT(false, "Unknown AudioStreamMode {}", std::to_underlying(mode));
			return false;
		}

	}

	std::span<const std::string_view> AudioImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 4> Extensions = { ".wav", ".flac", ".mp3", ".ogg" };
		return Extensions;
	}

	Result<ImportResult> AudioImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		const std::string importing = std::format("while importing audio clip '{}'", sourcePath);
		ENGINE_TRY_ASSIGN(const AudioImportSettings settings,
			WithContext(Utils::ReadAudioSettings(context.GetSettings(), context.GetRegistry()), std::format("while reading the import settings of '{}'", sourcePath)));

		// The decoder detects the format from the bytes and decodes every frame once (AudioDecoder.h).
		const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(context.GetSourceBytes());
		if (!probed.has_value())
			return std::unexpected(Utils::MakeImportFailed(probed.error(), sourcePath, importing));

		AudioClipData clip;
		clip.Encoding = Utils::ToClipEncoding(probed->Format);
		clip.SampleRate = probed->SampleRate;
		clip.ChannelCount = probed->ChannelCount;
		clip.FrameCount = probed->FrameCount;
		clip.Stream = Utils::ResolveStream(settings.Stream, *probed);
		// The original file, byte for byte (§6.8).
		clip.Bytes.assign(context.GetSourceBytes().begin(), context.GetSourceBytes().end());
		const Status valid = ValidateAudioClipData(clip);
		if (!valid.has_value())
			return std::unexpected(Utils::MakeImportFailed(valid.error(), sourcePath, importing));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::AudioClip, .SubAssetKey = {}, .Cooked = CookAudioClip(clip, Version) });
		return result;
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
