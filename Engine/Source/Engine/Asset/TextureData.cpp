#include "EnginePCH.h"
#include "Engine/Asset/TextureData.h"

// M6 contract stub (Roadmap rule 3): stream B (texture, material and font importers) implements the texture payload.

namespace Engine {

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

	std::span<const std::byte> GetMipPixels(const TextureData& /*texture*/, uint32_t /*level*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status ValidateTextureData(const TextureData& /*texture*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValidateTextureData is an M6 contract stub");
	}

	Buffer SerializeTexturePayload(const TextureData& /*texture*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<TextureData> DeserializeTexturePayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "DeserializeTexturePayload is an M6 contract stub");
	}

	Buffer CookTexture(const TextureData& /*texture*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<TextureData>> LoadCookedTexture(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "LoadCookedTexture is an M6 contract stub");
	}

}
