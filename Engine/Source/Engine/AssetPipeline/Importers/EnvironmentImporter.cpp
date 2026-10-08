#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IEnvironmentBaker.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <stb_image.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <format>
#include <memory>
#include <string>

namespace Engine {

	namespace {

		// Releases pixels stb_image allocated.
		struct StbFloatDeleter
		{
			void operator()(float* pixels) const { stbi_image_free(pixels); }
		};

	}

	namespace Utils {

		// The hint of an OpenEXR source (§7.4).
		constexpr std::string_view OpenExrHint = "download the .hdr variant from Poly Haven";
		// The widest equirectangular source, as EnvironmentBaker accepts it (8k: a 1024² skybox face, the largest, needs no more
		// than a 4k source).
		constexpr uint32_t MaxEquirectWidth = 8192;
		// The range of EnvironmentImportSettings::MaxLuminance: 1 to the largest binary16 value.
		constexpr double MinClampLuminance = 1.0;
		constexpr double MaxClampLuminance = 65504.0;
		constexpr int RgbChannels = 3;

		[[nodiscard]] static bool IsOpenExrExtension(std::string_view extension)
		{
			constexpr std::string_view Exr = ".exr";
			return std::ranges::equal(extension, Exr, [](char character, char lower)
			{
				return (character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character) == lower;
			});
		}

		// stb_image's reason for its last failure on this thread (its failure reason is thread-local).
		[[nodiscard]] static std::string_view GetStbFailureReason()
		{
			const char* reason = stbi_failure_reason();
			return reason != nullptr ? std::string_view(reason) : std::string_view("unknown reason");
		}

		// The settings object of an import as the registered struct (null: the defaults); the registry enforces the field
		// ranges. Errors: Validation for settings the struct rejects; InvalidState for a registry without
		// EnvironmentImportSettings.
		[[nodiscard]] static Result<EnvironmentImportSettings> ReadSettings(const Json& settings, const TypeRegistry& registry)
		{
			EnvironmentImportSettings result;
			if (settings.is_null())
				return result;
			const StructInfo* type = registry.FindStruct<EnvironmentImportSettings>();
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no EnvironmentImportSettings (RegisterAssetPipelineTypes was not called)");
			ReadContext context;
			context.Strict = true;
			ENGINE_TRY(type->FromJson(&result, JsonReader(settings), context));
			return result;
		}

