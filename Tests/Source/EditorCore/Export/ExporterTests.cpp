#include "TestsPCH.h"

#include "EditorCore/Export/Exporter.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "EditorCore/Project/ProjectManager.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IEnvironmentBaker.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"
#include "Engine/Core/BuildId.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Paths.h"
#include "Engine/Project/GameManifest.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/ExportBinaryRoot.h"
#include "Support/FakeNativeBackends.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/PlatformProbes.h"
#include "Support/ScriptOverrideProject.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"
#include "Support/TestOptions.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The exporter (Architecture §14.1, §14.2; Docs/Decisions/0012-m7-decisions.md decision 14, Docs/Decisions/0024-m15-contract.md
// decisions 2 to 8) against a build output directory made from this build's real Runtime output (Support/ExportBinaryRoot.h:
// the executable the export copies and patches, its app-local CRT on Windows or its frameworks on macOS, and the notices
// file) and a few shader files. The smoke test per configuration and the exported game's run are tested by
// Tests/Automation/test_export.py and test_runtime.py against the built binaries.

namespace Engine {

	namespace {

		constexpr std::string_view MainScene = "Assets/Scenes/Main.scene";

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

		// A bin directory with this build's real Runtime output as Scripts/Build.py leaves it (Support/ExportBinaryRoot.h: the
		// executable, its Redist on Windows, its frameworks on macOS, the notices file) and Shaders/ with two files the export
		// packages and a dependency file it leaves out.
		std::filesystem::path MakeRealBinaryRoot(const Test::TempDirectory& directory, const Test::ExportBinaryRootOptions& options = {})
		{
			const std::filesystem::path root = Test::MakeExportBinaryRoot(directory, options);
			const std::filesystem::path output = root / Exporter::GetBuildOutputDirectoryName(options.Configuration);
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

		// The game's executable in the package at `output` (§14.1): "<Name>(.exe)", or on macOS the bundle's
		// "<Name>.app/Contents/MacOS/<Name>" (AppBundle.h).
		std::filesystem::path PackageExecutable(const std::filesystem::path& output, std::string_view name)
		{
#if defined(ENGINE_PLATFORM_MACOS)
			return output / (std::string(name) + ".app") / "Contents" / "MacOS" / std::string(name);
#else
			return output / ExecutableName(name);
#endif
		}

		// Where Game.json, Data/ and the notices are in the package at `output`: `output`, or the bundle's Contents/Resources.
		std::filesystem::path PackageResources(const std::filesystem::path& output, std::string_view name)
		{
#if defined(ENGINE_PLATFORM_MACOS)
			return output / (std::string(name) + ".app") / "Contents" / "Resources";
#else
			static_cast<void>(name);
			return output;
#endif
		}

		// The text of a valid game manifest for "TestProject", as an earlier export left it.
		std::string MakeEarlierManifestText()
		{
			GameManifest manifest;
			manifest.Name = "TestProject";
			manifest.EngineVersion = "0.1.0";
			manifest.Build = "0.1.0+3f2a9c1d4e5b";
			manifest.StartScene = UUID(0x8a61c0d2e4f31b77ull);
			manifest.Paks = { GameManifestPak{ .Path = "Data/Engine.pak", .Hash = 1 }, GameManifestPak{ .Path = "Data/Game.pak", .Hash = 2 } };
			const Result<std::string> text = GameManifestSerializer::SaveToString(manifest);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			return *text;
		}

		// An earlier export in `output` as the output-directory rule recognizes it (ADR 0024 decision 2, the M15 review's
		// M01): its Game.json that loads as a game manifest (in the bundle on macOS) and one more file, Earlier.txt, which a
		// replacing export removes.
		void WriteEarlierExport(const std::filesystem::path& output)
		{
			WriteTestFile(PackageResources(output, "TestProject") / "Game.json", MakeEarlierManifestText());
			WriteTestFile(output / "Earlier.txt", "an earlier export");
		}

		// The names and contents of the regular files below `directory` (recursive), sorted by path.
		std::vector<std::pair<std::string, std::string>> ReadTree(const std::filesystem::path& directory)
		{
			std::vector<std::pair<std::string, std::string>> files;
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory, true);
			REQUIRE(entries.has_value());
			for (const std::filesystem::path& entry : *entries)
			{
				std::error_code error;
				if (!std::filesystem::is_regular_file(entry, error))
					continue;
				const Result<std::string> text = FileSystem::ReadText(entry);
				REQUIRE(text.has_value());
				files.emplace_back(FileSystem::PathToUtf8(entry.lexically_relative(directory)), *text);
			}
			std::ranges::sort(files);
			return files;
		}

		// The names WriteEarlierExport leaves in the output directory, sorted.
		std::vector<std::string> EarlierExportNames()
		{
#if defined(ENGINE_PLATFORM_MACOS)
			return { "Earlier.txt", "TestProject.app" };
#else
			return { "Earlier.txt", "Game.json" };
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

		// M19 (Docs/Decisions/0030-m19-contract.md decision 20): a Behaviour of the project at `root` whose Assets.Load reference
		// is computed, so Engine.pak keeps every File and Generated built-in the export can cook.
		void WriteComputedLoadScript(const std::filesystem::path& root)
		{
			WriteTestFile(root / "Assets/Scripts/Jingle.luau",
				"local Jingle = {}\n"
				"function Jingle.OnStart(self: any)\n"
				"\tlocal clip = Assets.Load(\"engine://Audio/\" .. \"Win\")\n"
				"\tif clip ~= nil then\n"
				"\t\tAudio.PlayOneShot(clip)\n"
				"\tend\n"
				"end\n"
				"return Script.Define(\"Jingle\", Jingle)\n");
		}

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

		// M12's sound-effect presets (engine://Audio/*, File entries cooked by SoundEffectImporter, which needs no GPU).
		constexpr std::array SoundPresets = { BuiltinAssetHandles::ClickSound, BuiltinAssetHandles::BlipSound, BuiltinAssetHandles::CoinSound,
			BuiltinAssetHandles::JumpSound, BuiltinAssetHandles::HitSound, BuiltinAssetHandles::ExplosionSound, BuiltinAssetHandles::PowerUpSound,
			BuiltinAssetHandles::LineClearSound, BuiltinAssetHandles::WinSound, BuiltinAssetHandles::LoseSound };

		// The presets the packaged game reaches (`reached`; M19, Docs/Decisions/0030-m19-contract.md decision 20) are in
		// `enginePak` as AudioClips that load, and the others are not.
		void CheckSoundPresets(const PakReader& enginePak, std::span<const AssetHandle> reached)
		{
			for (const AssetHandle handle : SoundPresets)
			{
				CAPTURE(handle.ToString());
				const PakEntry* entry = enginePak.FindByHandle(handle);
				if (std::ranges::find(reached, handle) == reached.end())
				{
					CHECK(entry == nullptr);
					continue;
				}
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

		TEST_CASE("Exporter: script type errors block export even when play allows them")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			ApplySettings(editor, R"({"Scripting":{"BlockPlayOnTypeErrors":false}})");
			WriteTestFile(fixture.GetProjectRoot() / "Assets/Scripts/Bad.luau", "--!strict\nlocal value: number = \"wrong\"\nreturn {Value = value}\n");
			const Test::TempDirectory binaries("ExportScriptError");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::Validation);
			CHECK(outcome->error().ToString().find("Bad.luau") != std::string::npos);
			CHECK((*exporter)->GetPhase() == ExportPhase::Validate);
			CHECK_FALSE(FileSystem::Exists(fixture.GetProjectRoot() / "Build"));
		}

		// S1-04: the build scene's prefab instance overrides a script field. A valid override exports without a warning (the
		// dry run's export warned 40 times about valid ones); an invalid one is warned about once, at the instance's own field,
		// under the code project.validate reports.
		TEST_CASE("Exporter: a prefab instance's script field override exports without warnings and an invalid one is named as project.validate names it")
		{
			Test::EditorTestFixture fixture("ExportScriptOverride");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			Test::WriteScriptOverrideProject(fixture.GetProjectRoot());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			ApplySettings(editor, R"({"StartScene":"Assets/Scenes/Main.scene","Export":{"BuildScenes":["Assets/Scenes/Main.scene"]}})");
			const Test::TempDirectory binaries("ExportScriptOverrideBinaries");
			const std::filesystem::path binaryRoot = MakeRealBinaryRoot(binaries);
			const auto exportWarnings = [&editor, &binaryRoot]()
			{
				Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(binaryRoot));
				REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				return (*outcome)->Warnings;
			};
			const auto mentions = [](const std::vector<std::string>& warnings, std::string_view text)
			{
				return std::ranges::count_if(warnings, [text](const std::string& warning)
				{
					return warning.contains(text);
				});
			};
			const auto describe = [](const std::vector<std::string>& warnings)
			{
				std::string text;
				for (const std::string& warning : warnings)
					text += warning + "\n";
				return text;
			};

			const std::vector<std::string> valid = exportWarnings();
			INFO(describe(valid));
			CHECK(mentions(valid, "Main.scene") == 0);

			Test::WriteScriptOverrideProject(fixture.GetProjectRoot(), "Pointz");
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const std::vector<std::string> invalid = exportWarnings();
			INFO(describe(invalid));
			REQUIRE(mentions(invalid, "Main.scene") == 1);
			CHECK(mentions(invalid, std::string(Test::ScriptOverrideFieldsPointer) + "Pointz") == 1);
			CHECK(mentions(invalid, "SCRIPT_UNKNOWN_FIELD_OVERRIDE") == 1);
			CHECK(mentions(invalid, Test::ScriptOverrideInstanceID.ToString()) == 1);
			CHECK(mentions(invalid, "/Components/Prefab/") == 0);
		}

