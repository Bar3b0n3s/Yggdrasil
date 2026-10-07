#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/TextureImporter.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>

namespace Engine {

	namespace {

		Result<ImportResult> ImportTexture(Test::AssetTestFixture& fixture, std::string_view relative, std::string_view usage)
		{
			const VfsPath source = fixture.ProjectPath(relative);
			const Buffer bytes = fixture.ReadProjectFile(relative);
			Json settings = Json::object();
			settings["Usage"] = std::string(usage);
			settings["GenerateMips"] = true;
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x7e57000000000001ull);
			metadata.Type = AssetType::Texture;
			metadata.Importer = std::string(TextureImporter::Id);
			metadata.ImporterVersion = TextureImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = source,
				.SourceBytes = bytes,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			return TextureImporter().Import(context, metadata);
		}

		AssetRef<TextureData> LoadMainTexture(const ImportResult& result)
		{
			REQUIRE(result.Artifacts.size() == 1);
			Result<AssetRef<TextureData>> texture = LoadCookedTexture(result.Artifacts.front().Cooked);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			return std::move(*texture);
		}

		// A committed fixture of Tests/Data/Assets/Textures (written by Tests/Data/Generate/MakeTextureFixtures.py).
		Buffer ReadTextureFixture(std::string_view name)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Textures") / std::string(name));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

		// Imports encoded bytes and loads the cooked texture back.
		AssetRef<TextureData> ImportFromMemory(std::span<const std::byte> encoded, TextureImportSettings settings, std::string_view name)
		{
			Result<Buffer> cooked = TextureImporter::ImportTextureFromMemory(encoded, settings, name);
			REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
			Result<AssetRef<TextureData>> texture = LoadCookedTexture(*cooked);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			return std::move(*texture);
		}

		// The RGBA texel (x, y) of mip level `level`.
		std::array<uint8_t, 4> GetTexel(const TextureData& texture, uint32_t level, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> pixels = GetMipPixels(texture, level);
			const size_t offset = (static_cast<size_t>(y) * texture.Mips[level].Width + x) * 4;
			return { std::to_integer<uint8_t>(pixels[offset]), std::to_integer<uint8_t>(pixels[offset + 1]), std::to_integer<uint8_t>(pixels[offset + 2]),
				std::to_integer<uint8_t>(pixels[offset + 3]) };
		}

		// A two-texel PNG: opaque black, then opaque white.
		Buffer MakeBlackWhitePng()
		{
			Result<Image> image = CreateImage(2, 1, nvrhi::Format::RGBA8_UNORM);
			REQUIRE(image.has_value());
			for (size_t channel = 0; channel < 4; ++channel)
			{
				image->Pixels[channel] = std::byte{ channel == 3 ? uint8_t{ 255 } : uint8_t{ 0 } };
				image->Pixels[4 + channel] = std::byte{ 255 };
			}
			Result<Buffer> png = EncodePng(*image);
			REQUIRE(png.has_value());
			return std::move(*png);
		}

