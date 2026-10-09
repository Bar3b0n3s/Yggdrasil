#include "TestsPCH.h"
#include "Support/GoldenScene.h"

#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestData.h"
#include "Support/Utf8Path.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The golden-scene support (Docs/Decisions/0013-m8-decisions.md decision 14): the image grid, the scenes the
// scaffold wrote into Projects/FeatureTest (on the CPU, so a broken scene fails the unit suite and not only the golden
// stage) and RenderGoldenScene's own contract on a device.

namespace Engine {

	// The golden scenes of Roadmap M8/M9, as Projects/FeatureTest/Scaffold/Golden.jsonl writes them.
	static constexpr std::array<std::string_view, 17> GoldenSceneNames = { "AlphaModes", "AnnotatedScreenshot", "Bloom", "DebugDraw",
		"GltfFixture", "GtaoOff", "GtaoOn", "GtaoOrtho", "IblOnly", "MaterialGrid", "SelectionOutline", "ShadowsFar", "ShadowsNear",
		"ShadowsOrtho", "SpotShadows", "Text", "Tonemappers" };

	static std::filesystem::path GetFeatureTestRoot()
	{
		return Test::GetRepositoryRoot() / "Projects" / "FeatureTest";
	}

	static std::filesystem::path GetFeatureTestPath(std::string_view relative)
	{
		return GetFeatureTestRoot() / Test::PathFromUtf8(relative);
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("GoldenScene: an image grid places each image row by row")
		{
			std::vector<Image> images;
			for (uint8_t value = 1; value <= 4; ++value)
			{
				Result<Image> image = CreateImage(2, 1, nvrhi::Format::RGBA8_UNORM);
				REQUIRE(image.has_value());
				std::fill(image->Pixels.begin(), image->Pixels.end(), std::byte{ value });
				images.push_back(std::move(*image));
			}
			const Result<Image> grid = Test::ComposeImageGrid(images, 2);
			REQUIRE_MESSAGE(grid.has_value(), grid.error().ToString());
			CHECK(grid->Width == 4);
			CHECK(grid->Height == 2);
			CHECK(grid->GetRow(0)[0] == std::byte{ 1 });
			CHECK(grid->GetRow(0)[8] == std::byte{ 2 });
			CHECK(grid->GetRow(1)[0] == std::byte{ 3 });
			CHECK(grid->GetRow(1)[8] == std::byte{ 4 });
			CHECK_FALSE(Test::ComposeImageGrid({}, 2).has_value());
			CHECK_FALSE(Test::ComposeImageGrid(images, 0).has_value());

			// A last row that is not full leaves its remaining cells zero.
			const Result<Image> partial = Test::ComposeImageGrid(std::span<const Image>(images).first(3), 2);
			REQUIRE_MESSAGE(partial.has_value(), partial.error().ToString());
			CHECK(partial->Width == 4);
			CHECK(partial->Height == 2);
			CHECK(partial->GetRow(1)[0] == std::byte{ 3 });
			CHECK(partial->GetRow(1)[8] == std::byte{ 0 });

			// Every image must have the first one's size and format.
			Result<Image> wide = CreateImage(3, 1, nvrhi::Format::RGBA8_UNORM);
			Result<Image> other = CreateImage(2, 1, nvrhi::Format::R8_UNORM);
			REQUIRE(wide.has_value());
			REQUIRE(other.has_value());
			const std::array<Image, 2> mixedSizes = { images[0], *wide };
			const std::array<Image, 2> mixedFormats = { images[0], *other };
			for (const std::array<Image, 2>& mixed : { mixedSizes, mixedFormats })
			{
				const Result<Image> rejected = Test::ComposeImageGrid(mixed, 2);
				REQUIRE_FALSE(rejected.has_value());
				CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("GoldenScene: the golden scenes load strictly and reference only assets the project or the engine has")
		{
			// The repository's project, opened read-only (nothing is written into it) with the engine resources, so built-in
			// handles resolve like the project's own.
			Test::AssetTestFixture assets({ .EngineResources = true });
			VirtualFileSystem& vfs = assets.GetVfs();
			REQUIRE(vfs.Unmount("project").has_value());
			REQUIRE(vfs.Mount("project", CreateScope<NativeDirectoryMount>(GetFeatureTestRoot(), MountAccess::ReadOnly)).has_value());
			const Result<AssetRefreshReport> refresh = assets.GetManager().OpenProject(
				{ .AssetsRoot = assets.ProjectPath("Assets"), .CacheRoot = Test::ParseVfsPath("cache://"), .ReadOnly = true, .HotReload = false });
			REQUIRE_MESSAGE(refresh.has_value(), refresh.error().ToString());
			CHECK(refresh->Diagnostics.empty());

			// Exactly the scenes of Roadmap M8/M9 live in the folder.
			const Result<std::vector<std::filesystem::path>> files = FileSystem::ListDirectory(GetFeatureTestPath("Assets/Scenes/Golden"));
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			std::vector<std::string> scenes;
			for (const std::filesystem::path& file : *files)
			{
				if (file.extension() == ".scene")
					scenes.push_back(Test::PathToUtf8(file.stem()));
			}
			CHECK(scenes == std::vector<std::string>(GoldenSceneNames.begin(), GoldenSceneNames.end()));

			const AssetManager& manager = assets.GetManager();
			for (const std::string_view name : GoldenSceneNames)
			{
				CAPTURE(std::string(name));
				SceneSpecification specification;
				specification.Name = std::string(name);
				specification.Registry = &assets.GetRegistry();
				specification.IdGenerator = &assets.GetGenerator();
				const Scope<Scene> scene = Scene::Create(specification);
				LoadReport report;
				const std::string relative = "Assets/Scenes/Golden/" + std::string(name) + ".scene";
				const Status loaded = SceneSerializer::LoadFromFile(*scene, vfs, assets.ProjectPath(relative), { .StrictUnknowns = true, .SourcePath = relative }, report);
				REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
				CHECK(report.Diagnostics.empty());
				TransformSystem::Update(*scene);

				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(*scene, { .Width = 640, .Height = 360 });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				CHECK(snapshot->HasCamera);
				CHECK_FALSE(snapshot->Meshes.empty());
				for (const MeshDrawItem& item : snapshot->Meshes)
				{
					CHECK(manager.GetAssetType(item.Mesh) == AssetType::Mesh);
					for (const AssetHandle material : item.Materials)
						CHECK((!material.IsValid() || manager.GetAssetType(material) == AssetType::Material));
				}
				if (snapshot->Environment.Environment.IsValid())
					CHECK(manager.GetAssetType(snapshot->Environment.Environment) == AssetType::Environment);
				// Only the two M9 GTAO-on fixtures enable AO; the M8 scenes keep it disabled.
				CHECK(snapshot->Post.SsaoEnabled == (name == "GtaoOn" || name == "GtaoOrtho"));
			}
		}

		TEST_CASE("GoldenScene: the scaffold's sources and their imported copies are the Tests/Data fixtures")
		{
			// Projects/FeatureTest/Scaffold/Sources holds the files Golden.jsonl imports (asset.import takes project paths or
			// absolute ones, so the committed scaffold imports from inside the project); they and the copies the import made
			// under Assets/ must stay byte-identical to the generated fixtures (Tests/Data/Generate).
			struct SourceCopy
			{
				std::string_view Fixture;  // below Tests/Data/Assets
				std::string_view Imported; // below Projects/FeatureTest/Assets
			};
			constexpr std::array<SourceCopy, 5> Copies = { {
				{ "Gltf/NormalMapped.gltf", "Models/Golden/NormalMapped.gltf" },
				{ "Gltf/Textured.bin", "Models/Golden/Textured.bin" },
				{ "Gltf/Textured.gltf", "Models/Golden/Textured.gltf" },
				{ "Gltf/Textures/Checker.png", "Models/Golden/Textures/Checker.png" },
				{ "Textures/Rgba.png", "Textures/Golden/Rgba.png" },
			} };
			for (const SourceCopy& copy : Copies)
			{
				CAPTURE(std::string(copy.Fixture));
				const Result<Buffer> fixture = FileSystem::ReadFile(Test::GetTestDataPath("Assets/" + std::string(copy.Fixture)));
				const Result<Buffer> source = FileSystem::ReadFile(GetFeatureTestPath("Scaffold/Sources/" + std::string(copy.Fixture)));
				const Result<Buffer> imported = FileSystem::ReadFile(GetFeatureTestPath("Assets/" + std::string(copy.Imported)));
				REQUIRE(fixture.has_value());
				REQUIRE(source.has_value());
				REQUIRE(imported.has_value());
				CHECK(*source == *fixture);
				CHECK(*imported == *fixture);
			}
		}

		TEST_CASE("GoldenScene: an unknown golden scene is NotFound" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			for (const std::string_view name : { "NoSuchScene", "../Golden/Bloom", "" })
			{
				CAPTURE(std::string(name));
				const Result<Image> image = Test::RenderGoldenScene(gpu, name);
				REQUIRE_FALSE(image.has_value());
				CHECK(image.error().GetCode() == ErrorCode::NotFound);
			}
		}

		TEST_CASE("GoldenScene: the snapshot edit applies before the capture at the requested size" * doctest::test_suite(Test::GpuSuite))
		{
			// The DebugDraw scene with its meshes and lights removed and a black clear colour renders black (within the
			// tonemapper's dither of one step) at 96 x 54.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			bool edited = false;
			const Test::GoldenSceneOptions options{ .Width = 96, .Height = 54, .EditSnapshot = [&edited](RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Camera.ViewportWidth == 96);
				CHECK(snapshot.Camera.ViewportHeight == 54);
				snapshot.Meshes.clear();
				snapshot.Lights.clear();
				snapshot.Camera.ClearColor = glm::vec3(0.0f);
				edited = true;
			} };
			const Result<Image> image = Test::RenderGoldenScene(gpu, "DebugDraw", options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(edited);
			REQUIRE(image->Width == 96);
			REQUIRE(image->Height == 54);
			REQUIRE(image->Format == nvrhi::Format::RGBA8_UNORM);
			int brightest = 0;
			for (size_t offset = 0; offset < image->Pixels.size(); offset += 4)
			{
				for (size_t channel = 0; channel < 3; ++channel)
					brightest = std::max(brightest, std::to_integer<int>(image->Pixels[offset + channel]));
			}
			CHECK(brightest <= 2);
		}
	}

}
