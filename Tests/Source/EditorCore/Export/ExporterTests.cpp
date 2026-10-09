#include "TestsPCH.h"

#include "EditorCore/Export/Exporter.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IEnvironmentBaker.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Paths.h"
#include "Engine/Project/GameManifest.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The exporter v0 (Architecture Â§14.1, Â§14.2; Docs/Decisions/0012-m7-decisions.md decision 14) against a fake build output
// directory: a Runtime "executable" the export only copies (never runs), the app-local CRT on Windows and a few shader
// files. The exports of the real Runtime, the smoke test per configuration and the exported game's run are tested by
// Tests/Automation/test_export.py and test_runtime.py against the built binaries.

namespace Engine {

	namespace {

		constexpr std::string_view MainScene = "Assets/Scenes/Main.scene";
		constexpr std::string_view FakeRuntimeBytes = "not a real executable: the export copies it and never runs it";

		// Polls `exporter` until it resolves. Cooking runs on jobs, so the wait is bounded by Test::WaitUntil's deadline, a
		// failure bound only (a spin count would depend on the machine's speed); nullopt when it did not resolve by then.
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

		// Polls `exporter` until it reaches `phase`, which must come before Done; false when it resolved (or never got there).
		bool RunUntilPhase(Exporter& exporter, ExportPhase phase)
		{
			bool resolved = false;
			const bool reached = Test::WaitUntil([&exporter, &resolved, phase]()
			{
				if (exporter.GetPhase() == phase)
					return true;
				resolved = exporter.Poll().has_value();
				return resolved;
			});
			return reached && !resolved && exporter.GetPhase() == phase;
		}

