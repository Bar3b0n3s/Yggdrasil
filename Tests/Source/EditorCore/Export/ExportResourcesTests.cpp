#include "TestsPCH.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Export/AppBundle.h"
#include "EditorCore/Export/ExecutableResources.h"
#include "EditorCore/Export/Exporter.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Platform/Process.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExportBinaryRoot.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"
#include "Support/Utf8Path.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The exported executable's labelling (Architecture §6.1 "Export", §14.2 step 5, §16; Roadmap M15 acceptance; Docs/
// Decisions/0024-m15-contract.md decision 4, stream A). The export copies the Runtime this test binary was built with, so
// the patched file is a real executable, and the exported game is run once afterwards to prove it still starts.

namespace Engine {

	namespace {

		constexpr std::string_view GameName = "Tetromino";

		std::optional<Result<ExportReport>> RunToEnd(Exporter& exporter)
		{
			std::optional<Result<ExportReport>> outcome;
			const bool resolved = Test::WaitUntil([&exporter, &outcome]()
			{
				outcome = exporter.Poll();
				return outcome.has_value();
			});
			if (!resolved)
				return std::nullopt;
			return outcome;
		}

		void WriteBytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, bytes, { .KeepBackup = false }).has_value());
		}

		// This build's real Runtime as Scripts/Build.py stages it, with the notices file (Support/ExportBinaryRoot.h), and one
		// shader file.
		std::filesystem::path MakeRealBinaryRoot(const Test::TempDirectory& directory)
		{
			const std::filesystem::path root = Test::MakeExportBinaryRoot(directory);
			WriteBytes(root / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Shaders" / "Test" / "VSMain.spv", AsBytes("spirv"));
			return root;
		}

		// The test project as a game named GameName with Main.scene (one cube) as its start scene, the 256 x 256 Assets/Icon.png
		// and Export Version 2.3.4 and Company "Fixture Studio". Returns the decoded icon.
		Image MakeLabelledProject(Test::EditorTestFixture& fixture)
		{
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const Entity cube = editor.GetScene().CreateEntity("Cube");
			cube.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			const Result<std::string> scene = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(scene.has_value());
			REQUIRE(editor.WriteProjectFile(*editor.GetScenePath(), AsBytes(*scene)).has_value());
			REQUIRE(editor.MarkSceneSaved(*editor.GetScenePath()));

			Result<Image> icon = CreateImage(256, 256, nvrhi::Format::RGBA8_UNORM);
			REQUIRE(icon.has_value());
			for (size_t index = 0; index < icon->Pixels.size(); ++index)
				icon->Pixels[index] = static_cast<std::byte>(index % 4 == 3 ? 255 : (index / 4 * 7 + index % 4 * 61) % 256);
			const Result<Buffer> png = EncodePng(*icon);
			REQUIRE(png.has_value());
			const Result<VfsPath> iconPath = VfsPath::Create("project", "Assets/Icon.png");
			REQUIRE(iconPath.has_value());
			REQUIRE(editor.WriteProjectFile(*iconPath, *png).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());

			const Result<Json> patch = JsonReader::Parse(std::format(
				R"({{"Name":"{}","StartScene":"Assets/Scenes/Main.scene","Export":{{"BuildScenes":["Assets/Scenes/Main.scene"],"Icon":"Assets/Icon.png","Version":"2.3.4","Company":"Fixture Studio"}}}})",
				GameName));
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Label the game");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			return std::move(*icon);
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("Export: resources carry the project icon, version and company" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ExportResources");
			const Image icon = MakeLabelledProject(fixture);
			const Test::TempDirectory binaries("ExportResourcesBinaries");
			ExportSpecification specification;
			specification.Configuration = ExportConfiguration::Release;
			specification.BinaryRoot = MakeRealBinaryRoot(binaries);
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const ExportReport& report = **outcome;

#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(report.ResourcesPatched);
			const Result<Test::ExecutableResourceProbe> probe = Test::ProbeExecutableResources(report.Executable);
			REQUIRE_MESSAGE(probe.has_value(), probe.error().ToString());
			CHECK(probe->HasManifest);
			CHECK(probe->FileVersion == std::array<uint16_t, 4>{ 2, 3, 4, 0 });
			CHECK(probe->ProductVersion == std::array<uint16_t, 4>{ 2, 3, 4, 0 });
			const auto text = [&probe](const char* key) -> std::string
			{
				const auto found = probe->Strings.find(key);
				return found == probe->Strings.end() ? std::string("<missing>") : found->second;
			};
			CHECK(text("CompanyName") == "Fixture Studio");
			CHECK(text("ProductName") == GameName);
			CHECK(text("FileDescription") == GameName);
			CHECK(text("OriginalFilename") == std::string(GameName) + ".exe");
			CHECK(text("FileVersion") == "2.3.4.0");
			REQUIRE(probe->Icons.size() == ExecutableIconSizes.size());
			for (size_t index = 0; index < ExecutableIconSizes.size(); ++index)
			{
				CAPTURE(ExecutableIconSizes[index]);
				CHECK(probe->Icons[index].first == ExecutableIconSizes[index]);
				const Result<Image> expected = DownscaleImage(icon, ExecutableIconSizes[index]);
				REQUIRE(expected.has_value());
				const Result<Image> actual = DecodePng(probe->Icons[index].second);
				REQUIRE_MESSAGE(actual.has_value(), actual.error().ToString());
				CHECK(actual->Width == ExecutableIconSizes[index]);
				CHECK(actual->Height == ExecutableIconSizes[index]);
				CHECK(actual->Pixels == expected->Pixels);
			}
#elif defined(ENGINE_PLATFORM_MACOS)
			static_cast<void>(icon); // the .icns content is AppBundle's test
			CHECK(report.ResourcesPatched);
			const std::filesystem::path bundle = report.OutputDirectory / (std::string(GameName) + ".app");
			CHECK(report.Executable == bundle / "Contents" / "MacOS" / std::string(GameName));
			const Result<std::string> plist = FileSystem::ReadText(bundle / "Contents" / "Info.plist");
			REQUIRE_MESSAGE(plist.has_value(), plist.error().ToString());
			CHECK(plist->find("<string>2.3.4</string>") != std::string::npos);
			CHECK(plist->find("<string>Fixture Studio</string>") != std::string::npos);
			CHECK(plist->find(std::string("<string>") + std::string(GameName) + "</string>") != std::string::npos);
			CHECK(FileSystem::Exists(bundle / "Contents" / "Resources" / "AppIcon.icns"));
#else
			// Linux executables carry no resources (§16): the package holds the Runtime unchanged.
			static_cast<void>(icon);
			CHECK_FALSE(report.ResourcesPatched);
			const Result<Buffer> exported = FileSystem::ReadFile(report.Executable);
			const Result<Buffer> source = FileSystem::ReadFile(specification.BinaryRoot / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Runtime" / "Runtime");
			REQUIRE(exported.has_value());
			REQUIRE(source.has_value());
			CHECK(*exported == *source);
#endif

			// The patched executable still runs (headless, no device needed).
			const Test::TempDirectory userData("ExportResourcesUserData");
			const Result<ProcessResult> run = Process::Run({ .Executable = report.Executable,
															   .Arguments = { "--headless", "--renderer", "none", "--frames", "5",
																   "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) } },
				std::chrono::seconds(120));
			REQUIRE_MESSAGE(run.has_value(), run.error().ToString());
			INFO("stderr: ", run->StandardError);
			CHECK(run->ExitCode == 0);
		}
	}

}