		// M16 review R24: the exporter named every unresolved Script.Fields value SCRIPT_UNKNOWN_FIELD_OVERRIDE, also the values
		// of a Module script, which project.validate reports as SCRIPT_NOT_A_BEHAVIOUR and never as an unknown override. It
		// names a value only under the code validate reports for it: a type mismatch as one, a Module's values under their
		// load code.
		TEST_CASE("Exporter: a script field value is named only under the code project.validate reports for it")
		{
			Test::EditorTestFixture fixture("ExportScriptFieldCodes");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			ApplySettings(editor, R"({"StartScene":"Assets/Scenes/Main.scene","Export":{"BuildScenes":["Assets/Scenes/Main.scene"]}})");
			const Test::TempDirectory binaries("ExportScriptFieldCodesBinaries");
			const std::filesystem::path binaryRoot = MakeRealBinaryRoot(binaries);
			// Every line the export reports: its warnings when it succeeds, its issues when it fails.
			const auto exportLines = [&editor, &binaryRoot]()
			{
				Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(binaryRoot));
				REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				std::vector<std::string> lines;
				if (outcome->has_value())
					return (*outcome)->Warnings;
				for (const ErrorIssue& issue : outcome->error().GetIssues())
					lines.push_back(issue.Message + " (hint: " + issue.Hint + ")");
				return lines;
			};
			const auto mentions = [](const std::vector<std::string>& lines, std::string_view text)
			{
				return std::ranges::count_if(lines, [text](const std::string& line)
				{
					return line.contains(text);
				});
			};
			const auto describe = [](const std::vector<std::string>& lines)
			{
				std::string text;
				for (const std::string& line : lines)
					text += line + "\n";
				return text;
			};

			Test::WriteScriptOverrideProject(fixture.GetProjectRoot(), "Points", "\"five\"");
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const std::vector<std::string> mismatched = exportLines();
			INFO(describe(mismatched));
			CHECK(mentions(mismatched, "SCRIPT_FIELD_TYPE_MISMATCH") == 1);
			CHECK(mentions(mismatched, "SCRIPT_UNKNOWN_FIELD_OVERRIDE") == 0);

			for (const std::string_view field : { "Points", "Pointz" })
			{
				CAPTURE(std::string(field));
				Test::WriteScriptOverrideProject(fixture.GetProjectRoot(), field);
				Test::WriteScriptOverrideScript(fixture.GetProjectRoot(), Test::ScriptOverrideModuleSource);
				REQUIRE(editor.GetAssets().Refresh().has_value());
				const std::vector<std::string> module = exportLines();
				INFO(describe(module));
				CHECK(mentions(module, "SCRIPT_UNKNOWN_FIELD_OVERRIDE") == 0);
				CHECK(mentions(module, "SCRIPT_FIELD_TYPE_MISMATCH") == 0);
				CHECK(mentions(module, "correct or remove the override") == 0);
			}
		}

