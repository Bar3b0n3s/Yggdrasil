#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <stb_image.h>
#include <stb_image_resize2.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace Engine {

	namespace {

		// The image formats an imported texture may come from, identified by their signatures (TGA has none).
		enum class ImageContainer : uint8_t
		{
			Png,
			Jpeg,
			Bmp,
			Tga
		};

		// A decoded image: RGBA8, rows top first, tightly packed.
		struct DecodedImage
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			Buffer Pixels{};
		};

		// Releases pixels stb_image allocated.
		struct StbImageDeleter
		{
			void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
		};

	}

	namespace Utils {

		static constexpr uint32_t RgbaBytesPerPixel = 4;

		[[nodiscard]] static bool StartsWith(std::span<const std::byte> bytes, std::string_view signature)
		{
			return bytes.size() >= signature.size() && std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
		}

		// The container of `encoded` by its signature. stb_image also decodes GIF, PSD, PIC, PNM and Radiance HDR; those are
		// refused here (§7.4 lists PNG, JPEG, TGA and BMP; HDR images are environments, M8), so a renamed file never imports
		// as something else. Anything without a known signature is tried as TGA, which has none.
		[[nodiscard]] static Result<ImageContainer> IdentifyImage(std::span<const std::byte> encoded, std::string_view name)
		{
			static constexpr std::string_view PngSignature("\x89PNG\r\n\x1A\n", 8);
			static constexpr std::string_view JpegSignature("\xFF\xD8\xFF", 3);
			if (StartsWith(encoded, PngSignature))
				return ImageContainer::Png;
			if (StartsWith(encoded, JpegSignature))
				return ImageContainer::Jpeg;
			if (StartsWith(encoded, "BM"))
				return ImageContainer::Bmp;

			struct RefusedFormat
			{
				std::string_view Signature;
				std::string_view Name;
			};
			static constexpr std::array<RefusedFormat, 8> Refused = { {
				{ "GIF87a", "a GIF" },
				{ "GIF89a", "a GIF" },
				{ "8BPS", "a Photoshop (PSD)" },
				{ "#?RADIANCE", "a Radiance HDR" },
				{ "#?RGBE", "a Radiance HDR" },
				{ std::string_view("\x53\x80\xF6\x34", 4), "a Softimage PIC" },
				{ "P5", "a PGM" },
				{ "P6", "a PPM" },
			} };
			for (const RefusedFormat& format : Refused)
			{
				if (StartsWith(encoded, format.Signature))
				{
					return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is {} image; textures import PNG, JPEG, TGA and BMP", name, format.Name))
							.WithHint("convert the image to PNG"));
				}
			}
			return ImageContainer::Tga;
		}

		[[nodiscard]] static std::string_view GetContainerName(ImageContainer container)
		{
			switch (container)
			{
				case ImageContainer::Png:  return "PNG";
				case ImageContainer::Jpeg: return "JPEG";
				case ImageContainer::Bmp:  return "BMP";
				case ImageContainer::Tga:  return "TGA";
			}
			return "image";
		}

		// stb_image's reason for its last failure on this thread (its failure reason is thread-local).
		[[nodiscard]] static std::string_view GetStbFailureReason()
		{
			const char* reason = stbi_failure_reason();
			return reason != nullptr ? std::string_view(reason) : std::string_view("unknown reason");
		}

		// Decodes `encoded` to RGBA8 (grey and grey-alpha replicate, missing alpha becomes 255, 16-bit channels keep their
		// high byte). The size is checked from the header before any texel is decoded.
		[[nodiscard]] static Result<DecodedImage> DecodeImage(std::span<const std::byte> encoded, std::string_view name)
		{
			if (encoded.empty())
				return MakeError(ErrorCode::ImportFailed, "'{}' is empty", name);
			if (encoded.size() > static_cast<size_t>(INT_MAX))
				return MakeError(ErrorCode::ImportFailed, "'{}' is {} bytes, too large to decode", name, encoded.size());
			ENGINE_TRY_ASSIGN(const ImageContainer container, IdentifyImage(encoded, name));

			const auto* data = reinterpret_cast<const stbi_uc*>(encoded.data());
			const int length = static_cast<int>(encoded.size());
			int width = 0;
			int height = 0;
			int channels = 0;
			if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0)
			{
				return MakeError(ErrorCode::ImportFailed, "'{}' is not a valid PNG, JPEG, TGA or BMP image ({} header: {})", name,
					GetContainerName(container), GetStbFailureReason());
			}
			if (width <= 0 || height <= 0)
				return MakeError(ErrorCode::ImportFailed, "'{}' declares {}x{} pixels; a texture needs at least one", name, width, height);
			if (static_cast<uint32_t>(width) > MaxTextureDimension || static_cast<uint32_t>(height) > MaxTextureDimension)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' is {}x{} pixels; textures are limited to {} pixels in each dimension", name, width, height, MaxTextureDimension)).WithHint("scale the image down"));
			}

			const std::unique_ptr<stbi_uc, StbImageDeleter> pixels(
				stbi_load_from_memory(data, length, &width, &height, &channels, static_cast<int>(RgbaBytesPerPixel)));
			if (pixels == nullptr)
				return MakeError(ErrorCode::ImportFailed, "could not decode the {} image '{}': {}", GetContainerName(container), name, GetStbFailureReason());

			DecodedImage image;
			image.Width = static_cast<uint32_t>(width);
			image.Height = static_cast<uint32_t>(height);
			const auto* texels = reinterpret_cast<const std::byte*>(pixels.get());
			image.Pixels.assign(texels, texels + static_cast<size_t>(image.Width) * image.Height * RgbaBytesPerPixel);
			return image;
		}

		// Maps every texel of a generated normal-map level back to a unit vector (decoded as value / 255 * 2 - 1), which
		// filtering shortened; a vector that averaged out to zero becomes the flat normal (0, 0, 1). Alpha is unchanged.
		static void RenormalizeNormals(std::span<std::byte> pixels)
		{
			const auto decode = [](std::byte value)
			{
				return static_cast<float>(std::to_integer<uint8_t>(value)) / 255.0f * 2.0f - 1.0f;
			};
			const auto encode = [](float component)
			{
				const float scaled = std::floor((component * 0.5f + 0.5f) * 255.0f + 0.5f);
				return static_cast<std::byte>(static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f)));
			};
			for (size_t offset = 0; offset + RgbaBytesPerPixel <= pixels.size(); offset += RgbaBytesPerPixel)
			{
				float x = decode(pixels[offset]);
				float y = decode(pixels[offset + 1]);
				float z = decode(pixels[offset + 2]);
				const float length = std::sqrt(x * x + y * y + z * z);
				if (length > 1e-6f)
				{
					x /= length;
					y /= length;
					z /= length;
				}
				else
				{
					x = 0.0f;
					y = 0.0f;
					z = 1.0f;
				}
				pixels[offset] = encode(x);
				pixels[offset + 1] = encode(y);
				pixels[offset + 2] = encode(z);
			}
		}

		// Writes level `previous` (width x height) halved into `next` (nextWidth x nextHeight, each side max(1, size / 2))
		// with stb_image_resize2's box filter, which averages 2x2 blocks exactly for even sizes: in linear light with alpha
		// weighting for colour (sRGB-correct, §7.4), on the stored values without alpha weighting for data and normals.
		[[nodiscard]] static Status DownsampleLevel(std::span<const std::byte> previous, uint32_t width, uint32_t height, std::span<std::byte> next,
			uint32_t nextWidth, uint32_t nextHeight, TextureUsage usage)
		{
			const bool color = usage == TextureUsage::Color;
			void* resized = stbir_resize(previous.data(), static_cast<int>(width), static_cast<int>(height), static_cast<int>(width * RgbaBytesPerPixel),
				next.data(), static_cast<int>(nextWidth), static_cast<int>(nextHeight), static_cast<int>(nextWidth * RgbaBytesPerPixel),
				color ? STBIR_RGBA : STBIR_4CHANNEL, color ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_BOX);
			if (resized == nullptr)
				return MakeError(ErrorCode::ImportFailed, "stb_image_resize2 could not make a {}x{} mip level", nextWidth, nextHeight);
			if (usage == TextureUsage::NormalMap)
				RenormalizeNormals(next);
			return {};
		}

		// The settings object of an import as the registered struct (null: the defaults). Errors: Validation for settings
		// the struct rejects; InvalidState for a registry without TextureImportSettings.
		[[nodiscard]] static Result<TextureImportSettings> ReadSettings(const Json& settings, const TypeRegistry& registry)
		{
			TextureImportSettings result;
			if (settings.is_null())
				return result;
			const StructInfo* type = registry.FindStruct<TextureImportSettings>();
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no TextureImportSettings (RegisterAssetPipelineTypes was not called)");
			ReadContext context;
			context.Strict = true;
			ENGINE_TRY(type->FromJson(&result, JsonReader(settings), context));
			return result;
		}

	}

	std::span<const std::string_view> TextureImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 5> Extensions = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
		return Extensions;
	}

	Result<ImportResult> TextureImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		ENGINE_TRY_ASSIGN(const TextureImportSettings settings, WithContext(Utils::ReadSettings(context.GetSettings(), context.GetRegistry()), std::format("while reading the import settings of '{}'", sourcePath)));
		ENGINE_TRY_ASSIGN(Buffer cooked, ImportTextureFromMemory(context.GetSourceBytes(), settings, sourcePath));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Texture, .SubAssetKey = {}, .Cooked = std::move(cooked) });
		return result;
	}

	Result<Buffer> TextureImporter::ImportTextureFromMemory(std::span<const std::byte> encoded, const TextureImportSettings& settings,
		std::string_view name)
	{
		ENGINE_TRY_ASSIGN(DecodedImage image, Utils::DecodeImage(encoded, name));

		TextureData texture;
		texture.Format = settings.Usage == TextureUsage::Color ? TextureFormat::RGBA8Srgb : TextureFormat::RGBA8Unorm;
		texture.Width = image.Width;
		texture.Height = image.Height;
		const uint32_t levelCount = settings.GenerateMips ? ComputeFullMipCount(image.Width, image.Height) : 1;
		uint64_t totalSize = 0;
		for (uint32_t level = 0; level < levelCount; ++level)
		{
			const uint32_t width = std::max(1u, image.Width >> level);
			const uint32_t height = std::max(1u, image.Height >> level);
			const uint64_t size = static_cast<uint64_t>(width) * height * Utils::RgbaBytesPerPixel;
			texture.Mips.push_back({ .Width = width, .Height = height, .Offset = totalSize, .Size = size });
			totalSize += size;
		}

		// Pixels is allocated once, so a level being made reads the one above it in place. The base level keeps the decoded
		// texels unchanged; each further level is made from the one before it.
		texture.Pixels.reserve(static_cast<size_t>(totalSize));
		texture.Pixels.assign(image.Pixels.begin(), image.Pixels.end());
		image.Pixels = Buffer();
		for (uint32_t level = 1; level < levelCount; ++level)
		{
			const TextureMip& above = texture.Mips[level - 1];
			const TextureMip& mip = texture.Mips[level];
			texture.Pixels.resize(static_cast<size_t>(mip.Offset + mip.Size));
			const std::span<const std::byte> previous(texture.Pixels.data() + static_cast<size_t>(above.Offset), static_cast<size_t>(above.Size));
			const std::span<std::byte> next(texture.Pixels.data() + static_cast<size_t>(mip.Offset), static_cast<size_t>(mip.Size));
			ENGINE_TRY(Utils::DownsampleLevel(previous, above.Width, above.Height, next, mip.Width, mip.Height, settings.Usage));
		}
		return CookTexture(texture, Version);
	}

	void TextureImporter::RegisterTypes(TypeRegistry& registry)
	{
		registry.Enum<TextureUsage>("TextureUsage", "How a texture's texels are interpreted.")
			.Entry(TextureUsage::Color, "Color", "Colour data (base colour, emissive): sRGB, mips filtered in linear light.")
			.Entry(TextureUsage::Linear, "Linear", "Linear data (metallic-roughness, occlusion, masks): mips filtered on the stored values.")
			.Entry(TextureUsage::NormalMap, "NormalMap", "Tangent-space normals: linear, every generated mip renormalized to unit vectors.");

		registry.Struct<TextureImportSettings>("TextureImportSettings", "How a PNG, JPEG, TGA or BMP image is imported as a texture.")
			.Field("Usage", &TextureImportSettings::Usage, "How the texels are interpreted: sRGB colour, linear data or a tangent-space normal map.")
			.Field("GenerateMips", &TextureImportSettings::GenerateMips, "Whether the full mip chain down to 1x1 is generated; otherwise only the base level.");
	}

}
