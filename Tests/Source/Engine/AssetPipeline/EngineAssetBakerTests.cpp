#include "TestsPCH.h"

#include "Engine/AssetPipeline/EngineAssetBaker.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Engine {

	namespace {

		// A stand-in for M8's blue-noise generator: a deterministic 4 x 4 linear texture.
		Result<Buffer> GenerateTestNoise()
		{
			return TextureImporter::ImportTextureFromMemory(Test::MakeTestPng(4, 4, 9), { .Usage = TextureUsage::Linear, .GenerateMips = false },
				"TestNoise");
		}

		// The repository's Resources as engine:// (read-only) and a memory enginecache://, on the fixture's services.
		struct BakeEnvironment
		{
			Test::AssetTestFixture Fixture;
			VirtualFileSystem Vfs;

			BakeEnvironment()
			{
				Result<Scope<NativeDirectoryMount>> resources = NativeDirectoryMount::Create(Test::GetRepositoryRoot() / "Resources", MountAccess::ReadOnly);
				REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
				REQUIRE(Vfs.Mount("engine", std::move(*resources)).has_value());
				REQUIRE(Vfs.Mount("enginecache", CreateScope<MemoryMount>()).has_value());
			}

			[[nodiscard]] EngineBakeSpecification GetSpecification()
			{
				return {
					.Vfs = &Vfs,
					.Importers = &Fixture.GetImporters(),
					.Registry = &Fixture.GetRegistry(),
					.Jobs = &Fixture.GetJobSystem(),
					.EnvironmentBaker = nullptr,
					.Generators = {},
				};
			}
		};

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("EngineAssetBaker: bakes the Default font into the engine cache once" * doctest::skip(true))
		{
			BakeEnvironment environment;
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Load(environment.Vfs);
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());

			Result<EngineBakeReport> first = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK(std::ranges::find(first->Baked, BuiltinAssetHandles::DefaultFont) != first->Baked.end());
			CHECK(first->UpToDate.empty());
			// Procedural built-ins are never cached.
			CHECK(std::ranges::find(first->Baked, BuiltinAssetHandles::CubeMesh) == first->Baked.end());

			Result<EngineBakeReport> second = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE(second.has_value());
			CHECK(second->Baked.empty());
			CHECK(std::ranges::find(second->UpToDate, BuiltinAssetHandles::DefaultFont) != second->UpToDate.end());

			const BuiltinAssetEntry* font = catalog->Find(BuiltinAssetHandles::DefaultFont);
			REQUIRE(font != nullptr);
			Result<std::vector<Buffer>> artifacts = GetOrBakeEngineAsset(environment.GetSpecification(), *font);
			REQUIRE(artifacts.has_value());
			REQUIRE(artifacts->size() == 1);
			CHECK(LoadCookedFont(artifacts->front()).has_value());
		}

		TEST_CASE("EngineAssetBaker: entries without an importer in this build are skipped with a warning" * doctest::skip(true))
		{
			BakeEnvironment environment;
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Load(environment.Vfs);
			REQUIRE(catalog.has_value());
			Result<EngineBakeReport> report = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE(report.has_value());
			// The environments wait for EnvironmentImporter (M8).
			const auto skipped = [&report](AssetHandle handle)
			{
				return std::ranges::any_of(report->Skipped, [handle](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Asset == handle && diagnostic.Severity == DiagnosticSeverity::Warning;
				});
			};
			CHECK(skipped(BuiltinAssetHandles::StudioEnvironment));
			CHECK(skipped(BuiltinAssetHandles::SkyEnvironment));
			const BuiltinAssetEntry* studio = catalog->Find(BuiltinAssetHandles::StudioEnvironment);
			REQUIRE(studio != nullptr);
			Result<std::vector<Buffer>> baked = GetOrBakeEngineAsset(environment.GetSpecification(), *studio);
			REQUIRE_FALSE(baked.has_value());
			CHECK(baked.error().GetCode() == ErrorCode::Unsupported);
		}

		TEST_CASE("EngineAssetBaker: generated entries and entry settings are baked under their own keys" * doctest::skip(true))
		{
			BakeEnvironment environment;
			// A catalogue with a Generated entry (the shape of M8's blue noise) and the Default font with non-default settings.
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(R"({
	"Format": "EngineAssets",
	"Version": 1,
	"Assets": [
		{ "Handle": "0000000000000186", "Path": "engine://Textures/TestNoise", "Type": "Texture", "Source": "Generated",
		  "File": "", "Importer": "", "Settings": {}, "Generator": "TestNoise" },
		{ "Handle": "00000000000001c1", "Path": "engine://Fonts/Default", "Type": "Font", "Source": "File",
		  "File": "Fonts/Inter-Regular.ttf", "Importer": "Font", "Settings": {}, "Generator": "" }
	]
}
)");
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			const EngineAssetGenerator generators[] = { { .Id = "TestNoise", .Version = 1, .Generate = &GenerateTestNoise } };

			// Without the generator the entry is skipped with a warning; with it, it is baked once.
			Result<EngineBakeReport> withoutGenerator = BakeEngineAssets(environment.GetSpecification(), *catalog);
			REQUIRE(withoutGenerator.has_value());
			CHECK(std::ranges::any_of(withoutGenerator->Skipped, [](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == AssetHandle(0x186) && diagnostic.Severity == DiagnosticSeverity::Warning;
			}));
			EngineBakeSpecification specification = environment.GetSpecification();
			specification.Generators = generators;
			Result<EngineBakeReport> generated = BakeEngineAssets(specification, *catalog);
			REQUIRE(generated.has_value());
			CHECK(std::ranges::find(generated->Baked, AssetHandle(0x186)) != generated->Baked.end());
			Result<std::vector<Buffer>> noise = GetOrBakeEngineAsset(specification, *catalog->Find(AssetHandle(0x186)));
			REQUIRE_MESSAGE(noise.has_value(), noise.error().ToString());
			CHECK(LoadCookedTexture(noise->front()).has_value());
			Result<EngineBakeReport> again = BakeEngineAssets(specification, *catalog);
			REQUIRE(again.has_value());
			CHECK(again->Baked.empty());

			// Settings are part of the key: the same font with other settings is baked again.
			BuiltinAssetEntry font = *catalog->Find(BuiltinAssetHandles::DefaultFont);
			Result<std::vector<Buffer>> defaults = GetOrBakeEngineAsset(specification, font);
			REQUIRE_MESSAGE(defaults.has_value(), defaults.error().ToString());
			Json settings = Json::object();
			settings["PixelSize"] = 32.0;
			font.Settings = VariantValue(std::move(settings));
			Result<std::vector<Buffer>> resized = GetOrBakeEngineAsset(specification, font);
			REQUIRE_MESSAGE(resized.has_value(), resized.error().ToString());
			CHECK(resized->front() != defaults->front());
		}
	}

}