		// Decodes a Radiance .hdr (stbi_loadf from memory, §8.6 step 1) into the bake's input: an equirectangular image twice as
		// wide as high, at most MaxEquirectWidth texels wide, every value finite and non-negative. The size is checked from the
		// header before any texel is decoded. Errors: ImportFailed, naming the first offending texel for bad values.
		[[nodiscard]] static Result<EnvironmentBakeInput> DecodeRadiance(std::span<const std::byte> encoded, std::string_view name)
		{
			if (encoded.empty())
				return MakeError(ErrorCode::ImportFailed, "'{}' is empty", name);
			if (encoded.size() > static_cast<size_t>(INT_MAX))
				return MakeError(ErrorCode::ImportFailed, "'{}' is {} bytes, too large to decode", name, encoded.size());
			const auto* data = reinterpret_cast<const stbi_uc*>(encoded.data());
			const int length = static_cast<int>(encoded.size());
			// stbi_loadf also decodes LDR images (converted to linear floats): only a Radiance header is an environment.
			if (stbi_is_hdr_from_memory(data, length) == 0)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is not a Radiance HDR image (no #?RADIANCE header)", name))
						.WithHint("environments import Radiance .hdr equirectangular images"));
			}
			int width = 0;
			int height = 0;
			int channels = 0;
			if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0)
				return MakeError(ErrorCode::ImportFailed, "'{}' is not a valid Radiance HDR image: {}", name, GetStbFailureReason());
			if (width <= 0 || height <= 0 || width != 2 * height)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' is {}x{} texels; an equirectangular environment is exactly twice as wide as it is high", name, width, height))
						.WithHint("use a 2:1 equirectangular (latitude-longitude) HDRI"));
			}
			if (static_cast<uint32_t>(width) > MaxEquirectWidth)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("'{}' is {}x{} texels; environments are limited to {} texels in width", name, width, height, MaxEquirectWidth))
						.WithHint("use the 8k or a smaller variant of the HDRI"));
			}

			const std::unique_ptr<float, StbFloatDeleter> texels(stbi_loadf_from_memory(data, length, &width, &height, &channels, RgbChannels));
			if (texels == nullptr)
				return MakeError(ErrorCode::ImportFailed, "could not decode the Radiance HDR image '{}': {}", name, GetStbFailureReason());

			EnvironmentBakeInput input;
			input.Width = static_cast<uint32_t>(width);
			input.Height = static_cast<uint32_t>(height);
			const size_t valueCount = static_cast<size_t>(input.Width) * input.Height * RgbChannels;
			input.Texels.assign(texels.get(), texels.get() + valueCount);
			const auto bad = std::ranges::find_if(input.Texels, [](float value)
			{
				return !std::isfinite(value) || value < 0.0f;
			});
			if (bad != input.Texels.end())
			{
				const size_t index = static_cast<size_t>(bad - input.Texels.begin());
				const size_t texel = index / RgbChannels;
				constexpr std::array<std::string_view, 3> ChannelNames = { "red", "green", "blue" };
				return MakeError(ErrorCode::ImportFailed, "'{}' has a non-finite or negative {} value ({}) at texel ({}, {})", name,
					ChannelNames[index % RgbChannels], *bad, texel % input.Width, texel / input.Width);
			}
			return input;
		}

	}

	std::span<const std::string_view> EnvironmentImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 2> Extensions = { ".hdr", ".exr" };
		return Extensions;
	}

	Result<ImportResult> EnvironmentImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		if (Utils::IsOpenExrExtension(context.GetSourcePath().GetExtension()))
		{
			return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is an OpenEXR image: OpenEXR is not supported", sourcePath))
					.WithHint(std::string(Utils::OpenExrHint)));
		}
		ENGINE_TRY_ASSIGN(const EnvironmentImportSettings settings, WithContext(Utils::ReadSettings(context.GetSettings(), context.GetRegistry()), std::format("while reading the import settings of '{}'", sourcePath)));
		ENGINE_TRY_ASSIGN(EnvironmentBakeInput input, Utils::DecodeRadiance(context.GetSourceBytes(), sourcePath));
		input.ClampLuminance = settings.ClampLuminance;
		input.ClampLuminanceMax = settings.MaxLuminance;

		// The bake needs a GPU (§8.6). An editor without one is served a cooked bake under the same cache key before the
		// importer is ever called (EditorAssetManager, §7.4), so reaching this point means no cache holds it.
		IEnvironmentBaker* baker = context.GetEnvironmentBaker();
		if (baker == nullptr)
		{
			return std::unexpected(Error(ErrorCode::Unsupported,
				std::format("'{}' needs a GPU bake, and no cooked bake of it exists: this editor has no graphics device (--renderer none or no Vulkan device)",
					sourcePath))
					.WithHint(std::string(EnvironmentImporter::GpuHint)));
		}
		ENGINE_TRY_ASSIGN(const EnvironmentData environment, WithContext(baker->Bake(input), std::format("while baking '{}'", sourcePath)));
		ENGINE_TRY(WithContext(ValidateEnvironmentData(environment), std::format("in the bake of '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{
			.Handle = metadata.Handle,
			.Type = AssetType::Environment,
			.SubAssetKey = {},
			.Cooked = CookEnvironment(environment, Version),
		});
		return result;
	}

	void EnvironmentImporter::RegisterTypes(TypeRegistry& registry)
	{
		registry.Struct<EnvironmentImportSettings>("EnvironmentImportSettings", "How a Radiance .hdr equirectangular image is baked into an environment.")
			.Field("ClampLuminance", &EnvironmentImportSettings::ClampLuminance,
				"Whether texels brighter than MaxLuminance (Rec. 709 luminance) are scaled down to it before baking; off by default, since the bake's "
				"filtered importance sampling already handles bright suns.")
			.Field("MaxLuminance", &EnvironmentImportSettings::MaxLuminance, "The luminance ClampLuminance scales brighter texels down to.",
				{ .Min = Utils::MinClampLuminance, .Max = Utils::MaxClampLuminance });
	}

}