		constexpr TextureImportSettings ColorSettings{ .Usage = TextureUsage::Color, .GenerateMips = true };

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("TextureImporter: a PNG imports with a full sRGB mip chain")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(16, 8, 5));
			Result<ImportResult> imported = ImportTexture(fixture, "Assets/Wood.png", "Color");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x7e57000000000001ull));
			CHECK(imported->Artifacts.front().SubAssetKey.empty());
			CHECK(imported->Artifacts.front().Type == AssetType::Texture);
			CHECK(imported->Dependencies.empty());
			CHECK(imported->Diagnostics.empty());
			const AssetRef<TextureData> texture = LoadMainTexture(*imported);
			CHECK(texture->Format == TextureFormat::RGBA8Srgb);
			CHECK(texture->Width == 16);
			CHECK(texture->Height == 8);
			CHECK(texture->Mips.size() == 5);
			// Mip 0 holds the decoded texels unchanged.
			Result<Image> decoded = DecodePng(fixture.ReadProjectFile("Assets/Wood.png"));
			REQUIRE(decoded.has_value());
			CHECK(std::ranges::equal(GetMipPixels(*texture, 0), decoded->Pixels));
		}

		TEST_CASE("TextureImporter: normal maps are linear and renormalized per mip")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Normal.png", Test::MakeTestPng(8, 8, 9));
			Result<ImportResult> imported = ImportTexture(fixture, "Assets/Normal.png", "NormalMap");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			const AssetRef<TextureData> texture = LoadMainTexture(*imported);
			CHECK(texture->Format == TextureFormat::RGBA8Unorm);
			for (uint32_t level = 1; level < texture->Mips.size(); ++level)
			{
				const std::span<const std::byte> pixels = GetMipPixels(*texture, level);
				for (size_t offset = 0; offset < pixels.size(); offset += 4)
				{
					const auto decode = [&pixels](size_t index)
					{
						return static_cast<float>(std::to_integer<uint8_t>(pixels[index])) / 127.5f - 1.0f;
					};
					const float length = std::sqrt(decode(offset) * decode(offset) + decode(offset + 1) * decode(offset + 1)
						+ decode(offset + 2) * decode(offset + 2));
					CHECK(length == doctest::Approx(1.0f).epsilon(0.02));
				}
			}
		}

		TEST_CASE("TextureImporter: corrupt and oversized images fail with ImportFailed")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Broken.png", "not a png");
			Result<ImportResult> broken = ImportTexture(fixture, "Assets/Broken.png", "Color");
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::ImportFailed);

			fixture.WriteProjectFile("Assets/Wide.png", Test::MakeTestPng(MaxTextureDimension + 1, 1));
			Result<ImportResult> wide = ImportTexture(fixture, "Assets/Wide.png", "Color");
			REQUIRE_FALSE(wide.has_value());
			CHECK(wide.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(wide.error().ToString().find("16384") != std::string::npos);

			const Result<Buffer> truncated = TextureImporter::ImportTextureFromMemory(ReadTextureFixture("Truncated.png"), ColorSettings, "Truncated.png");
			REQUIRE_FALSE(truncated.has_value());
			CHECK(truncated.error().GetCode() == ErrorCode::ImportFailed);
			const Result<Buffer> empty = TextureImporter::ImportTextureFromMemory({}, ColorSettings, "Empty.png");
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetCode() == ErrorCode::ImportFailed);
		}

		TEST_CASE("TextureImporter: GIF, HDR and other formats stb_image could decode are refused")
		{
			for (const std::string_view signature : { std::string_view("GIF89a\x01\x00\x01\x00", 10), std::string_view("#?RADIANCE\n"), std::string_view("P6\n1 1\n255\n") })
			{
				CAPTURE(std::string(signature.substr(0, 2)));
				const Result<Buffer> refused = TextureImporter::ImportTextureFromMemory(AsBytes(signature), ColorSettings, "Renamed.png");
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::ImportFailed);
				CHECK(refused.error().ToString().find("Renamed.png") != std::string::npos);
			}
		}

		TEST_CASE("TextureImporter: ImportTextureFromMemory matches Import")
		{
			Test::AssetTestFixture fixture;
			const Buffer png = Test::MakeTestPng(4, 4, 2);
			fixture.WriteProjectFile("Assets/Small.png", png);
			Result<ImportResult> imported = ImportTexture(fixture, "Assets/Small.png", "Linear");
			REQUIRE(imported.has_value());
			Result<Buffer> direct = TextureImporter::ImportTextureFromMemory(png, { .Usage = TextureUsage::Linear, .GenerateMips = true }, "Small.png");
			REQUIRE(direct.has_value());
			CHECK(*direct == imported->Artifacts.front().Cooked);
		}

		TEST_CASE("TextureImporter: Color mips are filtered in linear light, Linear mips on the stored values")
		{
			const Buffer png = MakeBlackWhitePng();
			const AssetRef<TextureData> color = ImportFromMemory(png, ColorSettings, "BlackWhite.png");
			REQUIRE(color->Mips.size() == 2);
			// The mean of black and white is 0.5 in linear light: 188 in sRGB.
			const std::array<uint8_t, 4> srgb = GetTexel(*color, 1, 0, 0);
			CHECK(srgb[0] >= 186);
			CHECK(srgb[0] <= 189);
			CHECK(srgb[3] == 255);

			const AssetRef<TextureData> linear = ImportFromMemory(png, { .Usage = TextureUsage::Linear, .GenerateMips = true }, "BlackWhite.png");
			CHECK(linear->Format == TextureFormat::RGBA8Unorm);
			const std::array<uint8_t, 4> stored = GetTexel(*linear, 1, 0, 0);
			CHECK(stored[0] >= 127);
			CHECK(stored[0] <= 128);
		}

		TEST_CASE("TextureImporter: without GenerateMips only the base level is cooked")
		{
			const AssetRef<TextureData> texture = ImportFromMemory(Test::MakeTestPng(8, 4, 1), { .Usage = TextureUsage::Color, .GenerateMips = false }, "Flat.png");
			CHECK(texture->Mips.size() == 1);
			CHECK(texture->Pixels.size() == 8 * 4 * 4);
		}

		TEST_CASE("TextureImporter: odd sizes round every mip level down to 1x1")
		{
			const AssetRef<TextureData> texture = ImportFromMemory(ReadTextureFixture("Rgb.png"), ColorSettings, "Rgb.png");
			REQUIRE(texture->Mips.size() == 3);
			CHECK(texture->Mips[1].Width == 3);
			CHECK(texture->Mips[1].Height == 2);
			CHECK(texture->Mips[2].Width == 1);
			CHECK(texture->Mips[2].Height == 1);
			CHECK(GetTexel(*texture, 0, 1, 1) == std::array<uint8_t, 4>{ 48, 82, 18, 255 });
		}

		TEST_CASE("TextureImporter: PNG, BMP and TGA variants decode to the expected RGBA texels")
		{
			const TextureImportSettings flat{ .Usage = TextureUsage::Linear, .GenerateMips = false };

			const AssetRef<TextureData> grey = ImportFromMemory(ReadTextureFixture("Grey.png"), flat, "Grey.png");
			CHECK(GetTexel(*grey, 0, 1, 2) == std::array<uint8_t, 4>{ 153, 153, 153, 255 });

			const AssetRef<TextureData> greyAlpha = ImportFromMemory(ReadTextureFixture("GreyAlpha.png"), flat, "GreyAlpha.png");
			CHECK(GetTexel(*greyAlpha, 0, 2, 1) == std::array<uint8_t, 4>{ 120, 120, 120, 155 });

			// Palette entries red, green, blue, yellow; tRNS alpha 255, 128, 0 and the missing fourth entry opaque.
			const AssetRef<TextureData> palette = ImportFromMemory(ReadTextureFixture("Palette.png"), flat, "Palette.png");
			CHECK(GetTexel(*palette, 0, 0, 0) == std::array<uint8_t, 4>{ 255, 0, 0, 255 });
			CHECK(GetTexel(*palette, 0, 1, 0) == std::array<uint8_t, 4>{ 0, 255, 0, 128 });
			CHECK(GetTexel(*palette, 0, 1, 1) == std::array<uint8_t, 4>{ 0, 0, 255, 0 });
			CHECK(GetTexel(*palette, 0, 3, 0) == std::array<uint8_t, 4>{ 255, 255, 0, 255 });

			// 16-bit channels keep their high byte.
			const AssetRef<TextureData> deep = ImportFromMemory(ReadTextureFixture("Deep.png"), flat, "Deep.png");
			CHECK(GetTexel(*deep, 0, 0, 0) == std::array<uint8_t, 4>{ 4, 16, 255, 255 });
			CHECK(GetTexel(*deep, 0, 2, 1) == std::array<uint8_t, 4>{ 239, 212, 185, 255 });

			const AssetRef<TextureData> bmp = ImportFromMemory(ReadTextureFixture("Solid.bmp"), flat, "Solid.bmp");
			CHECK(bmp->Width == 5);
			CHECK(bmp->Height == 3);
			CHECK(GetTexel(*bmp, 0, 0, 0) == std::array<uint8_t, 4>{ 0, 0, 200, 255 });
			CHECK(GetTexel(*bmp, 0, 4, 2) == std::array<uint8_t, 4>{ 200, 240, 40, 255 });

			// Stored bottom row first (bottom-left origin); rows come out top first.
			const AssetRef<TextureData> tga = ImportFromMemory(ReadTextureFixture("Alpha.tga"), flat, "Alpha.tga");
			CHECK(GetTexel(*tga, 0, 0, 0) == std::array<uint8_t, 4>{ 255, 0, 0, 64 });
			CHECK(GetTexel(*tga, 0, 2, 1) == std::array<uint8_t, 4>{ 95, 200, 200, 192 });

			const AssetRef<TextureData> rle = ImportFromMemory(ReadTextureFixture("Rle.tga"), flat, "Rle.tga");
			CHECK(GetTexel(*rle, 0, 2, 0) == std::array<uint8_t, 4>{ 30, 20, 10, 255 });
			CHECK(GetTexel(*rle, 0, 3, 0) == std::array<uint8_t, 4>{ 60, 50, 40, 255 });
			CHECK(GetTexel(*rle, 0, 3, 1) == std::array<uint8_t, 4>{ 180, 170, 160, 255 });
		}

		TEST_CASE("TextureImporter: baseline JPEGs decode close to their source colours")
		{
			const AssetRef<TextureData> photo = ImportFromMemory(ReadTextureFixture("Photo.jpg"), ColorSettings, "Photo.jpg");
			CHECK(photo->Width == 16);
			CHECK(photo->Height == 16);
			CHECK(photo->Mips.size() == 5);
			// The source texel (x, y) is (15x, 15y, 255 - 7(x + y)); quality 90 keeps a smooth gradient within a few levels.
			for (const auto [x, y] : { std::pair<uint32_t, uint32_t>{ 3, 4 }, std::pair<uint32_t, uint32_t>{ 12, 9 } })
			{
				const std::array<uint8_t, 4> texel = GetTexel(*photo, 0, x, y);
				CHECK(std::abs(static_cast<int>(texel[0]) - static_cast<int>(15 * x)) <= 12);
				CHECK(std::abs(static_cast<int>(texel[1]) - static_cast<int>(15 * y)) <= 12);
				CHECK(std::abs(static_cast<int>(texel[2]) - static_cast<int>(255 - 7 * (x + y))) <= 12);
				CHECK(texel[3] == 255);
			}

			const AssetRef<TextureData> grey = ImportFromMemory(ReadTextureFixture("GreyPhoto.jpg"), ColorSettings, "GreyPhoto.jpg");
			CHECK(grey->Width == 8);
			const std::array<uint8_t, 4> texel = GetTexel(*grey, 0, 5, 5);
			CHECK(texel[0] == texel[1]);
			CHECK(texel[1] == texel[2]);
		}

		TEST_CASE("TextureImporter: every texture fixture imports identically twice in every usage")
		{
			Result<std::vector<std::string>> fixtures = Test::ListTestDataFiles("Assets/Textures", { ".png", ".jpg", ".bmp", ".tga" });
			REQUIRE(fixtures.has_value());
			CHECK(fixtures->size() >= 12);
			for (const std::string& path : *fixtures)
			{
				if (path.ends_with("Truncated.png"))
					continue;
				CAPTURE(path);
				Result<Buffer> encoded = FileSystem::ReadFile(Test::GetTestDataPath(path));
				REQUIRE(encoded.has_value());
				for (const TextureUsage usage : { TextureUsage::Color, TextureUsage::Linear, TextureUsage::NormalMap })
				{
					const TextureImportSettings settings{ .Usage = usage, .GenerateMips = true };
					const Result<Buffer> first = TextureImporter::ImportTextureFromMemory(*encoded, settings, path);
					const Result<Buffer> second = TextureImporter::ImportTextureFromMemory(*encoded, settings, path);
					REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
					REQUIRE(second.has_value());
					CHECK(*first == *second);
					const Result<AssetRef<TextureData>> texture = LoadCookedTexture(*first);
					REQUIRE(texture.has_value());
					CHECK((*texture)->Mips.size() == ComputeFullMipCount((*texture)->Width, (*texture)->Height));
				}
			}
		}

		TEST_CASE("TextureImporter: settings come from the registry and invalid settings fail")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Tile.png", Test::MakeTestPng(4, 4, 3));
			Result<ImportResult> invalid = ImportTexture(fixture, "Assets/Tile.png", "Bump");
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
			CHECK(invalid.error().ToString().find("Tile.png") != std::string::npos);

			const StructInfo* settings = fixture.GetRegistry().FindStruct("TextureImportSettings");
			REQUIRE(settings != nullptr);
			CHECK(settings->FindField("Usage") != nullptr);
			CHECK(settings->FindField("GenerateMips") != nullptr);
			REQUIRE(fixture.GetRegistry().FindEnum<TextureUsage>() != nullptr);
			CHECK(fixture.GetRegistry().FindEnum<TextureUsage>()->GetEntries().size() == 3);
		}
	}

}
