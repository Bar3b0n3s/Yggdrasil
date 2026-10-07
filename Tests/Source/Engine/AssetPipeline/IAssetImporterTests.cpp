#include "TestsPCH.h"

#include "Engine/AssetPipeline/IAssetImporter.h"

#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/Hash.h"
#include "Support/AssetTestFixture.h"

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ImportContext: dependency reads and lookups are recorded sorted and once" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Models/Data/B.bin", "bbbb");
			fixture.WriteProjectText("Assets/Models/Data/A.bin", "aaaa");
			const Buffer source = fixture.ReadProjectFile("Assets/Models/Data/A.bin");
			const std::vector<ImportAssetLookupEntry> assets = {
				{ .SourcePath = fixture.ProjectPath("Assets/Models/Wood.png"), .Handle = AssetHandle(0x55), .Kind = AssetMetaKind::Asset, .Type = AssetType::Texture, .Owner = AssetHandle() },
			};
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath("Assets/Models/Track.gltf"),
				.SourceBytes = source,
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = assets,
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			REQUIRE(context.ReadDependency(fixture.ProjectPath("Assets/Models/Data/B.bin")).has_value());
			REQUIRE(context.ReadDependency(fixture.ProjectPath("Assets/Models/Data/A.bin")).has_value());
			REQUIRE(context.ReadDependency(fixture.ProjectPath("Assets/Models/Data/B.bin")).has_value());
			const std::vector<ImportDependencyRead> reads = context.GetDependencyReads();
			REQUIRE(reads.size() == 2);
			CHECK(reads[0].Path == fixture.ProjectPath("Assets/Models/Data/A.bin"));
			CHECK(reads[0].Hash == XXH64(std::string_view("aaaa")));
			CHECK(reads[1].Path == fixture.ProjectPath("Assets/Models/Data/B.bin"));

			const std::optional<ImportAssetLookupEntry> wood = context.FindAsset(fixture.ProjectPath("Assets/Models/Wood.png"));
			REQUIRE(wood.has_value());
			CHECK(wood->Handle == AssetHandle(0x55));
			CHECK_FALSE(context.FindAsset(fixture.ProjectPath("Assets/Models/Other.png")).has_value());
			const std::vector<ImportAssetLookup> lookups = context.GetLookups();
			REQUIRE(lookups.size() == 2);
			CHECK(lookups[0].Path == fixture.ProjectPath("Assets/Models/Other.png"));
			CHECK_FALSE(lookups[0].Found.has_value());
			CHECK(lookups[1].Found.has_value());
		}

		TEST_CASE("ImportContext: reads outside the source's asset root are rejected" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Shared/Utils.bin", "shared");
			fixture.WriteProjectText("Library/Secret.bin", "secret");
			fixture.WriteProjectText("Automation/Secret.bin", "secret");
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath("Assets/Models/Track.gltf"),
				.SourceBytes = {},
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			// Anywhere under project://Assets is allowed (a script's required modules, §7.4); importers add stricter rules.
			REQUIRE(context.ReadDependency(fixture.ProjectPath("Assets/Shared/Utils.bin")).has_value());
			// Outside the Assets folder, or in another scheme, is not.
			for (const char* outsidePath : { "Library/Secret.bin", "Automation/Secret.bin" })
			{
				CAPTURE(std::string(outsidePath));
				Result<Buffer> outside = context.ReadDependency(fixture.ProjectPath(outsidePath));
				REQUIRE_FALSE(outside.has_value());
				CHECK(outside.error().GetCode() == ErrorCode::InvalidArgument);
			}
			Result<Buffer> otherScheme = context.ReadDependency(Test::ParseVfsPath("cache://Assets/Models/Track.bin"));
			REQUIRE_FALSE(otherScheme.has_value());
			CHECK(otherScheme.error().GetCode() == ErrorCode::InvalidArgument);
			Result<Buffer> missing = context.ReadDependency(fixture.ProjectPath("Assets/Models/Missing.bin"));
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			// Only the successful read is recorded.
			REQUIRE(context.GetDependencyReads().size() == 1);
			CHECK(context.GetDependencyReads().front().Path == fixture.ProjectPath("Assets/Shared/Utils.bin"));
		}

		TEST_CASE("IAssetImporter: CanImport matches the extensions ignoring ASCII case" * doctest::skip(true))
		{
			const TextureImporter importer;
			CHECK(importer.CanImport(".png"));
			CHECK(importer.CanImport(".PNG"));
			CHECK(importer.CanImport(".Jpeg"));
			CHECK_FALSE(importer.CanImport("png"));
			CHECK_FALSE(importer.CanImport(".gltf"));
			CHECK_FALSE(importer.CanImport(""));
		}
	}

}