		void WriteTestFile(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, AsBytes(text), { .KeepBackup = false }).has_value());
		}

		// A bin directory as Scripts/Build.py leaves it for `configuration`: Runtime/Runtime(.exe), Runtime/Redist/*.dll on
		// Windows, and Shaders/ with two files the export packages and a dependency file it leaves out.
		std::filesystem::path MakeBinaryRoot(const Test::TempDirectory& directory, ExportConfiguration configuration)
		{
			const std::filesystem::path root = directory / "bin";
			const std::filesystem::path output = root / Exporter::GetBuildOutputDirectoryName(configuration);
#if defined(ENGINE_PLATFORM_WINDOWS)
			WriteTestFile(output / "Runtime" / "Runtime.exe", FakeRuntimeBytes);
			for (const std::string_view library : { "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll" })
				WriteTestFile(output / "Runtime" / "Redist" / std::string(library), library);
#else
			WriteTestFile(output / "Runtime" / "Runtime", FakeRuntimeBytes);
#endif
			WriteTestFile(output / "Shaders" / "Test" / "VSMain.spv", "spirv");
			WriteTestFile(output / "Shaders" / "Test" / "VSMain.refl.json", "{}");
			WriteTestFile(output / "Shaders" / "Test" / "VSMain.d", "dependencies");
			return root;
		}

		void ApplySettings(EditorContext& editor, std::string_view patchText)
		{
			const Result<Json> patch = JsonReader::Parse(patchText);
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
		}

		// Saves the open scene to its own path.
		void SaveScene(EditorContext& editor)
		{
			REQUIRE(editor.GetScenePath().has_value());
			const Result<std::string> text = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(text.has_value());
			REQUIRE(editor.WriteProjectFile(*editor.GetScenePath(), AsBytes(*text)).has_value());
			REQUIRE(editor.MarkSceneSaved(*editor.GetScenePath()));
		}

		// A project whose Main.scene holds one cube and is the start scene and the only build scene.
		void MakeExportableProject(Test::EditorTestFixture& fixture)
		{
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene(MainScene);
			EditorContext& editor = fixture.GetEditor();
			const Entity cube = editor.GetScene().CreateEntity("Cube");
			cube.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			SaveScene(editor);
			ApplySettings(editor, R"({"StartScene":"Assets/Scenes/Main.scene","Export":{"BuildScenes":["Assets/Scenes/Main.scene"]}})");
		}

		ExportSpecification MakeSpecification(const std::filesystem::path& binaryRoot)
		{
			ExportSpecification specification;
			specification.Configuration = ExportConfiguration::Release;
			specification.BinaryRoot = binaryRoot;
			return specification;
		}

		std::vector<std::string> ListNames(const std::filesystem::path& directory)
		{
			std::vector<std::string> names;
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory, false);
			REQUIRE(entries.has_value());
			for (const std::filesystem::path& entry : *entries)
				names.push_back(FileSystem::PathToUtf8(entry.filename()));
			return names;
		}

		std::string ExecutableName(std::string_view name)
		{
#if defined(ENGINE_PLATFORM_WINDOWS)
			return std::string(name) + ".exe";
#else
			return std::string(name);
#endif
		}

		std::vector<std::string> IssuePointers(const Error& error)
		{
			std::vector<std::string> pointers;
			for (const ErrorIssue& issue : error.GetIssues())
				pointers.push_back(issue.JsonPointer);
			return pointers;
		}

		// M8: an editor whose engine context mounts the repository's Resources as engine:// and a temporary engine cooked cache
		// as enginecache:// (as a development editor does, with --engine-cache-dir), so Engine.pak takes the built-ins of
		// EngineAssets.json; `baker` is the editor's environment baker (null: no GPU, as with --renderer none). The project
		// is MakeExportableProject's.
		class EngineResourcesEditor
		{
		public:
			explicit EngineResourcesEditor(IEnvironmentBaker* baker = nullptr)
				: m_Directory("ExportEngineResources")
			{
				REQUIRE(FileSystem::CreateDirectories(m_Directory / "UserData").has_value());
				Result<Scope<EngineContext>> engine = EngineContext::Create({
					.WorkerCount = 0,
					.UserDataDirectory = m_Directory / "UserData",
					.EngineResourcesDirectory = Test::GetRepositoryRoot() / "Resources",
					.EngineCacheDirectory = m_Directory / "EngineCache",
					.RegisterTypes = &RegisterEditorMethodTypes,
				});
				REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
				m_Engine = std::move(*engine);
				Result<Scope<EditorContext>> editor = EditorContext::Create(*m_Engine, {
																						   .IdGeneratorState = Test::EditorTestIdState,
																						   .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources" / "Templates" / "Projects",
																						   .ReadOnlyCacheRoot = m_Directory / "ReadOnlyCache",
																						   .HistoryLimits = {},
																						   .EnvironmentBaker = baker,
																					   });
				REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
				m_Editor = std::move(*editor);

				const Result<CreatedProject> created = ProjectManager::CreateProject(
					{
						.Directory = GetProjectRoot(),
						.Name = "TestProject",
						.Template = ProjectTemplate::Empty,
						.TemplatesDirectory = m_Editor->GetSpecification().TemplatesDirectory,
					},
					m_Engine->GetTypeRegistry());
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(created->ProjectFile, {}, m_Engine->GetTypeRegistry());
				REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
				REQUIRE(m_Editor->OpenProject(std::move(*project)).has_value());

				const Result<VfsPath> scenePath = VfsPath::Create("project", MainScene);
				REQUIRE(scenePath.has_value());
				Scope<Scene> scene = m_Editor->CreateScene("Main");
				const Entity cube = scene->CreateEntity("Cube");
				cube.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
				const Result<std::string> text = SceneSerializer::SaveToString(*scene);
				REQUIRE(text.has_value());
				REQUIRE(m_Editor->WriteProjectFile(*scenePath, AsBytes(*text)).has_value());
				m_Editor->SetScene(std::move(scene), *scenePath);
				ApplySettings(*m_Editor, R"({"StartScene":"Assets/Scenes/Main.scene","Export":{"BuildScenes":["Assets/Scenes/Main.scene"]}})");
			}

			~EngineResourcesEditor()
			{
				// The editor holds the project lock and the project:// mount: release both before the directory goes.
				m_Editor.reset();
				m_Engine.reset();
			}

			EngineResourcesEditor(const EngineResourcesEditor&) = delete;
			EngineResourcesEditor& operator=(const EngineResourcesEditor&) = delete;

			[[nodiscard]] EditorContext& GetEditor() { return *m_Editor; }
			[[nodiscard]] std::filesystem::path GetProjectRoot() const { return m_Directory / "TestProject"; }
		private:
			Test::TempDirectory m_Directory;
			Scope<EngineContext> m_Engine;
			Scope<EditorContext> m_Editor;
		};

		// The warnings of `report` that mention environments.
		std::vector<std::string> EnvironmentWarnings(const ExportReport& report)
		{
			std::vector<std::string> warnings;
			for (const std::string& warning : report.Warnings)
			{
				if (warning.find("environment") != std::string::npos)
					warnings.push_back(warning);
			}
			return warnings;
		}

		// M12's sound-effect presets (engine://Audio/*, File entries cooked by SoundEffectImporter, which needs no GPU) are in
		// `enginePak` as AudioClips that load.
		void CheckSoundPresets(const PakReader& enginePak)
		{
			constexpr std::array Presets = { BuiltinAssetHandles::ClickSound, BuiltinAssetHandles::BlipSound, BuiltinAssetHandles::CoinSound,
				BuiltinAssetHandles::JumpSound, BuiltinAssetHandles::HitSound, BuiltinAssetHandles::ExplosionSound, BuiltinAssetHandles::PowerUpSound,
				BuiltinAssetHandles::LineClearSound, BuiltinAssetHandles::WinSound, BuiltinAssetHandles::LoseSound };
			for (const AssetHandle handle : Presets)
			{
				CAPTURE(handle.ToString());
				const PakEntry* entry = enginePak.FindByHandle(handle);
				REQUIRE(entry != nullptr);
				CHECK(entry->Type == "AudioClip");
				const Result<Buffer> bytes = enginePak.ReadEntry(*entry);
				REQUIRE(bytes.has_value());
				const Result<AssetRef<AudioClipData>> clip = LoadCookedAudioClip(*bytes);
				REQUIRE_MESSAGE(clip.has_value(), clip.error().ToString());
			}
			// The silent clip is procedural: the Runtime generates it, so it is not in the pak.
			CHECK(enginePak.FindByHandle(BuiltinAssetHandles::SilentClip) == nullptr);
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("Exporter: names the default output directory after the platform, configuration and project")
		{
			const std::string directory = Exporter::GetDefaultOutputDirectory("Tetris", ExportConfiguration::Dist);
			CHECK(directory == std::string("Build/") + std::string(Exporter::GetPlatformName()) + "-Dist/Tetris");
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(Exporter::GetPlatformName() == "Windows");
			CHECK(Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) == "Release-windows-x86_64");
#elif defined(ENGINE_PLATFORM_LINUX)
			CHECK(Exporter::GetPlatformName() == "Linux");
			CHECK(Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Debug) == "Debug-linux-x86_64");
