#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Support/AssetTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// EnvironmentImporter (Architecture §7.4, §8.6): Radiance .hdr through the GPU bake, Unsupported without a baker, .exr refused
// with its hint. Skeletons of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9); stream B implements the
// importer, registers it and removes the skips.

namespace Engine {

	namespace {

		// The committed Studio HDRI (Resources/Environments, Poly Haven CC0, 1k).
		Buffer ReadStudio()
		{
			Result<Buffer> hdr = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Environments/Studio.hdr");
			REQUIRE_MESSAGE(hdr.has_value(), hdr.error().ToString());
			return std::move(*hdr);
		}

		// Imports `bytes` as project://Assets/Environments/<name> with `settings` and `baker` (null: none).
		Result<ImportResult> ImportEnvironment(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> bytes,
			const Json& settings, IEnvironmentBaker* baker)
		{
			const std::string relative = std::string("Assets/Environments/") + std::string(name);
			fixture.WriteProjectFile(relative, bytes);
			const Buffer source = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xe0e7000000000001ull);
			metadata.Type = AssetType::Environment;
			metadata.Importer = std::string(EnvironmentImporter::Id);
			metadata.ImporterVersion = EnvironmentImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = source,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = baker,
				.ScriptDiagnostics = nullptr,
			});
			return EnvironmentImporter().Import(context, metadata);
		}

		Json DefaultSettings()
		{
			Json settings = Json::object();
			settings["ClampLuminance"] = false;
			settings["MaxLuminance"] = 1000.0;
			return settings;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("EnvironmentImporter: claims .hdr and .exr, runs on the main thread and is registered" * doctest::skip(true))
		{
			const EnvironmentImporter importer;
			CHECK(importer.CanImport(".hdr"));
			CHECK(importer.CanImport(".EXR"));
			CHECK(importer.RequiresMainThread());
			CHECK(importer.GetMainType() == AssetType::Environment);
			ImporterRegistry registry;
			RegisterBuiltinImporters(registry);
			CHECK(registry.FindForExtension(".hdr") == registry.FindById(EnvironmentImporter::Id));
			Test::AssetTestFixture fixture;
			CHECK(fixture.GetRegistry().FindStruct("EnvironmentImportSettings") != nullptr);
		}

		TEST_CASE("EnvironmentImporter: without a baker the import is Unsupported with the GPU hint" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> imported = ImportEnvironment(fixture, "Studio.hdr", ReadStudio(), DefaultSettings(), nullptr);
			REQUIRE_FALSE(imported.has_value());
			CHECK(imported.error().GetCode() == ErrorCode::Unsupported);
			CHECK(imported.error().ToString().contains("start the editor with a GPU once to bake this environment"));
		}

		TEST_CASE("EnvironmentImporter: .exr is refused with the Poly Haven hint and broken .hdr files fail precisely" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> exr = ImportEnvironment(fixture, "Sky.exr", AsBytes("v/1\x01"), DefaultSettings(), nullptr);
			REQUIRE_FALSE(exr.has_value());
			CHECK(exr.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(exr.error().ToString().contains("download the .hdr variant from Poly Haven"));

			const Result<ImportResult> garbage = ImportEnvironment(fixture, "Broken.hdr", AsBytes("#?RADIANCE\nnot an image"), DefaultSettings(), nullptr);
			REQUIRE_FALSE(garbage.has_value());
			CHECK(garbage.error().GetCode() == ErrorCode::ImportFailed);
		}

		TEST_CASE("EnvironmentImporter: bakes and cooks the Studio HDRI deterministically" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				Test::AssetTestFixture fixture;
				const Buffer studio = ReadStudio();
				const Result<ImportResult> first = ImportEnvironment(fixture, "Studio.hdr", studio, DefaultSettings(), baker->get());
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				REQUIRE(first->Artifacts.size() == 1);
				CHECK(first->Artifacts.front().Type == AssetType::Environment);
				const Result<AssetRef<EnvironmentData>> environment = LoadCookedEnvironment(first->Artifacts.front().Cooked);
				REQUIRE_MESSAGE(environment.has_value(), environment.error().ToString());
				CHECK((*environment)->Skybox.FaceSize == 256); // 1k HDRI: min(1024, 1024 / 4)
				CHECK((*environment)->Specular.FaceSize == EnvironmentData::SpecularFaceSize);
				// "Importers: importing twice gives identical hashes" holds for the GPU bake on one device.
				const Result<ImportResult> second = ImportEnvironment(fixture, "Studio.hdr", studio, DefaultSettings(), baker->get());
				REQUIRE(second.has_value());
				CHECK(second->Artifacts.front().Cooked == first->Artifacts.front().Cooked);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
