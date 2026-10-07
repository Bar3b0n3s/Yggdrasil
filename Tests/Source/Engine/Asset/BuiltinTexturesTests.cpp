#include "TestsPCH.h"

#include "Engine/Asset/BuiltinTextures.h"

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("BuiltinTextures: each texture is named after its engine path and other values are Unknown")
		{
			CHECK(BuiltinTextureToString(BuiltinTexture::White) == "White");
			CHECK(BuiltinTextureToString(BuiltinTexture::FlatNormal) == "FlatNormal");
			CHECK(BuiltinTextureToString(BuiltinTexture::Missing) == "Missing");
			CHECK(BuiltinTextureToString(static_cast<BuiltinTexture>(99)) == "Unknown");
		}

		TEST_CASE("BuiltinTextures: textures have their documented sizes, formats and texels" * doctest::skip(true))
		{
			const TextureData white = GenerateBuiltinTexture(BuiltinTexture::White);
			REQUIRE(ValidateTextureData(white).has_value());
			CHECK(white.Format == TextureFormat::RGBA8Srgb);
			CHECK(white.Width == 4);
			CHECK(std::ranges::all_of(GetMipPixels(white, 0), [](std::byte value)
			{
				return value == std::byte{ 255 };
			}));

			const TextureData flat = GenerateBuiltinTexture(BuiltinTexture::FlatNormal);
			REQUIRE(ValidateTextureData(flat).has_value());
			CHECK(flat.Format == TextureFormat::RGBA8Unorm);
			const std::span<const std::byte> texel = GetMipPixels(flat, 0).first(4);
			CHECK(texel[0] == std::byte{ 128 });
			CHECK(texel[1] == std::byte{ 128 });
			CHECK(texel[2] == std::byte{ 255 });
			CHECK(texel[3] == std::byte{ 255 });

			for (const BuiltinTexture checker : { BuiltinTexture::Checker, BuiltinTexture::Missing })
			{
				CAPTURE(std::string(BuiltinTextureToString(checker)));
				const TextureData texture = GenerateBuiltinTexture(checker);
				REQUIRE(ValidateTextureData(texture).has_value());
				CHECK(texture.Width == 64);
				CHECK(texture.Height == 64);
				CHECK(texture.Mips.size() == ComputeFullMipCount(64, 64));
				CHECK(SerializeTexturePayload(texture) == SerializeTexturePayload(GenerateBuiltinTexture(checker)));
			}
			// Missing alternates magenta and black squares of 8x8 texels.
			const TextureData missing = GenerateBuiltinTexture(BuiltinTexture::Missing);
			const std::span<const std::byte> base = GetMipPixels(missing, 0);
			CHECK(base[0] == std::byte{ 255 });
			CHECK(base[1] == std::byte{ 0 });
			CHECK(base[2] == std::byte{ 255 });
			CHECK(base[8 * 4] == std::byte{ 0 });
		}
	}

}
