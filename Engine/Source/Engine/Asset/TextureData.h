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
#include <vector>

// Texture CPU data and its cooked payload (Architecture §6.8, §7.4). Produced by TextureImporter (PNG, JPEG, TGA, BMP,
// with CPU mips), by GltfImporter (texture sub-assets) and by the built-in procedural textures (BuiltinTextures.h);
// uploaded by Renderer/GpuResourceCache through the M5 upload paths (HostImageUpload).

namespace Engine {

	// Pixel formats of cooked textures. The values are persisted in the payload: append only. There is no GPU texture
	// compression in v1 (§1.2).
	enum class TextureFormat : uint8_t
	{
		RGBA8Unorm = 1, // linear data: normal maps, metallic-roughness, occlusion, masks
		RGBA8Srgb = 2,  // colour data: base colour, emissive (§8.5)
		R8Unorm = 3     // single channel: the blue-noise texture (M8, §8.4), masks
	};

	// "RGBA8Unorm", "RGBA8Srgb", "R8Unorm"; "Unknown" otherwise.
	[[nodiscard]] std::string_view TextureFormatToString(TextureFormat format);

	// 4 for the RGBA8 formats, 1 for R8Unorm, 0 for a value outside the enumeration.
	[[nodiscard]] constexpr uint32_t GetTextureBytesPerPixel(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::RGBA8Unorm:
			case TextureFormat::RGBA8Srgb:  return 4;
			case TextureFormat::R8Unorm:    return 1;
		}
		return 0;
	}

	// The largest width or height a texture may have (§7.4: "> 16384 px ... is an error").
	inline constexpr uint32_t MaxTextureDimension = 16384;

	// The number of levels of a full mip chain down to 1x1: floor(log2(max(width, height))) + 1, so one zero dimension
	// counts as 1 (ComputeFullMipCount(0, 7) == 3); 0 only when both dimensions are zero. A texture with a zero dimension
	// is invalid anyway (ValidateTextureData).
	[[nodiscard]] constexpr uint32_t ComputeFullMipCount(uint32_t width, uint32_t height)
	{
		uint32_t largest = width > height ? width : height;
		uint32_t count = 0;
		while (largest != 0)
		{
			++count;
			largest >>= 1;
		}
		return count;
	}

	// One mip level: its size and where its texels lie in TextureData::Pixels (rows top first, tightly packed:
	// Width * bytes per pixel per row).
	struct TextureMip
	{
		uint32_t Width = 0;  // max(1, base width >> level)
		uint32_t Height = 0; // max(1, base height >> level)
		uint64_t Offset = 0; // the sum of the sizes of the levels before it
		uint64_t Size = 0;   // Width * Height * bytes per pixel

		bool operator==(const TextureMip&) const = default;
	};

	// A loaded 2D texture (AssetType::Texture). Plain data; immutable once loaded (AssetRef<TextureData>).
	struct TextureData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Texture;
		// The cooked payload layout below (CookedHeader::FormatVersion).
		static constexpr uint16_t FormatVersion = 1;

		TextureData()
			: Asset(StaticType)
		{
		}

		TextureFormat Format = TextureFormat::RGBA8Unorm;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<TextureMip> Mips{}; // level 0 first; either the base level alone or the full chain to 1x1
		Buffer Pixels{};                // every level, in order
	};

	// The texels of mip `level` (asserted < Mips.size()); views `texture.Pixels`.
	[[nodiscard]] std::span<const std::byte> GetMipPixels(const TextureData& texture, uint32_t level);

	// Checks: a known format; 1 to MaxTextureDimension in both dimensions; one level or the full chain, each level's size
	// derived from the base size and the format, offsets contiguous from 0, and Pixels exactly the sum of the level sizes.
	// Pure. Errors: Validation naming the first violation.
	[[nodiscard]] Status ValidateTextureData(const TextureData& texture);

	// The cooked texture payload (FormatVersion 1), little-endian:
	//     uint8 Format; uint8 Reserved[3] (0); uint32 Width; uint32 Height; uint32 MipCount;
	//     MipCount x { uint32 Width; uint32 Height; uint64 Size; }
	//     the texels of every level, in order (the sum of the sizes)
	// Asserts ValidateTextureData. Pure; identical textures give identical bytes.
	[[nodiscard]] Buffer SerializeTexturePayload(const TextureData& texture);

	// Reads a payload written by SerializeTexturePayload, then ValidateTextureData. Sizes are checked against the remaining
	// bytes before anything is allocated, so a corrupt size cannot trigger a huge allocation. Never asserts on data. Errors:
	// Parse for truncation, trailing bytes or non-zero reserved bytes; Validation from ValidateTextureData.
	[[nodiscard]] Result<TextureData> DeserializeTexturePayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload) of `texture`.
	[[nodiscard]] Buffer CookTexture(const TextureData& texture, uint32_t importerVersion);

	// The texture of a cooked artifact. Errors: as ReadCookedArtifact and DeserializeTexturePayload.
	[[nodiscard]] Result<AssetRef<TextureData>> LoadCookedTexture(std::span<const std::byte> cooked);

}
