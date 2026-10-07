#include "TestsPCH.h"

#include "Engine/Asset/AssetRegistry.h"

#include "Support/AssetTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Engine {

	namespace {

		// The importers the scan knows in these tests: textures and glTF, as the ImporterRegistry describes them.
		std::vector<AssetImporterDescription> MakeImporterDescriptions()
		{
			return {
				{ .Id = "Gltf", .MainType = AssetType::Prefab, .Extensions = { ".gltf", ".glb" }, .Version = 3 },
				{ .Id = "Texture", .MainType = AssetType::Texture, .Extensions = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" }, .Version = 1 },
			};
		}

		// A texture .meta with `handle` and default-like settings.
		std::string MakeTextureMeta(AssetHandle handle)
		{
			AssetMetadata metadata;
			metadata.Handle = handle;
			metadata.Type = AssetType::Texture;
			metadata.Importer = "Texture";
			metadata.ImporterVersion = 1;
			Json settings = Json::object();
			settings["Usage"] = "Color";
			settings["GenerateMips"] = true;
			metadata.Settings = VariantValue(std::move(settings));
			return SerializeAssetMetadata(metadata);
		}

		// A .gltf with one external buffer and one external image, its meta and both dependency metas.
		void WriteGltfWithDependencies(Test::AssetTestFixture& fixture, AssetHandle gltf, AssetHandle buffer, AssetHandle image)
		{
			fixture.WriteProjectText("Assets/Models/Track.gltf", R"({"asset": {"version": "2.0"}, "buffers": [{"uri": "Data/Track.bin", "byteLength": 4}],
				"images": [{"uri": "Textures/Track.png"}]})");
			fixture.WriteProjectText("Assets/Models/Data/Track.bin", "abcd");
			fixture.WriteProjectFile("Assets/Models/Textures/Track.png", Test::MakeTestPng(2, 2));
			AssetMetadata metadata;
			metadata.Handle = gltf;
			metadata.Type = AssetType::Prefab;
			metadata.Importer = "Gltf";
			metadata.ImporterVersion = 1;
			Json settings = Json::object();
			settings["Scale"] = 1.0;
			settings["GenerateMissingTangents"] = true;
			settings["ImportMaterials"] = true;
			settings["MergeMeshes"] = false;
			metadata.Settings = VariantValue(std::move(settings));
			fixture.WriteProjectText("Assets/Models/Track.gltf.meta", SerializeAssetMetadata(metadata));
			fixture.WriteProjectText("Assets/Models/Data/Track.bin.meta", SerializeAssetMetadata(MakeDependencyMetadata(buffer, gltf)));
			fixture.WriteProjectText("Assets/Models/Textures/Track.png.meta", SerializeAssetMetadata(MakeDependencyMetadata(image, gltf)));
		}

		Status ApplyMoves(VirtualFileSystem& vfs, std::span<const AssetFileMove> moves)
		{
			for (const AssetFileMove& move : moves)
			{
				ENGINE_TRY(vfs.CreateDirectories(move.To.GetParent()));
				ENGINE_TRY(vfs.Move(move.From, move.To));
			}
			return {};
		}

		// The error code of a failed result, or nullopt for a success.
		template<typename T>
		std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
		{
			return result.has_value() ? std::nullopt : std::optional<ErrorCode>(result.error().GetCode());
		}

		bool HasDiagnostic(const AssetScanResult& result, std::string_view code, std::string_view path)
		{
			return std::ranges::any_of(result.Diagnostics, [code, path](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Code == code && diagnostic.Path == path;
			});
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetRegistry: moved file keeps its handle")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle handle(0x1111222233334444ull);
			fixture.WriteProjectFile("Assets/Textures/Wood.png", Test::MakeTestPng(4, 4));
			fixture.WriteProjectText("Assets/Textures/Wood.png.meta", MakeTextureMeta(handle));
			const std::vector<AssetImporterDescription> importers = MakeImporterDescriptions();

			AssetRegistry registry;
			REQUIRE(registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers).has_value());
			REQUIRE(registry.FindBySourcePath(fixture.ProjectPath("Assets/Textures/Wood.png")) != nullptr);

			// Move the pair (file + .meta) by hand, as a user would outside the editor, and rescan.
			REQUIRE(fixture.GetVfs().CreateDirectories(fixture.ProjectPath("Assets/Materials/Wood")).has_value());
			REQUIRE(fixture.GetVfs().Move(fixture.ProjectPath("Assets/Textures/Wood.png"), fixture.ProjectPath("Assets/Materials/Wood/Oak.png")).has_value());
			REQUIRE(fixture.GetVfs().Move(fixture.ProjectPath("Assets/Textures/Wood.png.meta"), fixture.ProjectPath("Assets/Materials/Wood/Oak.png.meta")).has_value());
			Result<AssetScanResult> rescanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
			REQUIRE_MESSAGE(rescanned.has_value(), rescanned.error().ToString());
			CHECK(rescanned->Diagnostics.empty());
			CHECK(rescanned->SourcesWithoutMeta.empty());
			const AssetRecord* moved = registry.Find(handle);
			REQUIRE(moved != nullptr);
			CHECK(moved->SourcePath == fixture.ProjectPath("Assets/Materials/Wood/Oak.png"));
			CHECK(registry.Resolve("Assets/Materials/Wood/Oak.png") == handle);
			CHECK_FALSE(registry.Resolve("Assets/Textures/Wood.png").has_value());
		}

		TEST_CASE("AssetRegistry: duplicate handle diagnostic and auto-fix")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle handle(0x1111222233334444ull);
			fixture.WriteProjectFile("Assets/A.png", Test::MakeTestPng(2, 2, 1));
			fixture.WriteProjectText("Assets/A.png.meta", MakeTextureMeta(handle));
			// A copy-pasted pair: the copy's .meta carries the same handle.
			fixture.WriteProjectFile("Assets/B.png", Test::MakeTestPng(2, 2, 2));
			fixture.WriteProjectText("Assets/B.png.meta", MakeTextureMeta(handle));
			const std::vector<AssetImporterDescription> importers = MakeImporterDescriptions();

			AssetRegistry registry;
			Result<AssetScanResult> scanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
			REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
			// Equally long paths: the first in byte order keeps the handle; the other is reported, auto-fixable.
			REQUIRE(scanned->Diagnostics.size() == 1);
			const AssetDiagnostic diagnostic = scanned->Diagnostics.front();
			CHECK(diagnostic.Code == AssetDuplicateHandleCode);
			CHECK(diagnostic.Path == "Assets/B.png.meta");
			CHECK(diagnostic.Severity == DiagnosticSeverity::Error);
			CHECK(diagnostic.AutoFixable);
			REQUIRE(registry.Find(handle) != nullptr);
			CHECK(registry.Find(handle)->SourcePath == fixture.ProjectPath("Assets/A.png"));
			CHECK(registry.FindBySourcePath(fixture.ProjectPath("Assets/B.png")) == nullptr);

			const AssetHandle fresh(0x5555666677778888ull);
			Result<AssetScanFix> fix = registry.PlanFix(diagnostic, fresh);
			REQUIRE_MESSAGE(fix.has_value(), fix.error().ToString());
			CHECK(fix->Kind == AssetScanFixKind::AssignNewHandle);
			CHECK(fix->MetaPath == fixture.ProjectPath("Assets/B.png.meta"));
			fixture.WriteProjectText("Assets/B.png.meta", fix->NewMetaText);

			Result<AssetScanResult> fixed = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
			REQUIRE(fixed.has_value());
			CHECK(fixed->Diagnostics.empty());
			REQUIRE(registry.Find(fresh) != nullptr);
			CHECK(registry.Find(fresh)->SourcePath == fixture.ProjectPath("Assets/B.png"));
			CHECK(registry.Find(handle)->SourcePath == fixture.ProjectPath("Assets/A.png"));

			// A folder copied by a file manager: "Models - Copy/" sorts before "Models/" (' ' < '/'), but the copy never takes
			// the handle from the original, whose path is the shorter one.
			Test::AssetTestFixture copied;
			const AssetHandle track(0x2222333344445555ull);
			copied.WriteProjectFile("Assets/Models/Track.png", Test::MakeTestPng(2, 2, 3));
			copied.WriteProjectText("Assets/Models/Track.png.meta", MakeTextureMeta(track));
			copied.WriteProjectFile("Assets/Models - Copy/Track.png", Test::MakeTestPng(2, 2, 3));
			copied.WriteProjectText("Assets/Models - Copy/Track.png.meta", MakeTextureMeta(track));
			AssetRegistry copiedRegistry;
			Result<AssetScanResult> copiedScan = copiedRegistry.Scan(copied.GetVfs(), copied.ProjectPath("Assets"), importers);
			REQUIRE_MESSAGE(copiedScan.has_value(), copiedScan.error().ToString());
			REQUIRE(copiedScan->Diagnostics.size() == 1);
			CHECK(copiedScan->Diagnostics.front().Code == AssetDuplicateHandleCode);
			CHECK(copiedScan->Diagnostics.front().Path == "Assets/Models - Copy/Track.png.meta");
			REQUIRE(copiedRegistry.Find(track) != nullptr);
			CHECK(copiedRegistry.Find(track)->SourcePath == copied.ProjectPath("Assets/Models/Track.png"));
		}

		TEST_CASE("AssetRegistry: a duplicated handle stays with its registered or last known file")
		{
			const std::vector<AssetImporterDescription> importers = MakeImporterDescriptions();
			const AssetHandle handle(0x3333444455556666ull);

			// Rule 1: a copied sidecar without a source is an orphan and takes no part in duplicate resolution.
			{
				Test::AssetTestFixture fixture;
				fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(2, 2));
				fixture.WriteProjectText("Assets/Wood.png.meta", MakeTextureMeta(handle));
				fixture.WriteProjectText("Assets/Wood.png - Copy.meta", MakeTextureMeta(handle));
				AssetRegistry registry;
				Result<AssetScanResult> scanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
				REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
				REQUIRE(scanned->Diagnostics.size() == 1);
				CHECK(scanned->Diagnostics.front().Code == AssetOrphanMetaCode);
				CHECK(scanned->Diagnostics.front().Path == "Assets/Wood.png - Copy.meta");
				REQUIRE(registry.Find(handle) != nullptr);
				CHECK(registry.Find(handle)->SourcePath == fixture.ProjectPath("Assets/Wood.png"));
			}

			// Rule 2: a copy made while the project is open loses against the registered path, even when it is shorter.
			{
				Test::AssetTestFixture fixture;
				fixture.WriteProjectFile("Assets/Textures/Wood.png", Test::MakeTestPng(2, 2));
				fixture.WriteProjectText("Assets/Textures/Wood.png.meta", MakeTextureMeta(handle));
				AssetRegistry registry;
				REQUIRE(registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers).has_value());
				fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(2, 2));
				fixture.WriteProjectText("Assets/Wood.png.meta", MakeTextureMeta(handle));
				Result<AssetScanResult> rescanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
				REQUIRE(rescanned.has_value());
				REQUIRE(rescanned->Diagnostics.size() == 1);
				CHECK(rescanned->Diagnostics.front().Path == "Assets/Wood.png.meta");
				CHECK(registry.Find(handle)->SourcePath == fixture.ProjectPath("Assets/Textures/Wood.png"));
				// GetKnownLocations is what the next cold open passes back.
				const std::vector<AssetKnownLocation> known = registry.GetKnownLocations();
				REQUIRE(known.size() == 1);
				CHECK(known.front() == AssetKnownLocation{ .Handle = handle, .SourcePath = fixture.ProjectPath("Assets/Textures/Wood.png") });

				// Rule 3: a cold open with the last session's locations keeps the original as well.
				AssetRegistry reopened;
				Result<AssetScanResult> cold = reopened.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers, known);
				REQUIRE(cold.has_value());
				REQUIRE(cold->Diagnostics.size() == 1);
				CHECK(cold->Diagnostics.front().Path == "Assets/Wood.png.meta");
				CHECK(reopened.Find(handle)->SourcePath == fixture.ProjectPath("Assets/Textures/Wood.png"));
			}
		}

		TEST_CASE("AssetRegistry: case mismatch diagnostic")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle handle(0x1111222233334444ull);
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(2, 2));
			// The sidecar's name matches the source only when case is ignored (§4.10 case policy).
			fixture.WriteProjectText("Assets/wood.png.meta", MakeTextureMeta(handle));

			AssetRegistry registry;
			Result<AssetScanResult> scanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions());
			REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
			REQUIRE(scanned->Diagnostics.size() == 1);
			CHECK(scanned->Diagnostics.front().Code == PathCaseMismatchCode);
			CHECK(scanned->Diagnostics.front().Path == "Assets/wood.png.meta");
			CHECK(scanned->Diagnostics.front().AutoFixable);
			// Neither an orphan meta nor a source without a meta: the case mismatch is the one problem.
			CHECK(scanned->SourcesWithoutMeta.empty());

			Result<AssetScanFix> fix = registry.PlanFix(scanned->Diagnostics.front(), AssetHandle());
			REQUIRE(fix.has_value());
			CHECK(fix->Kind == AssetScanFixKind::RenameMetaToSourceCase);
			CHECK(fix->NewMetaPath == fixture.ProjectPath("Assets/Wood.png.meta"));
			REQUIRE(fixture.GetVfs().Move(fix->MetaPath, fix->NewMetaPath).has_value());
			Result<AssetScanResult> fixed = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions());
			REQUIRE(fixed.has_value());
			CHECK(fixed->Diagnostics.empty());
			CHECK(registry.Resolve("Assets/Wood.png") == handle);
		}

		TEST_CASE("AssetRegistry: dependency metas move and trash with their owner")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle gltf(0x1000000000000001ull);
			const AssetHandle buffer(0x1000000000000002ull);
			const AssetHandle image(0x1000000000000003ull);
			WriteGltfWithDependencies(fixture, gltf, buffer, image);
			const std::vector<AssetImporterDescription> importers = MakeImporterDescriptions();

			AssetRegistry registry;
			Result<AssetScanResult> scanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
			REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
			CHECK(scanned->Diagnostics.empty());
			// The image has a dependency meta, so it is not a source waiting for a meta of its own.
			CHECK(scanned->SourcesWithoutMeta.empty());
			REQUIRE(registry.GetDependencyRecords(gltf).size() == 2);
			// Dependencies are never loadable assets.
			CHECK_FALSE(registry.Locate(buffer).has_value());
			CHECK_FALSE(registry.Resolve("Assets/Models/Textures/Track.png").has_value());

			// Move: the source, its .meta and every dependency with its meta, relative paths preserved.
			Result<std::vector<AssetFileMove>> moves = registry.PlanMove(gltf, fixture.ProjectPath("Assets/Levels/Track.gltf"));
			REQUIRE_MESSAGE(moves.has_value(), moves.error().ToString());
			const std::vector<AssetFileMove> expected = {
				{ fixture.ProjectPath("Assets/Models/Track.gltf"), fixture.ProjectPath("Assets/Levels/Track.gltf") },
				{ fixture.ProjectPath("Assets/Models/Track.gltf.meta"), fixture.ProjectPath("Assets/Levels/Track.gltf.meta") },
				{ fixture.ProjectPath("Assets/Models/Data/Track.bin"), fixture.ProjectPath("Assets/Levels/Data/Track.bin") },
				{ fixture.ProjectPath("Assets/Models/Data/Track.bin.meta"), fixture.ProjectPath("Assets/Levels/Data/Track.bin.meta") },
				{ fixture.ProjectPath("Assets/Models/Textures/Track.png"), fixture.ProjectPath("Assets/Levels/Textures/Track.png") },
				{ fixture.ProjectPath("Assets/Models/Textures/Track.png.meta"), fixture.ProjectPath("Assets/Levels/Textures/Track.png.meta") },
			};
			CHECK(*moves == expected);
			REQUIRE(ApplyMoves(fixture.GetVfs(), *moves).has_value());
			Result<AssetScanResult> moved = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
			REQUIRE(moved.has_value());
			CHECK(moved->Diagnostics.empty());
			REQUIRE(registry.Find(gltf) != nullptr);
			CHECK(registry.Find(gltf)->SourcePath == fixture.ProjectPath("Assets/Levels/Track.gltf"));
			CHECK(registry.GetDependencyRecords(gltf).size() == 2);

			// Trash: the same files under Library/Trash/<entry>/, keeping their project-relative paths.
			const VfsPath trash = fixture.ProjectPath("Library/Trash/1000000000000001");
			Result<std::vector<AssetFileMove>> trashed = registry.PlanTrash(gltf, trash);
			REQUIRE_MESSAGE(trashed.has_value(), trashed.error().ToString());
			REQUIRE(trashed->size() == 6);
			CHECK(trashed->front().To == fixture.ProjectPath("Library/Trash/1000000000000001/Assets/Levels/Track.gltf"));
			CHECK(trashed->back().To == fixture.ProjectPath("Library/Trash/1000000000000001/Assets/Levels/Textures/Track.png.meta"));
			REQUIRE(ApplyMoves(fixture.GetVfs(), *trashed).has_value());
			Result<AssetScanResult> afterTrash = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), importers);
			REQUIRE(afterTrash.has_value());
			// Nothing is left behind: no orphan dependency, no orphan meta.
			CHECK(afterTrash->Diagnostics.empty());
			CHECK(registry.GetRecordCount() == 0);
		}

		TEST_CASE("AssetRegistry: orphan metas and orphan dependencies are reported and fixable")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Gone.png.meta", MakeTextureMeta(AssetHandle(0x2000000000000001ull)));
			fixture.WriteProjectText("Assets/Loose.bin", "data");
			fixture.WriteProjectText("Assets/Loose.bin.meta", SerializeAssetMetadata(MakeDependencyMetadata(AssetHandle(0x2000000000000002ull), AssetHandle(0x2000000000000003ull))));

			AssetRegistry registry;
			Result<AssetScanResult> scanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions());
			REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
			CHECK(HasDiagnostic(*scanned, AssetOrphanMetaCode, "Assets/Gone.png.meta"));
			CHECK(HasDiagnostic(*scanned, AssetOrphanDependencyCode, "Assets/Loose.bin.meta"));
			for (const AssetDiagnostic& diagnostic : scanned->Diagnostics)
			{
				Result<AssetScanFix> fix = registry.PlanFix(diagnostic, AssetHandle());
				REQUIRE(fix.has_value());
				CHECK(fix->Kind == AssetScanFixKind::TrashMeta);
			}
		}

		TEST_CASE("AssetRegistry: sources without a meta and unreadable metas are reported")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/New.png", Test::MakeTestPng(2, 2));
			fixture.WriteProjectText("Assets/Readme.txt", "not an asset");
			fixture.WriteProjectFile("Assets/Broken.png", Test::MakeTestPng(2, 2));
			fixture.WriteProjectText("Assets/Broken.png.meta", "{ not json");

			AssetRegistry registry;
			Result<AssetScanResult> scanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions());
			REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
			// Readme.txt has no importer: it is ignored. Broken.png has a .meta, unreadable: reported and left alone.
			CHECK(scanned->SourcesWithoutMeta == std::vector<VfsPath>{ fixture.ProjectPath("Assets/New.png") });
			CHECK(HasDiagnostic(*scanned, AssetImportFailedCode, "Assets/Broken.png.meta"));
			CHECK(scanned->MetaCount == 1);
		}

		TEST_CASE("AssetRegistry: references resolve to main assets and sub-assets")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle gltf(0x1000000000000001ull);
			WriteGltfWithDependencies(fixture, gltf, AssetHandle(0x1000000000000002ull), AssetHandle(0x1000000000000003ull));
			AssetRegistry registry;
			REQUIRE(registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions()).has_value());

			AssetMetadata metadata = registry.Find(gltf)->Metadata;
			metadata.SubAssets = { { .Key = "mesh:0:Straight", .Handle = DeriveSubAssetHandle(gltf, "mesh:0:Straight"), .Type = AssetType::Mesh } };
			AssetRecord record = *registry.Find(gltf);
			record.Metadata = metadata;
			REQUIRE(registry.Update(record).has_value());

			const AssetHandle mesh = DeriveSubAssetHandle(gltf, "mesh:0:Straight");
			CHECK(registry.Resolve("Assets/Models/Track.gltf") == gltf);
			CHECK(registry.Resolve("Assets/Models/Track.gltf#mesh:0:Straight") == mesh);
			CHECK(registry.Resolve(mesh.ToString()) == mesh);
			CHECK(registry.GetReferencePath(mesh) == "Assets/Models/Track.gltf#mesh:0:Straight");
			const std::optional<AssetRegistry::Location> location = registry.Locate(mesh);
			REQUIRE(location.has_value());
			CHECK(location->Type == AssetType::Mesh);
			CHECK(location->SubAssetKey == "mesh:0:Straight");
			CHECK_FALSE(registry.Resolve("Assets/Models/track.gltf").has_value());
			CHECK_FALSE(registry.Resolve("engine://Meshes/Cube").has_value());
			std::vector<AssetHandle> handles = { gltf, mesh };
			std::ranges::sort(handles);
			CHECK(registry.GetHandles() == handles);
		}

		TEST_CASE("AssetRegistry: edits keep the lookups consistent and refuse conflicts")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle handle(0x4000000000000001ull);
			Result<AssetMetadata> parsed = ParseAssetMetadata(MakeTextureMeta(handle));
			REQUIRE(parsed.has_value());
			const AssetRecord record{ .Metadata = *parsed, .SourcePath = fixture.ProjectPath("Assets/Wood.png"), .MetaPath = fixture.ProjectPath("Assets/Wood.png.meta") };

			AssetRegistry registry;
			REQUIRE(registry.Add(record).has_value());
			CHECK(ErrorCodeOf(registry.Add(record)) == ErrorCode::AlreadyExists);
			AssetRecord samePath = record;
			samePath.Metadata.Handle = AssetHandle(0x4000000000000002ull);
			CHECK(ErrorCodeOf(registry.Add(samePath)) == ErrorCode::AlreadyExists);
			AssetRecord misplacedMeta = samePath;
			misplacedMeta.SourcePath = fixture.ProjectPath("Assets/Other.png");
			CHECK(ErrorCodeOf(registry.Add(misplacedMeta)) == ErrorCode::InvalidArgument);

			REQUIRE(registry.Rename(handle, fixture.ProjectPath("Assets/Textures/Oak.png")).has_value());
			CHECK(registry.FindBySourcePath(fixture.ProjectPath("Assets/Wood.png")) == nullptr);
			REQUIRE(registry.Find(handle) != nullptr);
			CHECK(registry.Find(handle)->MetaPath == fixture.ProjectPath("Assets/Textures/Oak.png.meta"));
			CHECK(registry.Resolve("Assets/Textures/Oak.png") == handle);
			CHECK(ErrorCodeOf(registry.Rename(AssetHandle(0x99), fixture.ProjectPath("Assets/X.png"))) == ErrorCode::NotFound);

			AssetRecord unknown = record;
			unknown.Metadata.Handle = AssetHandle(0x4000000000000003ull);
			CHECK(ErrorCodeOf(registry.Update(unknown)) == ErrorCode::NotFound);
			REQUIRE(registry.Remove(handle).has_value());
			CHECK(registry.GetRecordCount() == 0);
			CHECK(ErrorCodeOf(registry.Remove(handle)) == ErrorCode::NotFound);
			CHECK_FALSE(registry.Resolve("Assets/Textures/Oak.png").has_value());
		}

		TEST_CASE("AssetRegistry: a meta whose importer disagrees with its extension is ASSET_TYPE_MISMATCH")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle handle(0x4100000000000001ull);
			fixture.WriteProjectFile("Assets/Wood.gltf", Test::MakeTestPng(2, 2));
			fixture.WriteProjectText("Assets/Wood.gltf.meta", MakeTextureMeta(handle));
			AssetRegistry registry;
			Result<AssetScanResult> scanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions());
			REQUIRE_MESSAGE(scanned.has_value(), scanned.error().ToString());
			REQUIRE(scanned->Diagnostics.size() == 1);
			const AssetDiagnostic& mismatch = scanned->Diagnostics.front();
			CHECK(mismatch.Code == AssetTypeMismatchCode);
			CHECK(mismatch.Path == "Assets/Wood.gltf.meta");
			CHECK(mismatch.Severity == DiagnosticSeverity::Error);
			CHECK(mismatch.AutoFixable);
			// Still registered: its handle stays valid while the .meta is corrected.
			CHECK(registry.Find(handle) != nullptr);

			// §7.3: the fix rewrites the .meta for the importer that takes the extension, keeping the handle; the old
			// importer's settings and sub-assets go (null Settings: the new importer's defaults).
			Result<AssetScanFix> fix = registry.PlanFix(mismatch, AssetHandle());
			REQUIRE_MESSAGE(fix.has_value(), fix.error().ToString());
			CHECK(fix->Kind == AssetScanFixKind::RewriteImporter);
			CHECK(fix->MetaPath == fixture.ProjectPath("Assets/Wood.gltf.meta"));
			Result<AssetMetadata> rewritten = ParseAssetMetadata(fix->NewMetaText);
			REQUIRE_MESSAGE(rewritten.has_value(), rewritten.error().ToString());
			CHECK(rewritten->Handle == handle);
			CHECK(rewritten->Kind == AssetMetaKind::Asset);
			CHECK(rewritten->Type == AssetType::Prefab);
			CHECK(rewritten->Importer == "Gltf");
			CHECK(rewritten->ImporterVersion == 3);
			CHECK(rewritten->Settings.IsNull());
			CHECK(rewritten->SubAssets.empty());

			// Applied, the next scan reports nothing.
			fixture.WriteProjectText("Assets/Wood.gltf.meta", fix->NewMetaText);
			Result<AssetScanResult> rescanned = registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions());
			REQUIRE(rescanned.has_value());
			CHECK(rescanned->Diagnostics.empty());
			CHECK(registry.Find(handle) != nullptr);
			AssetDiagnostic unreported = mismatch;
			unreported.Path = "Assets/Other.gltf.meta";
			CHECK(ErrorCodeOf(registry.PlanFix(unreported, AssetHandle())) == ErrorCode::NotFound);
		}

		TEST_CASE("AssetRegistry: moves and trash refuse sub-assets, dependencies and bad destinations")
		{
			Test::AssetTestFixture fixture;
			const AssetHandle gltf(0x1000000000000001ull);
			const AssetHandle buffer(0x1000000000000002ull);
			WriteGltfWithDependencies(fixture, gltf, buffer, AssetHandle(0x1000000000000003ull));
			fixture.WriteProjectFile("Assets/Other.png", Test::MakeTestPng(2, 2));
			fixture.WriteProjectText("Assets/Other.png.meta", MakeTextureMeta(AssetHandle(0x1000000000000004ull)));
			AssetRegistry registry;
			REQUIRE(registry.Scan(fixture.GetVfs(), fixture.ProjectPath("Assets"), MakeImporterDescriptions()).has_value());
			AssetRecord record = *registry.Find(gltf);
			record.Metadata.SubAssets = { { .Key = "mesh:0", .Handle = DeriveSubAssetHandle(gltf, "mesh:0"), .Type = AssetType::Mesh } };
			REQUIRE(registry.Update(record).has_value());

			CHECK(ErrorCodeOf(registry.PlanMove(DeriveSubAssetHandle(gltf, "mesh:0"), fixture.ProjectPath("Assets/X.gltf"))) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(registry.PlanMove(buffer, fixture.ProjectPath("Assets/X.bin"))) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(registry.PlanMove(AssetHandle(0x5), fixture.ProjectPath("Assets/X.gltf"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(registry.PlanMove(gltf, fixture.ProjectPath("Library/Track.gltf"))) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(registry.PlanMove(gltf, fixture.ProjectPath("Assets/Models/Track.gltf"))) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(registry.PlanMove(gltf, fixture.ProjectPath("Assets/Other.png"))) == ErrorCode::AlreadyExists);
			CHECK(ErrorCodeOf(registry.PlanTrash(buffer, fixture.ProjectPath("Library/Trash/1"))) == ErrorCode::InvalidArgument);

			// A rename within the folder keeps the dependency files where they are.
			Result<std::vector<AssetFileMove>> renamed = registry.PlanMove(gltf, fixture.ProjectPath("Assets/Models/Circuit.gltf"));
			REQUIRE(renamed.has_value());
			CHECK(renamed->size() == 2);
		}
	}

}