		TEST_CASE("Exporter: invalid checker configuration blocks export instead of skipping type checks")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			ApplySettings(editor, R"({"Scripting":{"BlockPlayOnTypeErrors":false}})");
			WriteTestFile(fixture.GetProjectRoot() / "Assets/Scripts/Bad.luau", "--!strict\nlocal value: number = \"wrong\"\nreturn {Value = value}\n");
			EditorScriptService* scripts = editor.GetScriptService();
			REQUIRE(scripts != nullptr);
			const auto checked = scripts->Check({});
			REQUIRE(checked.has_value());
			CHECK_FALSE(checked->Diagnostics.empty());
			WriteTestFile(fixture.GetProjectRoot() / ".luaurc", "\xff");
			Test::ExpectLog expected(LogLevel::Error, "Cannot refresh script diagnostics");
			const Test::TempDirectory binaries("ExportCheckerConfigurationError");
			auto exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE_MESSAGE(exporter, (exporter ? "" : exporter.error().ToString()));
			const auto outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::InvalidState);
			CHECK(outcome->error().GetMessageText().contains("script diagnostics are unavailable"));
			CHECK((*exporter)->GetPhase() == ExportPhase::Validate);
			CHECK_FALSE(FileSystem::Exists(fixture.GetProjectRoot() / "Build"));
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
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
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
				Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeRealBinaryRoot(binaries)));
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
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeRealBinaryRoot(binaries)));
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
			CHECK((*outcome)->Executable == PackageExecutable(output, name));
			CHECK(FileSystem::Exists((*outcome)->Executable));
			CHECK(ListNames(output.parent_path()) == std::vector<std::string>{ name });
		}

		TEST_CASE("Exporter: a missing Runtime is NotFound naming the build command")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			// Release is built, Dist is not.
			ExportSpecification dist = MakeSpecification(MakeRealBinaryRoot(binaries));
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
			CHECK(Exporter::IsMissingBuildOutput(error));
			CHECK(error.GetLocation().File.contains(Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Dist)));
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
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
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

		// Docs/Decisions/0030-m19-contract.md decision 20 (stream D): a scene's Script field values are references too, read
		// with the editor's script field schemas, so one naming an asset Export.Exclude leaves out fails the export as a
		// component's reference does, with the glob in the hint.
		TEST_CASE("Exporter: a script field naming an asset the export leaves out fails the export")
		{
			Test::EditorTestFixture fixture("ExportScriptFieldReference");
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			const Result<std::string> material = Test::ReadTestDataText("Assets/Materials/Red.material");
			REQUIRE(material.has_value());
			WriteTestFile(fixture.GetProjectRoot() / "Assets/Tests/Look.material", *material);
			WriteTestFile(fixture.GetProjectRoot() / "Assets/Scripts/Painter.luau",
				"local Painter = {Fields = {Look = Field.Asset(\"Material\")}}\n"
				"function Painter.OnStart(self: any)\n"
				"\tif self.Look ~= nil then\n"
				"\t\tLog.Info(\"painted\")\n"
				"\tend\n"
				"end\n"
				"return Script.Define(\"Painter\", Painter)\n");
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const std::optional<AssetHandle> script = editor.GetAssets().Resolve("Assets/Scripts/Painter.luau");
			const std::optional<AssetHandle> look = editor.GetAssets().Resolve("Assets/Tests/Look.material");
			REQUIRE(script.has_value());
			REQUIRE(look.has_value());
			const Entity cube = editor.GetScene().FindEntityByPath("/Cube");
			REQUIRE(cube.IsValid());
			ScriptComponent& painter = cube.AddComponent<ScriptComponent>();
			painter.Script = TypedAssetHandle<AssetType::Script>(*script);
			painter.Fields["Look"] = VariantValue(Json(look->ToString()));
			SaveScene(editor);

			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			REQUIRE(error.GetIssues().size() == 1);
			CHECK(error.GetIssues().front().Message.find("Assets/Tests/Look.material") != std::string::npos);
			CHECK(error.GetIssues().front().Message.find(std::string(MainScene)) != std::string::npos);
			CHECK(error.GetIssues().front().Hint.find("Assets/Tests/**") != std::string::npos);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		// Regression (M19 stream D): a binary glTF imports as a Prefab, but it is no prefab document, so the script field
		// references are read only from the files of the Scene and Prefab importers, never from a .glb, which is not text.
		TEST_CASE("Exporter: a binary glTF exports beside the scenes whose script fields it reads")
		{
			Test::EditorTestFixture fixture("ExportBinaryGltf");
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			const Result<Buffer> glb = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/Box.glb"));
			REQUIRE_MESSAGE(glb.has_value(), glb.error().ToString());
			const Result<VfsPath> path = VfsPath::Create("project", "Assets/Models/Box.glb");
			REQUIRE(path.has_value());
			REQUIRE(editor.WriteProjectFile(*path, *glb).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			REQUIRE(editor.GetAssets().GetAssetType(editor.GetAssets().Resolve("Assets/Models/Box.glb").value_or(AssetHandle())) == AssetType::Prefab);
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const Result<Ref<const PakReader>> gamePak = PakReader::Open(PackageResources((*outcome)->OutputDirectory, "TestProject") / "Data" / "Game.pak");
			REQUIRE_MESSAGE(gamePak.has_value(), gamePak.error().ToString());
			CHECK((*gamePak)->FindByPath("Assets/Models/Box.glb") != nullptr);
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
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const ExportReport& report = **outcome;
			CHECK((*exporter)->GetPhase() == ExportPhase::Done);

			const std::filesystem::path output = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release));
			const std::filesystem::path resources = PackageResources(output, "TestProject");
			CHECK(report.OutputDirectory == output);
			CHECK(report.Executable == PackageExecutable(output, "TestProject"));
			CHECK(report.Configuration == ExportConfiguration::Release);
			CHECK_FALSE(report.SmokeTestRan);
			CHECK_FALSE(report.Testing);
			// Nothing but the output directory is left beside it (no staging directory).
			CHECK(ListNames(output.parent_path()) == std::vector<std::string>{ "TestProject" });

			const Result<Buffer> runtime = FileSystem::ReadFile(binaries / "bin" / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Runtime" / ExecutableName("Runtime"));
			REQUIRE(runtime.has_value());
			const Result<Buffer> executable = FileSystem::ReadFile(report.Executable);
			REQUIRE(executable.has_value());
#if defined(ENGINE_PLATFORM_WINDOWS)
			// The copied Runtime carries the project's version resource (§14.2 step 5); without Export.Icon no icon group.
			CHECK(report.ResourcesPatched);
			const Result<Test::ExecutableResourceProbe> probe = Test::ProbeExecutableResources(report.Executable);
			REQUIRE_MESSAGE(probe.has_value(), probe.error().ToString());
			CHECK(probe->HasManifest);
			CHECK(probe->FileVersion == std::array<uint16_t, 4>{ 1, 0, 0, 0 });
			const auto text = [&probe](const char* key) -> std::string
			{
				const auto found = probe->Strings.find(key);
				return found == probe->Strings.end() ? std::string("<missing>") : found->second;
			};
			CHECK(text("ProductName") == "TestProject");
			CHECK(text("OriginalFilename") == "TestProject.exe");
			CHECK(text("CompanyName") == "<missing>");
			CHECK(probe->Icons.empty());
			std::vector<std::string> expected = { "Data", ExecutableName("TestProject"), "Game.json", "THIRD_PARTY_NOTICES.txt", "msvcp140.dll",
				"vcruntime140.dll", "vcruntime140_1.dll" };
#elif defined(ENGINE_PLATFORM_MACOS)
			// The output directory holds only the bundle (AppBundle.h); its Info.plist carries the version.
			CHECK(report.ResourcesPatched);
			CHECK(*executable == *runtime);
			CHECK(FileSystem::ReadText(output / "TestProject.app" / "Contents" / "Info.plist").value_or("").contains("<string>1.0.0</string>"));
			std::vector<std::string> expected = { "TestProject.app" };
#else
			// Linux executables carry no resources (§16): the Runtime is copied unchanged.
			CHECK_FALSE(report.ResourcesPatched);
			CHECK(*executable == *runtime);
			std::vector<std::string> expected = { "Data", ExecutableName("TestProject"), "Game.json", "THIRD_PARTY_NOTICES.txt" };
#endif
			std::ranges::sort(expected);
			CHECK(ListNames(output) == expected);
			CHECK(FileSystem::ReadText(resources / "THIRD_PARTY_NOTICES.txt").value_or("") == Test::DefaultExportNotices);

			// Engine.pak: the configuration's shader files (not their dependency files) and its metadata.
			const Result<Ref<const PakReader>> enginePak = PakReader::Open(resources / "Data" / "Engine.pak");
			REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
			CHECK((*enginePak)->FindByPath("Shaders/Test/VSMain.spv") != nullptr);
			CHECK((*enginePak)->FindByPath("Shaders/Test/VSMain.refl.json") != nullptr);
			CHECK((*enginePak)->FindByPath("Shaders/Test/VSMain.d") == nullptr);
			CHECK(report.EngineEntryCount == (*enginePak)->GetEntries().size());
			const Json& engineMetadata = (*enginePak)->GetMetadata().Get();
			CHECK(engineMetadata["Configuration"] == Json("Release"));
			CHECK(engineMetadata.contains("EngineVersion"));
			// M19 (Docs/Decisions/0030-m19-contract.md decisions 13 and 18): the exporting build's id, in Engine.pak and Game.json.
			CHECK(engineMetadata["Build"] == Json(FormatBuildId(GetBuildId())));

			// Game.pak: every registered asset (here the scene), with the project settings in its metadata.
			const Result<Ref<const PakReader>> gamePak = PakReader::Open(resources / "Data" / "Game.pak");
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
			const Result<GameManifest> manifest = GameManifestSerializer::LoadFromFile(resources / "Game.json");
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			CHECK(manifest->Name == "TestProject");
			CHECK(manifest->Build == FormatBuildId(GetBuildId()));
			CHECK(manifest->StartScene == *scene);
			CHECK_FALSE(manifest->Testing);
			REQUIRE(manifest->Paks.size() == 2);
			for (const GameManifestPak& pak : manifest->Paks)
			{
				const Result<Buffer> bytes = FileSystem::ReadFile(resources / FileSystem::PathFromUtf8(pak.Path));
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
			const ExportSpecification specification = MakeSpecification(MakeRealBinaryRoot(binaries));
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
			const std::filesystem::path binaryRoot = MakeRealBinaryRoot(binaries);
			const std::filesystem::path output = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release));
			WriteEarlierExport(output);

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
			CHECK(ListNames(output) == EarlierExportNames());
		}

		TEST_CASE("Exporter: Cancel removes what the export wrote and leaves an earlier export untouched")
		{
			Test::EditorTestFixture fixture;
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path output = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(Exporter::GetDefaultOutputDirectory("TestProject", ExportConfiguration::Release));
			WriteEarlierExport(output);

			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			REQUIRE(RunUntilPhase(**exporter, ExportPhase::CopyRuntime));
			(*exporter)->Cancel();
			(*exporter)->Cancel(); // idempotent
			CHECK(ListNames(output.parent_path()) == std::vector<std::string>{ "TestProject" });
			CHECK(ListNames(output) == EarlierExportNames());

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
			CHECK(ExportPhaseToString(ExportPhase::NativeBuild) == "NativeBuild");
			CHECK(ExportPhaseToString(ExportPhase::PatchResources) == "PatchResources");
			CHECK(ExportPhaseToString(ExportPhase::Sign) == "Sign");
			CHECK(ExportPhaseToString(ExportPhase::SmokeTest) == "SmokeTest");
			CHECK(ExportPhaseToString(ExportPhase::MoveToOutput) == "MoveToOutput");
			CHECK(ExportPhaseToString(ExportPhase::Done) == "Done");
		}

		// M8 (§7.5, §7.6; Docs/Decisions/0013-m8-decisions.md decision 9): the built-in environments in Engine.pak.

		TEST_CASE("Exporter: without a bake or a GPU the built-in environments are left out of Engine.pak with a warning")
		{
			// M19 (decision 20): an environment nothing reaches is never cooked, so a computed Assets.Load, which keeps every
			// built-in, is what asks for them here.
			EngineResourcesEditor fixture;
			WriteComputedLoadScript(fixture.GetProjectRoot());
			REQUIRE(fixture.GetEditor().GetAssets().Refresh().has_value());
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeRealBinaryRoot(binaries)));
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
			const Result<Ref<const PakReader>> enginePak = PakReader::Open(PackageResources((*outcome)->OutputDirectory, "TestProject") / "Data" / "Engine.pak");
			REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::DefaultFont) != nullptr);
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::BlueNoiseTexture) != nullptr);
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::StudioEnvironment) == nullptr);
			CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::SkyEnvironment) == nullptr);
			CheckSoundPresets(**enginePak, SoundPresets);
		}

		TEST_CASE("Exporter: a reference to a built-in environment that has no bake fails the export with the GPU hint")
		{
			EngineResourcesEditor fixture;
			EditorContext& editor = fixture.GetEditor();
			const Entity world = editor.GetScene().CreateEntity("World");
			world.AddComponent<EnvironmentComponent>().Environment = TypedAssetHandle<AssetType::Environment>(BuiltinAssetHandles::StudioEnvironment);
			SaveScene(editor);
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
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
				// M12's Coin preset, so one Engine.pak holds the built-ins of both milestones.
				world.AddComponent<AudioSourceComponent>().Clip = TypedAssetHandle<AssetType::AudioClip>(BuiltinAssetHandles::CoinSound);
				SaveScene(editor);
				const Test::TempDirectory binaries("ExportBinaries");
				Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
				REQUIRE(exporter.has_value());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				CHECK(EnvironmentWarnings(**outcome).empty());

				const Result<Ref<const PakReader>> enginePak = PakReader::Open(PackageResources((*outcome)->OutputDirectory, "TestProject") / "Data" / "Engine.pak");
				REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
				// The referenced Studio is baked and goes in; Sky, which nothing reaches, stays out (M19, decision 20).
				const PakEntry* studio = (*enginePak)->FindByHandle(BuiltinAssetHandles::StudioEnvironment);
				REQUIRE(studio != nullptr);
				CHECK(studio->Type == "Environment");
				const Result<Buffer> studioBytes = (*enginePak)->ReadEntry(*studio);
				REQUIRE(studioBytes.has_value());
				CHECK(LoadCookedEnvironment(*studioBytes).has_value());
				CHECK((*enginePak)->FindByHandle(BuiltinAssetHandles::SkyEnvironment) == nullptr);
				CHECK(std::ranges::find((*outcome)->Reachability.PrunedBuiltins, "engine://Environments/Sky") != (*outcome)->Reachability.PrunedBuiltins.end());
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
				// And M12's Coin preset: one Engine.pak holds the built-ins of both milestones.
				CheckSoundPresets(**enginePak, std::array{ BuiltinAssetHandles::CoinSound });
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M15 (Docs/Decisions/0024-m15-contract.md decisions 2 to 8, stream A). These cases export from this build's real
		// Runtime output (MakeRealBinaryRoot, Support/ExportBinaryRoot.h), because PatchResources needs a real executable,
		// the notices file becomes required and macOS signs real frameworks. Stream A moves the M7 to M14 cases above from
		// MakeBinaryRoot's fake executable to it in the same change that makes those steps real.

		TEST_CASE("Exporter: the package carries THIRD_PARTY_NOTICES.txt from the build output")
		{
			Test::EditorTestFixture fixture("ExportNotices");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			constexpr std::string_view Notices = "Third-party notices of this build\n";
			const std::filesystem::path root = MakeRealBinaryRoot(binaries, { .Notices = std::string(Notices) });
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(root));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
#if defined(ENGINE_PLATFORM_MACOS)
			const std::filesystem::path notices = (*outcome)->OutputDirectory / "TestProject.app" / "Contents" / "Resources" / "THIRD_PARTY_NOTICES.txt";
#else
			const std::filesystem::path notices = (*outcome)->OutputDirectory / "THIRD_PARTY_NOTICES.txt";
#endif
			const Result<std::string> text = FileSystem::ReadText(notices);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(*text == Notices);
			CHECK(std::ranges::any_of((*outcome)->Files, [](const ExportedFile& file)
			{
				return file.Path.ends_with("THIRD_PARTY_NOTICES.txt");
			}));
		}

		TEST_CASE("Exporter: a missing notices file is NotFound naming the build command")
		{
			Test::EditorTestFixture fixture("ExportNoNotices");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeRealBinaryRoot(binaries, { .Notices = std::nullopt })));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::NotFound);
			CHECK(error.GetMessageText().find("THIRD_PARTY_NOTICES.txt") != std::string::npos);
			CHECK(error.GetMessageText().find("python Scripts/Build.py --config Release --project Runtime") != std::string::npos);
			CHECK(Exporter::IsMissingBuildOutput(error));
			CHECK(error.GetLocation().File.ends_with("THIRD_PARTY_NOTICES.txt"));
		}

		TEST_CASE("Exporter: only a missing build output counts as one")
		{
			// Editor --export exits 3 for these and 1 for every other failure (ADR 0024 decision 3, review finding n3).
			Test::EditorTestFixture fixture("ExportMissingOutput");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path root = MakeRealBinaryRoot(binaries);
			const std::filesystem::path runtime = root / Exporter::GetBuildOutputDirectoryName(ExportConfiguration::Release) / "Runtime";
			const Result<std::vector<std::filesystem::path>> files = FileSystem::ListDirectory(runtime, false);
			REQUIRE(files.has_value());
			for (const std::filesystem::path& file : *files)
			{
				if (file.stem() == "Runtime")
					REQUIRE(FileSystem::Remove(file).has_value());
			}
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(root));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			INFO(outcome->error().ToString());
			CHECK(outcome->error().GetCode() == ErrorCode::NotFound);
			CHECK(Exporter::IsMissingBuildOutput(outcome->error()));

			// A validation failure, and a NotFound that names no build output, are not.
			CHECK_FALSE(Exporter::IsMissingBuildOutput(Error(ErrorCode::NotFound, "the asset 'Assets/Gone.png' does not exist")));
			CHECK_FALSE(Exporter::IsMissingBuildOutput(Error(ErrorCode::Validation, "the project does not export")));
		}

		TEST_CASE("Exporter: an editor without a GPU refuses a project environment without a cooked bake at validation, with the GPU hint")
		{
			// A project .hdr that nothing loaded yet: it arrives with its .meta (as from version control), so registering it
			// imports nothing and no asset diagnostic exists, and before M15 the export failed in Cook with Unsupported.
			// Validation now names the asset (§14.2 step 1), reading Library/Cache without importing, so nothing is logged.
			Test::EditorTestFixture fixture("ExportProjectEnvironment");
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			const Result<Buffer> hdr = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Environments/Gradient.hdr"));
			REQUIRE_MESSAGE(hdr.has_value(), hdr.error().ToString());
			const Result<VfsPath> metaPath = VfsPath::Create("project", "Assets/Sky.hdr.meta");
			REQUIRE(metaPath.has_value());
			constexpr std::string_view Meta = R"({
	"Format": "AssetMeta",
	"Version": 1,
	"Handle": "7a3c51e0b2d94f16",
	"Type": "Environment",
	"Importer": "Environment",
	"ImporterVersion": 1,
	"Settings": {
		"ClampLuminance": false,
		"MaxLuminance": 1000
	},
	"SubAssets": []
}
)";
			REQUIRE(editor.WriteProjectFile(*metaPath, AsBytes(Meta)).has_value());
			const Result<VfsPath> path = VfsPath::Create("project", "Assets/Sky.hdr");
			REQUIRE(path.has_value());
			REQUIRE(editor.WriteProjectFile(*path, *hdr).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			REQUIRE(editor.GetAssets().GetDiagnostics().empty());
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			CHECK((*exporter)->GetPhase() == ExportPhase::Validate);
			REQUIRE(error.GetIssues().size() == 1);
			CHECK(error.GetIssues().front().Message.find("Assets/Sky.hdr") != std::string::npos);
			CHECK(error.GetIssues().front().Message.find("cooked bake") != std::string::npos);
			CHECK(error.GetIssues().front().Hint == EnvironmentImporter::GpuHint);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));
		}

		TEST_CASE("Exporter: an editor without a GPU exports a project environment from the bake a GPU editor cooked" * doctest::test_suite(Test::GpuSuite))
		{
			// §14.2 step 1 "using cooked environment bakes" (ADR 0024 decision 2): the bake a GPU editor stored in Library/Cache
			// is what a --renderer none editor exports. The device-less editor gets the project's Assets/Sky.hdr, its .meta and
			// Library/Cache from the GPU editor's project. The M7 exporter already cooks from that bake, so the contract adds
			// this case unskipped: it keeps the behaviour while stream A moves validation ahead of Cook.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Result<Buffer> hdr = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Environments/Gradient.hdr"));
			REQUIRE_MESSAGE(hdr.has_value(), hdr.error().ToString());
			const std::vector<std::string_view> carried = { "Assets/Sky.hdr", "Assets/Sky.hdr.meta", "Library/Cache" };
			const Test::TempDirectory baked("ExportBakedProject");
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				EngineResourcesEditor fixture(baker->get());
				EditorContext& editor = fixture.GetEditor();
				const Result<VfsPath> path = VfsPath::Create("project", "Assets/Sky.hdr");
				REQUIRE(path.has_value());
				REQUIRE(editor.WriteProjectFile(*path, *hdr).has_value());
				REQUIRE(editor.GetAssets().Refresh().has_value());
				SaveScene(editor);
				const Test::TempDirectory binaries("ExportBinaries");
				Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
				REQUIRE(exporter.has_value());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				for (const std::string_view relative : carried)
				{
					const std::filesystem::path target = baked / relative;
					REQUIRE(FileSystem::CreateDirectories(target.parent_path()).has_value());
					std::error_code error;
					std::filesystem::copy(fixture.GetProjectRoot() / FileSystem::PathFromUtf8(relative), target, std::filesystem::copy_options::recursive, error);
					REQUIRE_MESSAGE(!error, relative, ": ", error.message());
				}
			}
			gpu.GetDevice().RunGarbageCollection();

			EngineResourcesEditor fixture;
			EditorContext& editor = fixture.GetEditor();
			for (const std::string_view relative : carried)
			{
				const std::filesystem::path target = fixture.GetProjectRoot() / FileSystem::PathFromUtf8(relative);
				REQUIRE(FileSystem::CreateDirectories(target.parent_path()).has_value());
				std::error_code error;
				std::filesystem::copy(baked / relative, target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
				REQUIRE_MESSAGE(!error, relative, ": ", error.message());
			}
			REQUIRE(editor.GetAssets().Refresh().has_value());
			SaveScene(editor);
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const Result<Ref<const PakReader>> gamePak = PakReader::Open(PackageResources((*outcome)->OutputDirectory, "TestProject") / "Data" / "Game.pak");
			REQUIRE_MESSAGE(gamePak.has_value(), gamePak.error().ToString());
			CHECK(std::ranges::any_of((*gamePak)->GetEntries(), [](const PakEntry& entry)
			{
				return entry.Type == "Environment" && entry.Path == "Assets/Sky.hdr";
			}));
		}

		TEST_CASE("Exporter: OutputPath exports into an absolute package directory and ExportRoot keeps OutputDirectory below it")
		{
			Test::EditorTestFixture fixture("ExportOutputPath");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path root = MakeRealBinaryRoot(binaries);
			const Test::TempDirectory target("ExportTarget");

			ExportSpecification direct = MakeSpecification(root);
			direct.OutputPath = target / "Packages" / "Game";
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), direct);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			CHECK((*outcome)->OutputDirectory == target / "Packages" / "Game");
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));

			// The earlier export there is replaced by the next one.
			exporter = Exporter::Start(fixture.GetEditor(), direct);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());

			ExportSpecification rooted = MakeSpecification(root);
			rooted.ExportRoot = target / "Root";
			rooted.OutputDirectory = "Build/Custom/Game";
			exporter = Exporter::Start(fixture.GetEditor(), rooted);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			CHECK((*outcome)->OutputDirectory == target / "Root" / "Custom" / "Game");
		}

		TEST_CASE("Exporter: OutputPath never replaces a source tree, a profile or a directory that holds no earlier export")
		{
			// Review finding M1 (ADR 0024 decision 3): a typo such as "Editor --export ." must never delete a tree.
			Test::EditorTestFixture fixture("ExportOutputPathGuards");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path root = MakeRealBinaryRoot(binaries);
			std::error_code error;
			const std::filesystem::path workingDirectory = std::filesystem::current_path(error);
			REQUIRE_FALSE(error);
			const Result<std::filesystem::path> userData = Paths::GetUserDataRoot();
			REQUIRE_MESSAGE(userData.has_value(), userData.error().ToString());
			const std::vector<std::filesystem::path> refused = {
				"Packages/Game",                        // relative
				fixture.GetProjectRoot(),               // the project
				fixture.GetProjectRoot().parent_path(), // an ancestor of the project
				root,                                   // BinaryRoot
				binaries.GetPath(),                     // an ancestor of BinaryRoot
				workingDirectory,                       // the working directory
				*userData,                              // the user-data root
				fixture.GetProjectRoot().root_path(),   // a file-system root
			};
			for (const std::filesystem::path& outputPath : refused)
			{
				CAPTURE(FileSystem::PathToUtf8(outputPath));
				ExportSpecification specification = MakeSpecification(root);
				specification.OutputPath = outputPath;
				const Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
				REQUIRE_FALSE(exporter.has_value());
				CHECK(exporter.error().GetCode() == ErrorCode::InvalidArgument);
			}

			// An existing directory that holds files but no earlier export is refused before anything is written, and kept.
			const Test::TempDirectory target("ExportGuardTarget");
			const std::filesystem::path occupied = target / "Notes";
			WriteTestFile(occupied / "Todo.txt", "keep me");
			ExportSpecification specification = MakeSpecification(root);
			specification.OutputPath = occupied;
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(outcome->error().GetMessageText().find(FileSystem::PathToUtf8(occupied)) != std::string::npos);
			const Result<std::string> kept = FileSystem::ReadText(occupied / "Todo.txt");
			REQUIRE(kept.has_value());
			CHECK(*kept == "keep me");

			// An empty existing directory is fine.
			REQUIRE(FileSystem::CreateDirectories(target / "Empty").has_value());
			specification.OutputPath = target / "Empty";
			exporter = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			const std::optional<Result<ExportReport>> exported = RunToEnd(**exporter);
			REQUIRE(exported.has_value());
			REQUIRE_MESSAGE(exported->has_value(), exported->error().ToString());
		}

		// The M15 review's M01: a file named Game.json proves an earlier export only when it loads as a game manifest. NTFS
		// and APFS match any spelling of the name, so an unrelated folder's "game.json" would otherwise get the folder moved
		// aside and deleted.
		TEST_CASE("Exporter: a directory whose Game.json is not a game manifest holds no earlier export, so it is refused and kept")
		{
			Test::EditorTestFixture fixture("ExportNotManifest");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path root = MakeRealBinaryRoot(binaries);
			const Test::TempDirectory target("ExportNotManifestTarget");
			const std::vector<std::pair<std::string, std::string>> occupants = {
				{ "Game.json", "{}" },
				{ "game.json", R"({"name": "a web game", "version": 3})" },
			};
			for (size_t index = 0; index < occupants.size(); ++index)
			{
				const auto& [name, text] = occupants[index];
				CAPTURE(name);
				const std::filesystem::path occupied = target.GetPath() / std::format("Occupied{}", index);
				WriteTestFile(occupied / FileSystem::PathFromUtf8(name), text);
				WriteTestFile(occupied / "index.html", "<html></html>");
				const std::vector<std::pair<std::string, std::string>> before = ReadTree(occupied);
				ExportSpecification specification = MakeSpecification(root);
				specification.OutputPath = occupied;
				Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
				REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
				const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_FALSE(outcome->has_value());
				CHECK(outcome->error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(outcome->error().GetMessageText().contains(FileSystem::PathToUtf8(occupied)));
				CHECK((*exporter)->GetPhase() == ExportPhase::Validate);
				CHECK(ReadTree(occupied) == before);
				// Nothing else was written beside it either (no staging directory is left).
				CHECK(ListNames(target.GetPath()).size() == index + 1);
			}
		}

		// The M15 review's M01: the output directory may change during the cook, so the move checks the rule again and keeps
		// what is there now instead of removing it.
		TEST_CASE("Exporter: the move into the output directory checks the output directory rule again and keeps files that appeared during the export")
		{
			Test::EditorTestFixture fixture("ExportRecheck");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			const Test::TempDirectory target("ExportRecheckTarget");
			const std::filesystem::path output = target / "Game";
			ExportSpecification specification = MakeSpecification(MakeRealBinaryRoot(binaries));
			specification.OutputPath = output;
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			REQUIRE(RunUntilPhase(**exporter, ExportPhase::CopyRuntime));
			// Validate found no output directory; something else creates it with files before the export moves its package in.
			WriteTestFile(output / "Notes.txt", "keep me");
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_FALSE(outcome->has_value());
			CHECK(outcome->error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(outcome->error().ToString().contains("while exporting MoveToOutput"));
			CHECK(ReadTree(output) == std::vector<std::pair<std::string, std::string>>{ { "Notes.txt", "keep me" } });
			// The staging directory is gone too: only the output directory is left beside it.
			CHECK(ListNames(target.GetPath()) == std::vector<std::string>{ "Game" });
		}

		// M16 friction S2-06: SmokeTest is listed only when the smoke test ran; without one its time counts toward MoveToOutput.
		TEST_CASE("Exporter: the report times every phase entered, in order")
		{
			Test::EditorTestFixture fixture("ExportTimings");
			MakeExportableProject(fixture);
			const Test::TempDirectory binaries("ExportBinaries");
			Result<Scope<Exporter>> exporter = Exporter::Start(fixture.GetEditor(), MakeSpecification(MakeRealBinaryRoot(binaries)));
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			std::vector<ExportPhase> phases;
			double total = 0.0;
			for (const ExportPhaseTiming& timing : (*outcome)->PhaseTimings)
			{
				phases.push_back(timing.Phase);
				CHECK(timing.Seconds >= 0.0);
				total += timing.Seconds;
			}
			CHECK_FALSE((*outcome)->SmokeTestRan);
			CHECK(phases == std::vector<ExportPhase>{ ExportPhase::Validate, ExportPhase::Cook, ExportPhase::WritePaks, ExportPhase::CopyRuntime, ExportPhase::PatchResources, ExportPhase::WriteManifest, ExportPhase::Sign, ExportPhase::MoveToOutput });
			CHECK(total <= (*outcome)->Seconds + 1e-6);
		}

		// Docs/Decisions/0030-m19-contract.md decisions 19 and 20 (B-PRUNE-ENG, stream D): Engine.pak holds the built-ins the
		// Runtime asks for by handle and those the packaged game reaches: through a component's reference, a script field's
		// value (read with the script's field schema) and a literal Assets.Load; no other built-in is cooked, so an
		// unreferenced environment needs no bake and warns about none. The report gives each kept built-in's reason and
		// lists the rest. A computed Assets.Load in a packaged script keeps every built-in the export can cook.
		TEST_CASE("Exporter: Engine.pak holds the built-ins the game reaches or the Runtime asks for, and the report names the rest")
		{
			EngineResourcesEditor fixture;
			EditorContext& editor = fixture.GetEditor();
			const auto writeScript = [&fixture, &editor](std::string_view load)
			{
				WriteTestFile(fixture.GetProjectRoot() / "Assets/Scripts/Sounds.luau",
					std::format("local Sounds = {{Fields = {{Clip = Field.Asset(\"AudioClip\")}}}}\n"
								"function Sounds.OnStart(self: any)\n"
								"\tlocal hit = Assets.Load({})\n"
								"\tif hit ~= nil and self.Clip ~= nil then\n"
								"\t\tAudio.PlayOneShot(hit)\n"
								"\tend\n"
								"end\n"
								"return Script.Define(\"Sounds\", Sounds)\n",
						load));
				REQUIRE(editor.GetAssets().Refresh().has_value());
			};
			writeScript("\"engine://Audio/Hit\"");
			const std::optional<AssetHandle> script = editor.GetAssets().Resolve("Assets/Scripts/Sounds.luau");
			REQUIRE(script.has_value());
			// The Coin preset through a component, Jump through the script's field, Hit through the literal load.
			const Entity speaker = editor.GetScene().CreateEntity("Speaker");
			speaker.AddComponent<AudioSourceComponent>().Clip = TypedAssetHandle<AssetType::AudioClip>(BuiltinAssetHandles::CoinSound);
			ScriptComponent& sounds = speaker.AddComponent<ScriptComponent>();
			sounds.Script = TypedAssetHandle<AssetType::Script>(*script);
			sounds.Fields["Clip"] = VariantValue(Json(BuiltinAssetHandles::JumpSound.ToString()));
			SaveScene(editor);

			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path binaryRoot = MakeRealBinaryRoot(binaries);
			const auto exportGame = [&editor, &binaryRoot]()
			{
				Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(binaryRoot));
				REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
				std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				return std::move(**outcome);
			};
			const auto packagedBuiltins = [](const ExportReport& report)
			{
				const Result<Ref<const PakReader>> enginePak = PakReader::Open(PackageResources(report.OutputDirectory, "TestProject") / "Data" / "Engine.pak");
				REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
				std::vector<AssetHandle> handles;
				for (const PakEntry& entry : (*enginePak)->GetEntries())
				{
					if (entry.Handle.IsValid())
						handles.push_back(entry.Handle);
				}
				return handles;
			};
			const auto findBuiltin = [](const ExportReport& report, std::string_view path) -> const ExportReachedAsset*
			{
				const auto found = std::ranges::find(report.Reachability.EngineBuiltins, path, &ExportReachedAsset::Path);
				return found == report.Reachability.EngineBuiltins.end() ? nullptr : &*found;
			};

			const ExportReport referenced = exportGame();
			CHECK(EnvironmentWarnings(referenced).empty());
			CHECK(packagedBuiltins(referenced)
				== std::vector<AssetHandle>{ BuiltinAssetHandles::BlueNoiseTexture, BuiltinAssetHandles::DefaultFont, BuiltinAssetHandles::CoinSound,
					BuiltinAssetHandles::JumpSound, BuiltinAssetHandles::HitSound });
			CHECK_FALSE(referenced.Reachability.Pruned);
			CHECK(referenced.Reachability.ComputedLoads.empty());
			CHECK(referenced.Reachability.EngineBuiltins.size() == 5);
			CHECK(std::ranges::is_sorted(referenced.Reachability.EngineBuiltins, {}, &ExportReachedAsset::Path));
			for (const std::string_view required : { "engine://Fonts/Default", "engine://Textures/BlueNoise" })
			{
				CAPTURE(required);
				const ExportReachedAsset* builtin = findBuiltin(referenced, required);
				REQUIRE(builtin != nullptr);
				CHECK(builtin->Reason == ExportReachReason::Required);
			}
			for (const std::string_view dependency : { "engine://Audio/Coin", "engine://Audio/Jump" })
			{
				CAPTURE(dependency);
				const ExportReachedAsset* builtin = findBuiltin(referenced, dependency);
				REQUIRE(builtin != nullptr);
				CHECK(builtin->Reason == ExportReachReason::Dependency);
				CHECK(builtin->From == MainScene);
			}
			const ExportReachedAsset* hit = findBuiltin(referenced, "engine://Audio/Hit");
			REQUIRE(hit != nullptr);
			CHECK(hit->Reason == ExportReachReason::LiteralLoad);
			CHECK(hit->From == "Assets/Scripts/Sounds.luau");
			CHECK(hit->Line == 3);
			for (const std::string_view pruned : { "engine://Audio/Click", "engine://Environments/Sky", "engine://Environments/Studio" })
			{
				CAPTURE(pruned);
				CHECK(std::ranges::find(referenced.Reachability.PrunedBuiltins, pruned) != referenced.Reachability.PrunedBuiltins.end());
			}

			// A computed load may name any built-in: Engine.pak keeps every one this editor can cook, and the environments,
			// which it cannot bake without a GPU, keep their warning.
			writeScript("\"engine://Audio/\" .. \"Win\"");
			const ExportReport computed = exportGame();
			REQUIRE(computed.Reachability.ComputedLoads.size() == 1);
			CHECK(computed.Reachability.ComputedLoads.front().Script == "Assets/Scripts/Sounds.luau");
			CHECK(computed.Reachability.ComputedLoads.front().Line == 3);
			CHECK(EnvironmentWarnings(computed).size() == 1);
			const Result<Ref<const PakReader>> enginePak = PakReader::Open(PackageResources(computed.OutputDirectory, "TestProject") / "Data" / "Engine.pak");
			REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
			CheckSoundPresets(**enginePak, SoundPresets);
			const ExportReachedAsset* win = findBuiltin(computed, "engine://Audio/Win");
			REQUIRE(win != nullptr);
			CHECK(win->Reason == ExportReachReason::ComputedLoad);
			CHECK(win->From == "Assets/Scripts/Sounds.luau");
			CHECK(win->Line == 3);
		}

		// Docs/Decisions/0030-m19-contract.md decision 21 (B-PRUNE-GAME, stream R): with Export.PruneUnreferenced, Game.pak holds
		// only what StartScene and Export.BuildScenes reach: a component's reference, a script and its field's value, a scene
		// a literal Assets.Load names; an unreferenced asset and one only a computed load names are left out, and the report
		// gives each packaged asset's reason, the assets left out and the computed load. Off, everything ships again.
		TEST_CASE("Exporter: Export.PruneUnreferenced packages only what the build scenes reach, and the report says why" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ExportPruned");
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			const Result<std::string> material = Test::ReadTestDataText("Assets/Materials/Red.material");
			REQUIRE(material.has_value());
			for (const std::string_view name : { "Used", "Field", "Computed", "Unused" })
				WriteTestFile(fixture.GetProjectRoot() / std::format("Assets/Materials/{}.material", name), *material);
			const Result<std::string> second = SceneSerializer::SaveToString(*editor.CreateScene("Second"));
			REQUIRE(second.has_value());
			WriteTestFile(fixture.GetProjectRoot() / "Assets/Scenes/Second.scene", *second);
			WriteTestFile(fixture.GetProjectRoot() / "Assets/Scripts/Loader.luau",
				"local Loader = {Fields = {Look = Field.Asset(\"Material\")}}\n"
				"function Loader.OnStart(self: any)\n"
				"\tlocal second = Assets.Load(\"Assets/Scenes/Second.scene\")\n"
				"\tlocal computed = Assets.Load(\"Assets/Materials/\" .. \"Computed.material\")\n"
				"\tif second ~= nil and computed ~= nil and self.Look ~= nil then\n"
				"\t\tScene.Load(second)\n"
				"\tend\n"
				"end\n"
				"return Script.Define(\"Loader\", Loader)\n");
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const auto handleOf = [&editor](std::string_view path)
			{
				const std::optional<AssetHandle> handle = editor.GetAssets().Resolve(path);
				REQUIRE_MESSAGE(handle.has_value(), path);
				return *handle;
			};
			const Entity cube = editor.GetScene().FindEntityByPath("/Cube");
			REQUIRE(cube.IsValid());
			cube.GetComponent<MeshRendererComponent>().Materials = { TypedAssetHandle<AssetType::Material>(handleOf("Assets/Materials/Used.material")) };
			ScriptComponent& loader = cube.AddComponent<ScriptComponent>();
			loader.Script = TypedAssetHandle<AssetType::Script>(handleOf("Assets/Scripts/Loader.luau"));
			loader.Fields["Look"] = VariantValue(Json(handleOf("Assets/Materials/Field.material").ToString()));
			SaveScene(editor);
			ApplySettings(editor, R"({"Export":{"PruneUnreferenced":true}})");

			const Test::TempDirectory binaries("ExportBinaries");
			const std::filesystem::path binaryRoot = MakeRealBinaryRoot(binaries);
			const auto exportGame = [&editor, &binaryRoot]()
			{
				Result<Scope<Exporter>> exporter = Exporter::Start(editor, MakeSpecification(binaryRoot));
				REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
				std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				return std::move(**outcome);
			};
			const ExportReport pruned = exportGame();
			const ExportReachability& reach = pruned.Reachability;
			CHECK(reach.Pruned);
			const auto reasonOf = [&reach](std::string_view path) -> const ExportReachedAsset*
			{
				const auto found = std::ranges::find(reach.GameAssets, path, &ExportReachedAsset::Path);
				return found == reach.GameAssets.end() ? nullptr : &*found;
			};
			std::vector<std::string> packaged;
			for (const ExportReachedAsset& asset : reach.GameAssets)
				packaged.push_back(asset.Path);
			CHECK(packaged
				== std::vector<std::string>{ "Assets/Materials/Field.material", "Assets/Materials/Used.material", "Assets/Scenes/Main.scene", "Assets/Scenes/Second.scene",
					"Assets/Scripts/Loader.luau" });
			CHECK(reach.PrunedAssets == std::vector<std::string>{ "Assets/Materials/Computed.material", "Assets/Materials/Unused.material" });
			REQUIRE(reasonOf("Assets/Scenes/Main.scene") != nullptr);
			CHECK(reasonOf("Assets/Scenes/Main.scene")->Reason == ExportReachReason::StartScene);
			for (const std::string_view dependency : { "Assets/Materials/Used.material", "Assets/Materials/Field.material", "Assets/Scripts/Loader.luau" })
			{
				CAPTURE(dependency);
				REQUIRE(reasonOf(dependency) != nullptr);
				CHECK(reasonOf(dependency)->Reason == ExportReachReason::Dependency);
				CHECK(reasonOf(dependency)->From == MainScene);
			}
			REQUIRE(reasonOf("Assets/Scenes/Second.scene") != nullptr);
			CHECK(reasonOf("Assets/Scenes/Second.scene")->Reason == ExportReachReason::LiteralLoad);
			CHECK(reasonOf("Assets/Scenes/Second.scene")->From == "Assets/Scripts/Loader.luau");
			CHECK(reasonOf("Assets/Scenes/Second.scene")->Line == 3);
			REQUIRE(reach.ComputedLoads.size() == 1);
			CHECK(reach.ComputedLoads.front().Script == "Assets/Scripts/Loader.luau");
			CHECK(reach.ComputedLoads.front().Line == 4);
			CHECK(pruned.GameAssetCount == 5);
			const Result<Ref<const PakReader>> gamePak = PakReader::Open(PackageResources(pruned.OutputDirectory, "TestProject") / "Data" / "Game.pak");
			REQUIRE_MESSAGE(gamePak.has_value(), gamePak.error().ToString());
			CHECK((*gamePak)->FindByHandle(handleOf("Assets/Materials/Unused.material")) == nullptr);
			CHECK((*gamePak)->FindByHandle(handleOf("Assets/Materials/Computed.material")) == nullptr);
			CHECK((*gamePak)->FindByHandle(handleOf("Assets/Scenes/Second.scene")) != nullptr);

			ApplySettings(editor, R"({"Export":{"PruneUnreferenced":false}})");
			const ExportReport full = exportGame();
			CHECK_FALSE(full.Reachability.Pruned);
			CHECK(full.Reachability.GameAssets.empty());
			CHECK(full.Reachability.PrunedAssets.empty());
			CHECK(full.GameAssetCount == 7);
		}

		// Docs/Decisions/0030-m19-contract.md decision 33 (C-NATIVE-STATIC, stream N-A): a game with Native/ is refused first,
		// before anything is written, while the user's preference is off (Unsupported on Linux and macOS); allowed, step 3a
		// builds its Runtime of the export's configuration with the module linked, the package holds that executable, and a
		// build that does not succeed fails the export with CompileFailed and its diagnostics. A game without Native/ never
		// enters the step (the timing test above).
		TEST_CASE("Exporter: a game with Native/ packages a Runtime linked with its module, and is refused first while native code is not allowed" * doctest::skip(true))
		{
			const Ref<Test::FakeNativeBuildBackend> builds = CreateRef<Test::FakeNativeBuildBackend>();
			Test::EditorTestFixture fixture("ExportNative", {}, nullptr, std::nullopt, false,
				Test::EditorTestNativeBackends{ .Build = builds, .Loader = CreateRef<Test::FakeNativeLibraryLoader>() });
			MakeExportableProject(fixture);
			EditorContext& editor = fixture.GetEditor();
			WriteTestFile(fixture.GetProjectRoot() / "Native" / "Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n");
			builds->SetApiHeader(MakeEditorNativeModuleHostSpecification(nullptr, nullptr).ApiHeader);
			const Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			builds->SetRuntimeExecutable(*runtime);
			const Test::TempDirectory binaries("ExportBinaries");
			const ExportSpecification specification = MakeSpecification(MakeRealBinaryRoot(binaries));

			Result<Scope<Exporter>> refused = Exporter::Start(editor, specification);
			REQUIRE(refused.has_value());
			const std::optional<Result<ExportReport>> refusal = RunToEnd(**refused);
			REQUIRE(refusal.has_value());
			REQUIRE_FALSE(refusal->has_value());
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(refusal->error().GetCode() == ErrorCode::PermissionDenied);
#else
			CHECK(refusal->error().GetCode() == ErrorCode::Unsupported);
#endif
			CHECK(builds->GetStartCount() == 0);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));

