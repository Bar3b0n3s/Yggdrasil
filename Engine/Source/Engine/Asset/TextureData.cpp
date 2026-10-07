#include "EnginePCH.h"
#include "Engine/Asset/TextureData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <algorithm>
#include <utility>

namespace Engine {

	namespace Utils {

		// The bytes of one mip entry in the payload: uint32 Width, uint32 Height, uint64 Size.
		static constexpr size_t TextureMipEntrySize = 16;
		// The number of reserved bytes after the format byte.
		static constexpr size_t TextureReservedByteCount = 3;

		// The size of level `level` of a `width` x `height` texture: max(1, size >> level) per dimension.
		[[nodiscard]] static uint32_t GetMipDimension(uint32_t size, uint32_t level)
		{
			return std::max(1u, level < 32 ? size >> level : 0u);
		}

	}

	std::string_view TextureFormatToString(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::RGBA8Unorm: return "RGBA8Unorm";
			case TextureFormat::RGBA8Srgb:  return "RGBA8Srgb";
			case TextureFormat::R8Unorm:    return "R8Unorm";
		}
		return "Unknown";
	}

	std::span<const std::byte> GetMipPixels(const TextureData& texture, uint32_t level)
	{
		ENGINE_CORE_ASSERT(level < texture.Mips.size(), "GetMipPixels: level {} of a texture with {} mip levels", level, texture.Mips.size());
		if (level >= texture.Mips.size())
			return {};
		const TextureMip& mip = texture.Mips[level];
		ENGINE_CORE_ASSERT(mip.Offset <= texture.Pixels.size() && mip.Size <= texture.Pixels.size() - mip.Offset,
			"GetMipPixels: level {} lies outside the texture's {} bytes", level, texture.Pixels.size());
		if (mip.Offset > texture.Pixels.size() || mip.Size > texture.Pixels.size() - mip.Offset)
			return {};
		return std::span<const std::byte>(texture.Pixels).subspan(static_cast<size_t>(mip.Offset), static_cast<size_t>(mip.Size));
	}

	Status ValidateTextureData(const TextureData& texture)
	{
		const uint32_t bytesPerPixel = GetTextureBytesPerPixel(texture.Format);
		if (bytesPerPixel == 0)
			return MakeError(ErrorCode::Validation, "unknown texture format {}", std::to_underlying(texture.Format));
		if (texture.Width == 0 || texture.Height == 0 || texture.Width > MaxTextureDimension || texture.Height > MaxTextureDimension)
		{
			return MakeError(ErrorCode::Validation, "a texture of {}x{} texels is outside 1 to {} texels in each dimension", texture.Width,
				texture.Height, MaxTextureDimension);
		}

		const uint32_t fullCount = ComputeFullMipCount(texture.Width, texture.Height);
		if (texture.Mips.size() != 1 && texture.Mips.size() != fullCount)
		{
			return MakeError(ErrorCode::Validation, "a {}x{} texture has {} mip levels; it needs 1 or the full chain of {}", texture.Width,
				texture.Height, texture.Mips.size(), fullCount);
		}

		uint64_t offset = 0;
		for (uint32_t level = 0; level < texture.Mips.size(); ++level)
		{
			const TextureMip& mip = texture.Mips[level];
			const uint32_t width = Utils::GetMipDimension(texture.Width, level);
			const uint32_t height = Utils::GetMipDimension(texture.Height, level);
			if (mip.Width != width || mip.Height != height)
			{
				return MakeError(ErrorCode::Validation, "mip level {} is {}x{}; a {}x{} texture's level {} is {}x{}", level, mip.Width, mip.Height,
					texture.Width, texture.Height, level, width, height);
			}
			const uint64_t size = static_cast<uint64_t>(width) * height * bytesPerPixel;
			if (mip.Size != size)
				return MakeError(ErrorCode::Validation, "mip level {} holds {} bytes; {}x{} {} texels take {}", level, mip.Size, width, height,
					TextureFormatToString(texture.Format), size);
			if (mip.Offset != offset)
				return MakeError(ErrorCode::Validation, "mip level {} starts at byte {}; the levels before it end at byte {}", level, mip.Offset, offset);
			offset += size;
		}
		if (texture.Pixels.size() != offset)
			return MakeError(ErrorCode::Validation, "the texture holds {} bytes of texels; its mip levels take {}", texture.Pixels.size(), offset);
		return {};
	}

	Buffer SerializeTexturePayload(const TextureData& texture)
	{
		ENGINE_CORE_ASSERT(ValidateTextureData(texture).has_value(), "SerializeTexturePayload needs a valid texture");
		BinaryWriter writer;
		writer.WriteU8(std::to_underlying(texture.Format));
		for (size_t index = 0; index < Utils::TextureReservedByteCount; ++index)
			writer.WriteU8(0);
		writer.WriteU32(texture.Width);
		writer.WriteU32(texture.Height);
		writer.WriteU32(static_cast<uint32_t>(texture.Mips.size()));
		for (const TextureMip& mip : texture.Mips)
		{
			writer.WriteU32(mip.Width);
			writer.WriteU32(mip.Height);
			writer.WriteU64(mip.Size);
		}
		writer.WriteBytes(texture.Pixels);
		return writer.TakeBuffer();
	}

	Result<TextureData> DeserializeTexturePayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		TextureData texture;
		ENGINE_TRY_ASSIGN(const uint8_t format, reader.ReadU8());
		texture.Format = static_cast<TextureFormat>(format);
		for (size_t index = 0; index < Utils::TextureReservedByteCount; ++index)
		{
			ENGINE_TRY_ASSIGN(const uint8_t reserved, reader.ReadU8());
			if (reserved != 0)
				return MakeError(ErrorCode::Parse, "texture payload: reserved byte {} is {}, not 0", index, reserved);
		}
		ENGINE_TRY_ASSIGN(texture.Width, reader.ReadU32());
		ENGINE_TRY_ASSIGN(texture.Height, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t mipCount, reader.ReadU32());
		if (mipCount > reader.GetRemaining() / Utils::TextureMipEntrySize)
		{
			return MakeError(ErrorCode::Parse, "texture payload: {} mip entries do not fit in the {} bytes left at offset {}", mipCount,
				reader.GetRemaining(), reader.GetPosition());
		}

		// The texels follow the entries. Each level's size is checked against the texel bytes not yet claimed by the levels
		// before it, so the running offset never overflows and a corrupt size cannot claim more than the payload holds.
		const uint64_t texelBytes = reader.GetRemaining() - static_cast<uint64_t>(mipCount) * Utils::TextureMipEntrySize;
		texture.Mips.reserve(mipCount);
		uint64_t offset = 0;
		for (uint32_t level = 0; level < mipCount; ++level)
		{
			TextureMip mip;
			ENGINE_TRY_ASSIGN(mip.Width, reader.ReadU32());
			ENGINE_TRY_ASSIGN(mip.Height, reader.ReadU32());
			ENGINE_TRY_ASSIGN(mip.Size, reader.ReadU64());
			if (mip.Size > texelBytes - offset)
			{
				return MakeError(ErrorCode::Parse, "texture payload: mip level {} declares {} bytes, more than the {} bytes of texels left", level,
					mip.Size, texelBytes - offset);
			}
			mip.Offset = offset;
			texture.Mips.push_back(mip);
			offset += mip.Size;
		}
		if (offset != reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "texture payload: the mip levels take {} bytes but {} bytes follow the entries at offset {}", offset,
				reader.GetRemaining(), reader.GetPosition());
		}

		ENGINE_TRY_ASSIGN(const std::span<const std::byte> texels, reader.ReadBytes(static_cast<size_t>(offset)));
		texture.Pixels.assign(texels.begin(), texels.end());
		ENGINE_TRY(ValidateTextureData(texture));
		return texture;
	}

	Buffer CookTexture(const TextureData& texture, uint32_t importerVersion)
	{
		return WriteCookedArtifact(AssetType::Texture, TextureData::FormatVersion, importerVersion, SerializeTexturePayload(texture));
	}

	Result<AssetRef<TextureData>> LoadCookedTexture(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::Texture, TextureData::FormatVersion));
		ENGINE_TRY_ASSIGN(TextureData texture, DeserializeTexturePayload(view.Payload));
		return AssetRef<TextureData>(CreateRef<TextureData>(std::move(texture)));
	}

}
