#include "TestsPCH.h"

#include "Engine/AssetPipeline/EditorAssetManager.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>

namespace Engine {

	namespace {

		// Copies files of Tests/Data/Assets/Gltf (paths relative to it) into project://<destination><path>, as another
		// program would (not through the manager's writer).
		void CopyGltfFixtures(Test::AssetTestFixture& fixture, std::initializer_list<std::string_view> files,
			std::string_view destination = "Assets/Models/")
		{
			for (const std::string_view file : files)
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/" + std::string(file)));
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				fixture.WriteProjectFile(std::string(destination) + std::string(file), *bytes);
			}
		}

		// The "Type" of the .meta at project://<relative> ("Texture", "Dependency", ...).
		std::string ReadMetaType(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			Result<AssetMetadata> meta = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile(relative)));
			REQUIRE_MESSAGE(meta.has_value(), meta.error().ToString());
			return meta->Kind == AssetMetaKind::Dependency ? std::string(AssetMetadata::DependencyTypeName) : std::string(AssetTypeToString(meta->Type));
		}

		bool HasTextureSubAsset(const AssetMetadata& metadata)
		{
			return std::ranges::any_of(metadata.SubAssets, [](const SubAssetEntry& entry)
			{
				return entry.Type == AssetType::Texture;
			});
		}

		// Polls at `seconds`, then drains the main-thread queue (the start of the next frame).
		void RunFrame(Test::AssetTestFixture& fixture, double seconds)
		{
			fixture.GetManager().Update(seconds);
			static_cast<void>(fixture.GetMainThreadQueue().Drain());
		}

		size_t CountDiagnostics(const AssetManager& manager, std::string_view code, AssetHandle asset)
		{
			return static_cast<size_t>(std::ranges::count_if(manager.GetDiagnostics(), [code, asset](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Code == code && diagnostic.Asset == asset;
			}));
		}

		// ".counted" files are PNGs imported as textures; every Import call is counted.
		class CountingTextureImporter final : public IAssetImporter
		{
		public:
			explicit CountingTextureImporter(uint32_t& importCount)
				: m_ImportCount(&importCount)
			{
			}

			[[nodiscard]] std::string_view GetId() const override { return "CountingTexture"; }
			[[nodiscard]] uint32_t GetVersion() const override { return 1; }
			[[nodiscard]] AssetType GetMainType() const override { return AssetType::Texture; }
			[[nodiscard]] std::span<const std::string_view> GetExtensions() const override
			{
				static constexpr std::string_view Extensions[] = { ".counted" };
				return Extensions;
			}
			[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

			[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override
			{
				++*m_ImportCount; // inline JobSystem: every import runs on the test's thread
				ENGINE_TRY_ASSIGN(Buffer cooked, TextureImporter::ImportTextureFromMemory(context.GetSourceBytes(), { .Usage = TextureUsage::Color, .GenerateMips = false }, "Counted"));
				ImportResult result;
				result.Artifacts.push_back({ .Handle = metadata.Handle, .Type = AssetType::Texture, .SubAssetKey = {}, .Cooked = std::move(cooked) });
				return result;
			}
		private:
			uint32_t* m_ImportCount = nullptr; // documented back-reference: the test's counter outlives the fixture
		};

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("EditorAssetManager: opening a project writes a meta for every new source" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Textures/Wood.png", Test::MakeTestPng(8, 8));
			fixture.WriteProjectText("Assets/Readme.txt", "ignored");
			const AssetRefreshReport report = fixture.OpenProject(false);
			REQUIRE(report.CreatedMetas == std::vector<VfsPath>{ fixture.ProjectPath("Assets/Textures/Wood.png.meta") });
			CHECK(report.Added.size() == 1);
			Result<AssetMetadata> meta = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile("Assets/Textures/Wood.png.meta")));
			REQUIRE_MESSAGE(meta.has_value(), meta.error().ToString());
			CHECK(meta->Importer == "Texture");
			CHECK(meta->Type == AssetType::Texture);
			CHECK(fixture.GetManager().Resolve("Assets/Textures/Wood.png") == meta->Handle);
			CHECK(fixture.GetManager().GetAssetType(meta->Handle) == AssetType::Texture);
			CHECK(fixture.GetManager().GetReferencePath(meta->Handle) == "Assets/Textures/Wood.png");
			// Opening again finds the meta: nothing new is written.
			fixture.GetManager().CloseProject();
			CHECK(fixture.OpenProject(false).CreatedMetas.empty());
		}

		TEST_CASE("EditorAssetManager: a loaded asset comes from the cache the second time" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(8, 8));
			fixture.OpenProject(false);
			const std::optional<AssetHandle> handle = fixture.GetManager().Resolve("Assets/Wood.png");
			REQUIRE(handle.has_value());

			Result<AssetRef<Asset>> first = fixture.GetManager().Load(*handle);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK(fixture.GetManager().GetState(*handle) == AssetState::Loaded);
			CHECK(fixture.GetManager().GetVersion(*handle) == 1);
			const AssetRef<TextureData> texture = AssetCast<TextureData>(*first);
			REQUIRE(texture != nullptr);
			CHECK(texture->Width == 8);
			CHECK(texture->Mips.size() == 4);

			// A second manager over the same mounts reads the cooked bytes without importing.
			fixture.GetManager().CloseProject();
			fixture.OpenProject(false);
			Result<AssetImportOutcome> reimport = fixture.GetManager().Reimport(*handle);
			REQUIRE(reimport.has_value());
			CHECK_FALSE(reimport->FromCache);
			Result<AssetRef<Asset>> second = fixture.GetManager().Load(*handle);
			REQUIRE(second.has_value());
			CHECK(AssetCast<TextureData>(*second)->Pixels == texture->Pixels);
		}

		TEST_CASE("EditorAssetManager: refresh reimports a changed source and bumps its version" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 1));
			fixture.OpenProject(false);
			const AssetHandle handle = fixture.GetManager().Resolve("Assets/Wood.png").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(handle).has_value());
			CHECK(fixture.GetManager().GetVersion(handle) == 1);

			fixture.WriteProjectFile("Assets/Wood.png", Test::MakeTestPng(4, 4, 2));
			Result<AssetRefreshReport> refreshed = fixture.GetManager().Refresh();
			REQUIRE_MESSAGE(refreshed.has_value(), refreshed.error().ToString());
			CHECK(refreshed->Changed == std::vector<AssetHandle>{ handle });
			CHECK(fixture.GetManager().GetVersion(handle) == 2);
			// Nothing changed since: a second refresh reimports nothing.
			Result<AssetRefreshReport> again = fixture.GetManager().Refresh();
			REQUIRE(again.has_value());
			CHECK(again->Changed.empty());
		}

		TEST_CASE("EditorAssetManager: built-ins resolve by engine path and serve placeholders" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			CHECK(fixture.GetManager().Resolve("engine://Meshes/Cube") == BuiltinAssetHandles::CubeMesh);
			CHECK(fixture.GetManager().GetAssetType(BuiltinAssetHandles::ErrorMaterial) == AssetType::Material);
			CHECK(fixture.GetManager().GetReferencePath(BuiltinAssetHandles::CubeMesh) == "engine://Meshes/Cube");
			Result<AssetRef<Asset>> cube = fixture.GetManager().Load(BuiltinAssetHandles::CubeMesh);
			REQUIRE_MESSAGE(cube.has_value(), cube.error().ToString());
			CHECK(AssetCast<MeshData>(*cube) != nullptr);
			CHECK(fixture.GetManager().GetMetadata(BuiltinAssetHandles::CubeMesh) == nullptr);
		}

		TEST_CASE("EditorAssetManager: a dependency handle cannot be loaded" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Data.bin", "data");
			const AssetHandle owner(0x3000000000000001ull);
			const AssetHandle dependency(0x3000000000000002ull);
			fixture.WriteProjectText("Assets/Data.bin.meta", SerializeAssetMetadata(MakeDependencyMetadata(dependency, owner)));
			fixture.OpenProject(false);
			Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(dependency);
			REQUIRE_FALSE(loaded.has_value());
			CHECK(loaded.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(fixture.GetManager().GetAssetType(dependency) == AssetType::None);
		}

		TEST_CASE("EditorAssetManager: a dry run leaves the registry as it was" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.OpenProject(false);
			const size_t before = fixture.GetManager().GetRegistry().GetRecordCount();
			fixture.GetManager().BeginDryRun();
			CHECK(fixture.GetManager().IsDryRun());
			CHECK(fixture.GetManager().GetWriter().IsDryRun());
			MaterialData material;
			material.Roughness = 0.2f;
			Result<std::string> text = MaterialToText(material, fixture.GetRegistry());
			REQUIRE(text.has_value());
			Result<AssetMetadata> meta = fixture.GetManager().CreateMetadata(fixture.ProjectPath("Assets/Red.material"));
			REQUIRE(meta.has_value());
			REQUIRE(fixture.GetManager().GetWriter().Write(fixture.ProjectPath("Assets/Red.material"), AsBytes(*text)).has_value());
			REQUIRE(fixture.GetManager().GetWriter().Write(fixture.ProjectPath("Assets/Red.material.meta"), AsBytes(SerializeAssetMetadata(*meta))).has_value());
			CHECK(fixture.GetManager().GetRegistry().GetRecordCount() == before + 1);
			fixture.GetManager().EndDryRun();
			CHECK_FALSE(fixture.GetManager().IsDryRun());
			CHECK(fixture.GetManager().GetRegistry().GetRecordCount() == before);
		}

		TEST_CASE("EditorAssetManager: import settings merge over the importer's defaults" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			Json patch = Json::object();
			patch["Usage"] = "NormalMap";
			Result<VariantValue> merged = fixture.GetManager().MergeImportSettings("Texture", VariantValue(), patch);
			REQUIRE_MESSAGE(merged.has_value(), merged.error().ToString());
			Json settings = merged->Get();
			CHECK(settings["Usage"] == Json("NormalMap"));
			CHECK(settings["GenerateMips"] == Json(true));
			Json invalid = Json::object();
			invalid["Usage"] = "Sometimes";
			Result<VariantValue> rejected = fixture.GetManager().MergeImportSettings("Texture", VariantValue(), invalid);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK_FALSE(fixture.GetManager().MergeImportSettings("Material", VariantValue(), patch).has_value());
		}

		TEST_CASE("EditorAssetManager: a glTF's referenced image becomes a dependency whatever order it arrives in" * doctest::skip(true))
		{
			// One batch at open: the glTF and the files it references arrive together.
			{
				Test::AssetTestFixture fixture;
				CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin", "Textures/Checker.png" });
				fixture.OpenProject(false);
				CHECK(ReadMetaType(fixture, "Assets/Models/Textured.bin.meta") == "Dependency");
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Dependency");
				CHECK(ReadMetaType(fixture, "Assets/Models/Textured.gltf.meta") == "Prefab");
			}

			// One poll: "Textured.gltf" sorts before "Textures/Checker.png" in the batch; the closure is resolved before any
			// meta is written, and the glTF's import sees the image's dependency meta (a texture sub-asset, not a reuse).
			{
				Test::AssetTestFixture fixture;
				fixture.OpenProject(true);
				RunFrame(fixture, 0.0);
				CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin", "Textures/Checker.png" });
				for (double seconds = 0.5; seconds <= 2.0; seconds += 0.5)
					RunFrame(fixture, seconds);
				fixture.GetManager().WaitIdle();
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Dependency");
				const AssetHandle gltf = fixture.GetManager().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
				REQUIRE(fixture.GetManager().Load(gltf).has_value());
				REQUIRE(fixture.GetManager().GetMetadata(gltf) != nullptr);
				CHECK(HasTextureSubAsset(*fixture.GetManager().GetMetadata(gltf)));
			}

			// The image in an earlier batch than the glTF: it keeps its own Texture meta (never converted), and the glTF reuses
			// that standalone texture by handle, also after the project is opened again (its manifest stays current).
			{
				Test::AssetTestFixture fixture;
				CopyGltfFixtures(fixture, { "Textures/Checker.png" });
				fixture.OpenProject(false);
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Texture");
				const AssetHandle checker = fixture.GetManager().Resolve("Assets/Models/Textures/Checker.png").value_or(AssetHandle());
				CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin" });
				REQUIRE(fixture.GetManager().Refresh().has_value());
				CHECK(ReadMetaType(fixture, "Assets/Models/Textures/Checker.png.meta") == "Texture");
				const AssetHandle gltf = fixture.GetManager().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
				REQUIRE(fixture.GetManager().Load(gltf).has_value());
				CHECK_FALSE(HasTextureSubAsset(*fixture.GetManager().GetMetadata(gltf)));
				CHECK(fixture.GetManager().GetPathDependents(checker) == std::vector<AssetHandle>{ gltf });
				const std::vector<SubAssetEntry> subAssets = fixture.GetManager().GetMetadata(gltf)->SubAssets;
				fixture.GetManager().CloseProject();
				fixture.OpenProject(false);
				REQUIRE(fixture.GetManager().Load(gltf).has_value());
				CHECK(fixture.GetManager().GetMetadata(gltf)->SubAssets == subAssets);
			}
		}

		TEST_CASE("EditorAssetManager: a file that is already another glTF's dependency fails the second import" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			CopyGltfFixtures(fixture, { "Textured.gltf", "Textured.bin", "Textures/Checker.png" });
			fixture.OpenProject(false);
			const AssetHandle owner = fixture.GetManager().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
			REQUIRE(fixture.GetManager().Load(owner).has_value());

			// A second glTF (an LOD export, say) that reads the same buffer and image: a dependency has one owner, so moving or
			// trashing the first glTF could otherwise take the files away from the second.
			fixture.WriteProjectFile("Assets/Models/TexturedLod.gltf", fixture.ReadProjectFile("Assets/Models/Textured.gltf"));
			REQUIRE(fixture.GetManager().Refresh().has_value());
			const AssetHandle lod = fixture.GetManager().Resolve("Assets/Models/TexturedLod.gltf").value_or(AssetHandle());
			REQUIRE(lod.IsValid());
			Test::ExpectLog failure(LogLevel::Error, "Assets/Models/TexturedLod.gltf");
			Result<AssetRef<Asset>> loaded = fixture.GetManager().Load(lod);
			REQUIRE_FALSE(loaded.has_value());
			CHECK(loaded.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(loaded.error().ToString().find("Assets/Models/Textured.gltf") != std::string::npos);
			CHECK(fixture.GetManager().HasErrorDiagnostics());
			// The first glTF keeps its files.
			Result<AssetMetadata> bufferMeta = ParseAssetMetadata(AsStringView(fixture.ReadProjectFile("Assets/Models/Textured.bin.meta")));
			REQUIRE(bufferMeta.has_value());
			CHECK(bufferMeta->Owner == owner);
		}

		TEST_CASE("EditorAssetManager: a missing reference's diagnostic clears when the asset comes back" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Red.material", R"({"Format": "Material", "Version": 1})");
			fixture.OpenProject(false);
			const AssetHandle red = fixture.GetManager().Resolve("Assets/Red.material").value_or(AssetHandle());
			REQUIRE(red.IsValid());
			const AssetRef<MaterialData> placeholder = AssetCast<MaterialData>(fixture.GetManager().GetPlaceholder(AssetType::Material));

			// asset.delete moves the pair into the trash; a scene still uses the material.
			AssetWriter& writer = fixture.GetManager().GetWriter();
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Red.material"), fixture.ProjectPath("Library/Trash/1/Assets/Red.material")).has_value());
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Red.material.meta"), fixture.ProjectPath("Library/Trash/1/Assets/Red.material.meta")).has_value());
			{
				Test::ExpectLog missing(LogLevel::Error, red.ToString());
				CHECK(fixture.GetManager().GetOrPlaceholder<MaterialData>(red) == placeholder);
			}
			CHECK(CountDiagnostics(fixture.GetManager(), AssetMissingCode, red) == 1);
			// A reference diagnostic does not gate play or export by itself (ProjectValidator checks current references).
			CHECK_FALSE(fixture.GetManager().HasErrorDiagnostics());

			// edit.undo moves the pair back: the handle is registered again and the diagnostic goes.
			REQUIRE(writer.Move(fixture.ProjectPath("Library/Trash/1/Assets/Red.material.meta"), fixture.ProjectPath("Assets/Red.material.meta")).has_value());
			REQUIRE(writer.Move(fixture.ProjectPath("Library/Trash/1/Assets/Red.material"), fixture.ProjectPath("Assets/Red.material")).has_value());
			CHECK(CountDiagnostics(fixture.GetManager(), AssetMissingCode, red) == 0);
			CHECK(fixture.GetManager().GetOrPlaceholder<MaterialData>(red) != placeholder);
			CHECK_FALSE(fixture.GetManager().HasErrorDiagnostics());
		}

		TEST_CASE("EditorAssetManager: a failed import is not retried until its source changes" * doctest::skip(true))
		{
			uint32_t importCount = 0;
			Test::AssetTestFixture fixture;
			fixture.GetImporters().Register(CreateScope<CountingTextureImporter>(importCount));
			fixture.WriteProjectText("Assets/Broken.counted", "not a png");
			fixture.OpenProject(false);
			const AssetHandle broken = fixture.GetManager().Resolve("Assets/Broken.counted").value_or(AssetHandle());
			REQUIRE(broken.IsValid());
			{
				Test::ExpectLog failure(LogLevel::Error, "Assets/Broken.counted");
				Result<AssetRef<Asset>> first = fixture.GetManager().Load(broken);
				REQUIRE_FALSE(first.has_value());
				Result<AssetRef<Asset>> second = fixture.GetManager().Load(broken);
				REQUIRE_FALSE(second.has_value());
				CHECK(second.error().GetCode() == first.error().GetCode());
				for (int use = 0; use < 3; ++use)
					CHECK(fixture.GetManager().GetOrPlaceholder<TextureData>(broken) != nullptr);
			}
			// Imported once: every later call got the recorded error.
			CHECK(importCount == 1);
			CHECK(fixture.GetManager().GetState(broken) == AssetState::Failed);

			// A changed source is imported again, and the failure's diagnostic goes.
			fixture.WriteProjectFile("Assets/Broken.counted", Test::MakeTestPng(2, 2));
			REQUIRE(fixture.GetManager().Refresh().has_value());
			REQUIRE(fixture.GetManager().Load(broken).has_value());
			CHECK(importCount == 2);
			CHECK(fixture.GetManager().GetState(broken) == AssetState::Loaded);
			CHECK(CountDiagnostics(fixture.GetManager(), AssetImportFailedCode, broken) == 0);
		}
	}

}