#if defined(ENGINE_PLATFORM_WINDOWS)
			editor.GetNativeModule().QueuePreferenceChange(true);
			editor.GetNativeModule().Update(editor);
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, specification);
			REQUIRE(exporter.has_value());
			const std::optional<Result<ExportReport>> outcome = RunToEnd(**exporter);
			REQUIRE(outcome.has_value());
			REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
			const ExportReport& report = **outcome;
			CHECK(report.NativeModule);
			CHECK(builds->GetStartCount() == 1);
			CHECK(builds->GetLastRequest().Kind == NativeBuildKind::Runtime);
			CHECK(builds->GetLastRequest().Configuration == ExportConfiguration::Release);
			std::vector<ExportPhase> phases;
			for (const ExportPhaseTiming& timing : report.PhaseTimings)
				phases.push_back(timing.Phase);
			const auto native = std::ranges::find(phases, ExportPhase::NativeBuild);
			REQUIRE(native != phases.end());
			CHECK(*(native - 1) == ExportPhase::WritePaks);
			CHECK(*(native + 1) == ExportPhase::CopyRuntime);
			CHECK(std::filesystem::is_regular_file(report.Executable, exists));

			builds->SetDiagnostics({ NativeBuildDiagnostic{ .Severity = DiagnosticSeverity::Error, .Code = "NATIVE_COMPILE_ERROR", .File = "Native/Spinner.cpp", .Line = 1, .Column = 1, .Message = "an error" } });
			Result<Scope<Exporter>> failing = Exporter::Start(editor, specification);
			REQUIRE(failing.has_value());
			const std::optional<Result<ExportReport>> failed = RunToEnd(**failing);
			REQUIRE(failed.has_value());
			REQUIRE_FALSE(failed->has_value());
			CHECK(failed->error().GetCode() == ErrorCode::CompileFailed);
			CHECK_FALSE(failed->error().GetIssues().empty());
#endif
		}
	}

}