#elif defined(ENGINE_PLATFORM_MACOS)
			CHECK(Exporter::GetPlatformName() == "macOS");
			CHECK(Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Dist) == "Dist-macosx-AARCH64");
#endif
		}

		TEST_CASE("Exporter: the specification is checked when the export starts")
		{
			Test::EditorTestFixture fixture;
			ExportSpecification specification;
			specification.BinaryRoot = fixture.GetDirectory().GetPath();
			const Result<Scope<Exporter>> noProject = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_FALSE(noProject.has_value());
			CHECK(noProject.error().GetCode() == ErrorCode::InvalidState);

			fixture.CreateAndOpenProject();
			specification.BinaryRoot = fixture.GetProjectRoot();
			for (const std::string_view outside : { "Assets/Out", "Build", "Build/../Assets/Out", "Build/", "/Build/Out", "build/Out" })
			{
				INFO(std::string(outside));
				specification.OutputDirectory = std::string(outside);
				const Result<Scope<Exporter>> refused = Exporter::Start(fixture.GetEditor(), specification);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			specification.OutputDirectory = "Build/Custom/Game";
			CHECK(Exporter::Start(fixture.GetEditor(), specification).has_value());

			specification.OutputDirectory.clear();
			specification.BinaryRoot.clear();
			const Result<Scope<Exporter>> noBinaries = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_FALSE(noBinaries.has_value());
			CHECK(noBinaries.error().GetCode() == ErrorCode::InvalidArgument);

			specification.BinaryRoot = fixture.GetProjectRoot();
			specification.ExportRoot = "Relative/Export";
			const Result<Scope<Exporter>> relativeRoot = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_FALSE(relativeRoot.has_value());
			CHECK(relativeRoot.error().GetCode() == ErrorCode::InvalidArgument);

			specification.ExportRoot.clear();
			specification.SmokeTestFrames = 0;
			const Result<Scope<Exporter>> noFrames = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_FALSE(noFrames.has_value());
			CHECK(noFrames.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Exporter: a project without a start scene fails validation and writes nothing")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			ExportSpecification specification;
			specification.BinaryRoot = fixture.GetProjectRoot();
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::Validation);
			CHECK(IssuePointers(outcome->error()) == std::vector<std::string>{ "/StartScene" });
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", error));
		}

		TEST_CASE("Exporter: validation reports every problem of the project at its setting")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Tests/Level.scene");
			EditorContext& editor = fixture.GetEditor();
			// A name that cannot be a file name, a start scene that does not exist and a build scene the default
			// Export.Exclude glob "Assets/Tests/**" leaves out.
			ApplySettings(editor, R"({"Name":"Game:One","StartScene":"Assets/Scenes/Missing.scene","Export":{"BuildScenes":["Assets/Tests/Level.scene"]}})");
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			REQUIRE(IssuePointers(error) == std::vector<std::string>{ "/Name", "/StartScene", "/Export/BuildScenes/0" });
			CHECK(error.GetIssues()[2].Message.find("Assets/Tests/**") != std::string::npos);
			CHECK((*exporter)->GetPhase() == ExportPhase::Validate);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: a game name the Runtime would refuse fails validation at /Name, and nothing is written")
		{
			// The Runtime reads Game.json's Name with Paths::ValidateAppName; validation applies the same rule, so such a name
			// never reaches the WriteManifest step after the paks and the Runtime were written.
			for (const std::string& name : { std::string(" Tiny"), std::string("Tiny."), std::string(Paths::MaxAppNameLength + 1, 'A') })
			{
				INFO(name);
				Test::EditorTestFixture fixture("ExportBadName");
				MakeExportableProject(fixture);
				ApplySettings(fixture.GetEditor(), Json{ { "Name", name } }.dump());
				const Test::TempDirectory binaries("ExportBinaries");
				Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
				REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_FALSE(outcome->has_value());
				INFO(outcome->error().ToString());
				CHECK(outcome->error().GetCode() == ErrorCode::Validation);
				CHECK(IssuePointers(outcome->error()) == std::vector<std::string>{ "/Name" });
				CHECK((*exporter)->GetPhase() == ExportPhase::Validate);
				std::error_code exists;
				CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
			}
		}

		TEST_CASE("Exporter: a game name of the longest legal length exports, and the staging directory's name does not grow with it")
		{
			// The staged executable's temporary file must stay inside Windows' MAX_PATH where long paths are off: the staging
			// directory's name is ".export-staging-<pid>-<tag>", never the game's name again.
			Test::EditorTestFixture fixture("ExportLongName");
			MakeExportableProject(fixture);
			const std::string name(Paths::MaxAppNameLength, 'G');
			ApplySettings(fixture.GetEditor(), Json{ { "Name", name } }.dump());
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
			REQUIRE(exporter.has_value());
			REQUIRE(RunUntilPhase(**exporter, ExportPhase::CopyRuntime));
			const std::filesystem::path output = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory(name, ExportConfiguration::Release));
			const std::vector<std::string> staging = ListNames(output.parent_path());
			REQUIRE(staging.size() == 1);
			CHECK(staging.front().starts_with(".export-staging-"));
			CHECK_FALSE(staging.front().contains(name));
			CHECK(staging.front().size() < 40);

			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			CHECK((*outcome)->Executable == output / ExecutableName(name));
			CHECK(FileSystem::Exists((*outcome)->Executable));
			CHECK(ListNames(output.parent_path()) == std::vector<std::string>{ name });
		}

		TEST_CASE("Exporter: a missing Runtime is NotFound naming the build command")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			// Release is built, Dist is not.
			ExportSpecification dist = MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release));
			dist.Configuration = ExportConfiguration::Dist;
			Result<Scope<Exporter>> distExporter = Exporter::Start(fixture.GetEditor(), dist);
			REQUIRE(distExporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**distExporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::NotFound);
			CHECK(error.GetMessageText().find("python Scripts/Build.py --config Dist --project Runtime") != std::string::npos);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: a reference to an asset the export does not include fails the export")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			// A mesh handle no asset has: the scene still loads (handles are data), but the game could not find the mesh.
			const AssetHandle missing(0x1234567890abcdefull);
			const Entity broken = editor.GetScene().CreateEntity("Broken");
			broken.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(missing);
			SaveScene(editor);
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			REQUIRE(error.GetIssues().size() == 1);
			CHECK(error.GetIssues().front().Message.find(missing.ToString()) != std::string::npos);
			CHECK(error.GetIssues().front().Message.find(std::string(MainScene)) != std::string::npos);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: an export writes the renamed executable, the CRT, both paks and the manifest")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			// A scene the default Export.Exclude glob "Assets/Tests/**" leaves out of Game.pak.
			const Result<VfsPath> excluded = VfsPath::Create("project", "Assets/Tests/Level.scene");
			REQUIRE(excluded.has_value());
			const Result<std::string> excludedText = SceneSerializer::SaveToString(*editor.CreateScene("Level"));
			REQUIRE(excludedText.has_value());
			REQUIRE(editor.WriteProjectFile(*excluded, AsBytes(*excludedText)).has_value());

			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const ExportReport& report = **outcome;
			CHECK((*exporter)->GetPhase() == ExportPhase::Done);

			const std::filesystem::path output = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release));
			CHECK(report.OutputDirectory == output);
			CHECK(report.Executable == output / ExecutableName("TestProject"));
			CHECK(report.Configuration == ExportConfiguration::Release);
			CHECK_FALSE(report.SmokeTestRan);
			// Nothing but the output directory is left beside it (no staging directory).
			CHECK(ListNames(output.parent_path()) == std::vector<std::string>{ "TestProject" });

			const Result<std::string> executable = FileSystem::ReadText(report.Executable);
			REQUIRE(executable.has_value());
			CHECK(*executable == FakeRuntimeBytes);
			std::vector<std::string> expected = { "Data", ExecutableName("TestProject"), "Game.json" };
#if defined(ENGINE_PLATFORM_WINDOWS)
			expected.insert(expected.end(), { "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll" });
#endif
			std::ranges::sort(expected);
			CHECK(ListNames(output) == expected);

			// Engine.pak: the configuration's shader files (not their dependency files) and its metadata.
			const Result<Ref<const PakReader>> enginePak = PakReader::Open(output / "Data" / "Engine.pak");
			REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
			CHECK((*enginePak)->FindByPath("Shaders/Test/VSMain.spv") != nullptr);
			CHECK((*enginePak)->FindByPath("Shaders/Test/VSMain.refl.json") != nullptr);
			CHECK((*enginePak)->FindByPath("Shaders/Test/VSMain.d") == nullptr);
			CHECK(report.EngineEntryCount == (*enginePak)->GetEntries().size());
			const Json& engineMetadata = (*enginePak)->GetMetadata().Get();
			CHECK(engineMetadata["Configuration"] == Json("Release"));
			CHECK(engineMetadata.contains("EngineVersion"));

			// Game.pak: every registered asset (here the scene), with the project settings in its metadata.
			const Result<Ref<const PakReader>> gamePak = PakReader::Open(output / "Data" / "Game.pak");
			REQUIRE_MESSAGE(gamePak.has_value(), gamePak.error().ToString());
			const std::optional<AssetHandle> scene = editor.GetAssets().Resolve(MainScene);
			REQUIRE(scene.has_value());
			const PakEntry* sceneEntry = (*gamePak)->FindByHandle(*scene);
			REQUIRE(sceneEntry != nullptr);
			CHECK(sceneEntry->Type == "Scene");
			CHECK(sceneEntry->Path == MainScene);
			CHECK((*gamePak)->ReadEntry(*sceneEntry).has_value());
			CHECK((*gamePak)->FindByPath("Assets/Tests/Level.scene") == nullptr);
			CHECK((*gamePak)->GetEntries().size() == 1);
			CHECK(report.GameAssetCount == (*gamePak)->GetEntries().size());
			CHECK((*gamePak)->GetMetadata().Get()["Project"]["Name"] == Json("TestProject"));

			// Game.json: the start scene and the hashes of both paks as written.
			const Result<GameManifest> manifest = GameManifestSerializer::LoadFromFile(output / "Game.json");
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			CHECK(manifest->Name == "TestProject");
			CHECK(manifest->StartScene == *scene);
			CHECK_FALSE(manifest->Testing);
			REQUIRE(manifest->Paks.size() == 2);
			for (const GameManifestPak& pak : manifest->Paks)
			{
				const Result<Buffer> bytes = FileSystem::ReadFile(output / FileSystem::PathFromUtf8(pak.Path));
				REQUIRE(bytes.has_value());
				CHECK(pak.Hash == XXH64(*bytes));
			}
			CHECK(manifest->Paks[0].Path == "Data/Engine.pak");
			CHECK(manifest->Paks[1].Path == "Data/Game.pak");

			// The report lists every file, sorted, with its size and hash.
			std::vector<std::string> listed;
			for (const ExportedFile& file : report.Files)
			{
				listed.push_back(file.Path);
				const Result<Buffer> bytes = FileSystem::ReadFile(output / FileSystem::PathFromUtf8(file.Path));
				REQUIRE(bytes.has_value());
				CHECK(file.Size == bytes->size());
				CHECK(file.Hash == XXH64(*bytes));
			}
			CHECK(std::ranges::is_sorted(listed));
			CHECK(std::ranges::find(listed, "Data/Game.pak") != listed.end());
			CHECK(std::ranges::find(listed, "Game.json") != listed.end());
		}

		TEST_CASE("Exporter: two exports of an unchanged project are identical file for file")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const ExportSpecification specification = MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release));
			std::vector<std::vector<ExportedFile>> exports;
			for (int run = 0; run < 2; ++run)
			{
				Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
				REQUIRE(exporter.has_value());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				exports.push_back((*outcome)->Files);
			}
			REQUIRE(exports[0].size() == exports[1].size());
			for (size_t index = 0; index < exports[0].size(); ++index)
			{
				INFO(exports[0][index].Path);
				CHECK(exports[0][index].Path == exports[1][index].Path);
				CHECK(exports[0][index].Size == exports[1][index].Size);
				CHECK(exports[0][index].Hash == exports[1][index].Hash);
			}
		}

		TEST_CASE("Exporter: a failed export removes its staging directory and leaves an earlier export untouched")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path binaryRoot = MakeBinaryRoot(binaries, ExportConfiguration::Release);
			const std::filesystem::path output = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release));
			WriteTestFile(output / "Earlier.txt", "an earlier export");

			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(binaryRoot));
			REQUIRE(exporter.has_value());
			REQUIRE(RunUntilPhase(**exporter, ExportPhase::CopyRuntime));
			CHECK(ListNames(output.parent_path()).size() == 2); // the earlier export and the staging directory
			// The Runtime disappears between validation and the copy (a build in progress).
			const std::filesystem::path runtime = binaryRoot / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Runtime" / ExecutableName("Runtime");
			REQUIRE(FileSystem::Remove(runtime).has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::NotFound);
			CHECK(outcome->error().ToString().find("while exporting CopyRuntime") != std::string::npos);

			CHECK(ListNames(output.parent_path()) == std::vector<std::string>{ "TestProject" });
			CHECK(ListNames(output) == std::vector<std::string>{ "Earlier.txt" });
		}

		TEST_CASE("Exporter: Cancel removes what the export wrote and leaves an earlier export untouched")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path output = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release));
			WriteTestFile(output / "Earlier.txt", "an earlier export");

			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
			REQUIRE(exporter.has_value());
			REQUIRE(RunUntilPhase(**exporter, ExportPhase::CopyRuntime));
			(*exporter)->Cancel();
			(*exporter)->Cancel(); // idempotent
			CHECK(ListNames(output.parent_path()) == std::vector<std::string>{ "TestProject" });
			CHECK(ListNames(output) == std::vector<std::string>{ "Earlier.txt" });

			// An export cancelled before it wrote anything leaves no Build/ directory behind either.
			Test::EditorTestFixture fresh("ExportCancel");
			MakeExportableProject(fresh);
			Result<Scope<Exporter>> early = Exporter::Start(fresh.GetEditor(), MakeSpecification(binaries / "bin"));
			REQUIRE(early.has_value());
			REQUIRE(RunUntilPhase(**early, ExportPhase::WritePaks));
			REQUIRE_FALSE((*early)->Poll().has_value()); // the paks are being written into the staging directory
			early->reset();
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fresh.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: configurations and phases have their enumerator names")
		{
			CHECK(ExportConfigurationToString(ExportConfiguration::Debug) == "Debug");
			CHECK(ExportConfigurationToString(ExportConfiguration::Release) == "Release");
			CHECK(ExportConfigurationToString(ExportConfiguration::Dist) == "Dist");
			CHECK(ExportPhaseToString(ExportPhase::Validate) == "Validate");
			CHECK(ExportPhaseToString(ExportPhase::SmokeTest) == "SmokeTest");
			CHECK(ExportPhaseToString(ExportPhase::MoveToOutput) == "MoveToOutput");
			CHECK(ExportPhaseToString(ExportPhase::Done) == "Done");
		}

		// M8 (Â§7.5, Â§7.6; Docs/Decisions/0013-m8-decisions.md decision 9): the built-in environments in Engine.pak.

		TEST_CASE("Exporter: without a bake or a GPU the built-in environments are left out of Engine.pak with a warning")
		{
			EngineResourcesEditor fixture;
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const std::vector<std::string> warnings = EnvironmentWarnings(**outcome);
			REQUIRE(warnings.size() == 1);
			CHECK(warnings.front().find("engine://Environments/Studio") != std::string::npos);
			CHECK(warnings.front().find("engine://Environments/Sky") != std::string::npos);
			CHECK(warnings.front().find(EnvironmentImporter::GpuHint) != std::string::npos);

			// The other File built-ins (the Default font and M12's sound-effect presets) and the Generated blue noise (which needs
			// no GPU) are there; the environments are not.
			const Result<Ref<const PakReader>> enginePak = PakReader::Open((*outcome)->OutputDirectory / "Data" / "Engine.pak");
			REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::DefaultFont) != nullptr);
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::BlueNoiseTexture) != nullptr);
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::StudioEnvironment) == nullptr);
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::SkyEnvironment) == nullptr);
			CheckSoundPresets(**enginePak);
		}

		TEST_CASE("Exporter: a reference to a built-in environment that has no bake fails the export with the GPU hint")
		{
			EngineResourcesEditor fixture;
			EditorContext& editor = fixture.GetEditor();
			const Entity world = editor.GetScene().CreateEntity("World");
			world.AddComponent<EnvironmentComponent>().Environment = TypedAssetHandle<AssetType::Environment>(BuiltinAssetHandles::StudioEnvironment);
			SaveScene(editor);
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			REQUIRE(error.GetIssues().size() == 1);
			const ErrorIssue& issue = error.GetIssues().front();
			CHECK(issue.Message.find("engine://Environments/Studio") != std::string::npos);
			CHECK(issue.Message.find(std::string(MainScene)) != std::string::npos);
			CHECK(issue.Hint == EnvironmentImporter::GpuHint);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: the built-in environments baked with the editor's GPU go into Engine.pak" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				EngineResourcesEditor fixture(baker->get());
				EditorContext& editor = fixture.GetEditor();
				const Entity world = editor.GetScene().CreateEntity("World");
				world.AddComponent<EnvironmentComponent>().Environment = TypedAssetHandle<AssetType::Environment>(BuiltinAssetHandles::StudioEnvironment);
				SaveScene(editor);
				const Test::TempDirectory binaries("ExportBinaries");
				Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeBinaryRoot(binaries, ExportConfiguration::Release)));
				REQUIRE(exporter.has_value());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				CHECK(EnvironmentWarnings(**outcome).empty());

				const Result<Ref<const PakReader>> enginePak = PakReader::Open((*outcome)->OutputDirectory / "Data" / "Engine.pak");
				REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
				for (const AssetHandle handle : { BuiltinAssetHandles::StudioEnvironment, BuiltinAssetHandles::SkyEnvironment })
				{
					CAPTURE(handle.ToString());
					const PakEntry* entry = (*enginePak)->FindByHandle(handle);
					REQUIRE(entry != nullptr);
					CHECK(entry->Type == "Environment");
					const Result<Buffer> bytes = (*enginePak)->ReadEntry(*entry);
					REQUIRE(bytes.has_value());
					CHECK(LoadCookedEnvironment(*bytes).has_value());
				}
				// With them, the blue noise of the tonemap's dither (a Generated built-in, decision 8): the 64x64 R8 texture.
				const PakEntry* noise = (*enginePak)->FindByHandle(BuiltinAssetHandles::BlueNoiseTexture);
				REQUIRE(noise != nullptr);
				CHECK(noise->Type == "Texture");
				const Result<Buffer> noiseBytes = (*enginePak)->ReadEntry(*noise);
				REQUIRE(noiseBytes.has_value());
				const Result<AssetRef<TextureData>> texture = LoadCookedTexture(*noiseBytes);
				REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
				CHECK((*texture)->Format == TextureFormat::R8Unorm);
				CHECK((*texture)->Width == 64);
				CHECK((*texture)->Height == 64);
				// And M12's sound-effect presets: one Engine.pak holds the built-ins of both milestones.
				CheckSoundPresets(**enginePak);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
