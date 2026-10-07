#include "TestsPCH.h"

#include "Engine/AssetPipeline/IAssetImporter.h"

#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Support/AssetTestFixture.h"

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ImportContext: dependency reads and lookups are recorded sorted and once")
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

		TEST_CASE("ImportContext: reads outside the source's asset root are rejected")
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

		TEST_CASE("ImportContext: a built-in reads anywhere under engine:// and nowhere else")
		{
			Test::AssetTestFixture fixture;
			REQUIRE(fixture.GetVfs().Mount("engine", CreateScope<MemoryMount>()).has_value());
			const VfsPath atlas = Test::ParseVfsPath("engine://Fonts/Data/Atlas.bin");
			REQUIRE(fixture.GetVfs().CreateDirectories(atlas.GetParent()).has_value());
			REQUIRE(fixture.GetVfs().WriteFileAtomic(atlas, AsBytes(std::string_view("atlas"))).has_value());
			fixture.WriteProjectText("Assets/Project.bin", "project");
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = Test::ParseVfsPath("engine://Fonts/Inter-Regular.ttf"),
				.SourceBytes = {},
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			Result<Buffer> read = context.ReadDependency(atlas);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(AsStringView(*read) == "atlas");
			Result<Buffer> project = context.ReadDependency(fixture.ProjectPath("Assets/Project.bin"));
			REQUIRE_FALSE(project.has_value());
			CHECK(project.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(context.GetDependencyReads() == std::vector<ImportDependencyRead>{ { .Path = atlas, .Hash = XXH64(std::string_view("atlas")) } });
		}

		TEST_CASE("IAssetImporter: CanImport matches the extensions ignoring ASCII case")
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
