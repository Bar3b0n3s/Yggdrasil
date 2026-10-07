#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/TextureImporter.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/Image.h"
#include "Support/AssetTestFixture.h"

#include <nlohmann/json.hpp>

#include <cmath>

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

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("TextureImporter: a PNG imports with a full sRGB mip chain" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(16, 8, 5));
			Result<ImportResult> imported = ImportTexture(fixture, "Assets/Wood.png", "Color");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x7e57000000000001ull));
			CHECK(imported->Artifacts.front().SubAssetKey.empty());
			CHECK(imported->Dependencies.empty());
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

		TEST_CASE("TextureImporter: normal maps are linear and renormalized per mip" * doctest::skip(true))
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

		TEST_CASE("TextureImporter: corrupt and oversized images fail with ImportFailed" * doctest::skip(true))
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
		}

		TEST_CASE("TextureImporter: ImportTextureFromMemory matches Import" * doctest::skip(true))
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
	}

}
