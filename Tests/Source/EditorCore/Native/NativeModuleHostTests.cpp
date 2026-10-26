#include "TestsPCH.h"
#include "EditorCore/Native/NativeModuleHost.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Platform/SharedLibrary.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/FakeNativeBackends.h"
#include "Support/PlatformProbes.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// An editor whose native module host builds and loads through the fakes, with a project whose Native/ holds a source
		// and an open scene.
		struct NativeHostSetup
		{
			Ref<Test::FakeNativeBuildBackend> Builds = CreateRef<Test::FakeNativeBuildBackend>();
			Ref<Test::FakeNativeLibraryLoader> Loader = CreateRef<Test::FakeNativeLibraryLoader>();
			Test::EditorTestFixture Fixture{ "NativeHost", {}, nullptr, std::nullopt, false, Test::EditorTestNativeBackends{ .Build = Builds, .Loader = Loader } };

			NativeHostSetup()
			{
				Builds->SetApiHeader(GetApiHeader());
				Fixture.CreateAndOpenProject();
				WriteSource("Native/Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n");
				Fixture.CreateAndOpenScene();
			}

			[[nodiscard]] EditorContext& GetEditor() { return Fixture.GetEditor(); }
			[[nodiscard]] NativeModuleHost& GetHost() { return Fixture.GetEditor().GetNativeModule(); }

			[[nodiscard]] static std::filesystem::path GetApiHeader()
			{
				return MakeEditorNativeModuleHostSpecification(nullptr, nullptr).ApiHeader;
			}

			void WriteSource(std::string_view path, std::string_view text)
			{
				const std::filesystem::path file = Fixture.GetProjectRoot() / FileSystem::PathFromUtf8(path);
				REQUIRE(FileSystem::CreateDirectories(file.parent_path()).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size()))).has_value());
			}

			// The user's checkbox in the Native panel, applied at the next safe point.
			void Allow(bool allowed)
			{
				GetHost().QueuePreferenceChange(allowed);
				GetHost().Update(GetEditor());
			}

			// A build of the current sources, polled until it resolves (the fake resolves at its first poll).
			[[nodiscard]] NativeBuildOutcome Build()
			{
				const Status started = GetHost().StartBuild(GetEditor());
				REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
				std::optional<Result<NativeBuildOutcome>> outcome = GetHost().PollBuild(GetEditor());
				REQUIRE(outcome.has_value());
				REQUIRE_MESSAGE(outcome->has_value(), outcome->error().ToString());
				return std::move(**outcome);
			}

			[[nodiscard]] NativeLoadReport Load()
			{
				Result<NativeLoadReport> report = GetHost().Load(GetEditor());
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
				return std::move(*report);
			}
		};

		[[nodiscard]] Json ParseJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			return std::move(*json);
		}

		// Puts `name` on `entity` exactly as `json` spells it, as a scene file holds it before any canonicalization: through
		// the entity's preserved components, never PatchNativeComponents, which canonicalizes.
		void SetRawNativeComponent(Entity entity, std::string_view name, std::string_view json)
		{
			UnknownComponentData data{ .Name = std::string(name), .Version = NativeComponentVersion, .Data = VariantValue(ParseJson(json)) };
			if (!entity.HasComponent<UnknownComponentsComponent>())
			{
				entity.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent{ { std::move(data) } });
				return;
			}
			entity.Patch<UnknownComponentsComponent>([&data](UnknownComponentsComponent& preserved)
			{
				preserved.Components.push_back(std::move(data));
			});
		}

		// The names of the numbered build directories below the project's Library/Native/Builds/, in order.
		[[nodiscard]] std::vector<std::string> ListBuildDirectories(const std::filesystem::path& projectRoot)
		{
			std::vector<std::string> names;
			const std::filesystem::path builds = projectRoot / "Library" / "Native" / "Builds";
			if (!FileSystem::Exists(builds))
				return names;
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(builds);
			REQUIRE(entries.has_value());
			for (const std::filesystem::path& entry : *entries)
				names.push_back(FileSystem::PathToUtf8(entry.filename()));
			return names;
		}

		// A --strict editor of its own engine context (EditorContextSpecification::StrictUnknowns) whose user data holds
		// `preferences` as user://Editor.json, with the native module host's fakes.
		class StrictNativeEditor
		{
		public:
			StrictNativeEditor(const std::filesystem::path& directory, std::string_view preferences, Ref<INativeBuildBackend> builds, Ref<INativeLibraryLoader> loader)
			{
				const std::filesystem::path userData = directory / "StrictUserData";
				REQUIRE(FileSystem::CreateDirectories(userData).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(userData / "Editor.json", std::as_bytes(std::span(preferences.data(), preferences.size()))).has_value());
				Result<Scope<EngineContext>> engine = EngineContext::Create({ .WorkerCount = 0, .UserDataDirectory = userData, .RegisterTypes = &RegisterEditorMethodTypes });
				REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
				m_Engine = std::move(*engine);
				Result<Scope<EditorContext>> editor = EditorContext::Create(*m_Engine, {
																						   .IdGeneratorState = Test::EditorTestIdState,
																						   .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources" / "Templates" / "Projects",
																						   .ReadOnlyCacheRoot = directory / "StrictReadOnlyCache",
																						   .HistoryLimits = {},
																						   .EnvironmentBaker = nullptr,
																						   .StrictUnknowns = true,
																						   .DeferAssetScan = false,
																						   .NativeBuild = std::move(builds),
																						   .NativeLoader = std::move(loader),
																					   });
				REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
				m_Editor = std::move(*editor);
			}

			~StrictNativeEditor()
			{
				// The editor holds the project lock and the project:// mount: release both before the engine context goes.
				m_Editor.reset();
				m_Engine.reset();
			}

			StrictNativeEditor(const StrictNativeEditor&) = delete;
			StrictNativeEditor& operator=(const StrictNativeEditor&) = delete;

			[[nodiscard]] EditorContext& GetEditor() { return *m_Editor; }

			// ProjectManager::OpenProject strictly, as every opener of a --strict editor does, then OpenProject.
			[[nodiscard]] Status Open(const std::filesystem::path& projectFile)
			{
				ENGINE_TRY_ASSIGN(Scope<LoadedProject> project, ProjectManager::OpenProject(projectFile, { .StrictUnknowns = true }, m_Editor->GetTypeRegistry()));
				return m_Editor->OpenProject(std::move(project));
			}
		private:
			Scope<EngineContext> m_Engine;
			Scope<EditorContext> m_Editor;
		};

		// A prefab whose root carries "Native.Ghost", a native component no test module registers.
		constexpr std::string_view UnregisteredNativePrefab = R"({
	"Format": "Prefab",
	"Version": 1,
	"Name": "Ghost",
	"Root": "00000000000c0001",
	"ComponentVersions": {
		"Transform": 1,
		"Native.Ghost": 1
	},
	"Entities": [
		{
			"ID": "00000000000c0001",
			"Name": "Ghost",
			"Parent": null,
			"Active": true,
			"Tags": [],
			"Components": {
				"Transform": {
					"Translation": [0, 0, 0],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				},
				"Native.Ghost": {
					"Haunts": true
				}
			}
		}
	]
}
)";

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0030-m19-contract.md decision 29 (N-C): while the user's preference is off, build and load are refused
		// with PermissionDenied; the Native panel's queued change turns it on, persisted in user://Editor.json, and turning it
		// off unloads the module.
		TEST_CASE("NativeModuleHost: build and load are refused while Allow native code is off, and the Native panel's change turns it on")
		{
			NativeHostSetup setup;
			REQUIRE(setup.GetHost().ReadPreference(setup.GetEditor()).has_value());
			CHECK_FALSE(setup.GetHost().IsAllowed());
			const Status build = setup.GetHost().StartBuild(setup.GetEditor());
			REQUIRE_FALSE(build.has_value());
			CHECK(build.error().GetCode() == ErrorCode::PermissionDenied);
			const Result<NativeLoadReport> load = setup.GetHost().Load(setup.GetEditor());
			REQUIRE_FALSE(load.has_value());
			CHECK(load.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK(setup.Builds->GetStartCount() == 0);
			CHECK(setup.Loader->GetLoadCount() == 0);

			setup.Allow(true);
			CHECK(setup.GetHost().IsAllowed());
			const Result<EditorPreferences> stored = ReadEditorPreferences(setup.GetEditor().GetVfs());
			REQUIRE(stored.has_value());
			CHECK(stored->AllowNativeCode);
			CHECK(setup.Build().Succeeded);
			static_cast<void>(setup.Load());
			CHECK(setup.GetHost().GetLoadedTypes() != nullptr);

			setup.Allow(false);
			CHECK_FALSE(setup.GetHost().IsAllowed());
			CHECK(setup.GetHost().GetLoadedTypes() == nullptr);
			const Status refused = setup.GetHost().StartBuild(setup.GetEditor());
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::PermissionDenied);
		}

		// Decision 31 (N-C): a load replaces the loaded module only once every step succeeded, so a newer build whose library
		// does not load leaves the previous module and the scene as they were; and nothing loads while a play session runs.
		TEST_CASE("NativeModuleHost: a load that fails keeps the previous module loaded and the scene unchanged")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			const NativeBuildOutcome first = setup.Build();
			REQUIRE(first.Succeeded);
			static_cast<void>(setup.Load());
			const Ref<const NativeModuleTypes> loaded = setup.GetHost().GetLoadedTypes();
			REQUIRE(loaded != nullptr);
			const Result<std::string> before = SceneSerializer::SaveToString(setup.GetEditor().GetScene());
			REQUIRE(before.has_value());

			// A newer build whose library cannot be loaded.
			setup.Builds->SetModule(Test::FakeNativeModule::Layout2);
			const NativeBuildOutcome second = setup.Build();
			REQUIRE(second.Succeeded);
			REQUIRE(FileSystem::Remove(second.Output).has_value());
			const Result<NativeLoadReport> failed = setup.GetHost().Load(setup.GetEditor());
			REQUIRE_FALSE(failed.has_value());
			CHECK(setup.GetHost().GetLoadedTypes() == loaded);
			CHECK(SceneSerializer::SaveToString(setup.GetEditor().GetScene()).value_or(std::string()) == *before);

			// While a play session runs, nothing is loaded.
			REQUIRE(setup.GetEditor().GetPlay().Start(PlayStartOptions{}).has_value());
			const Result<NativeLoadReport> playing = setup.GetHost().Load(setup.GetEditor());
			REQUIRE_FALSE(playing.has_value());
			CHECK(playing.error().GetCode() == ErrorCode::InvalidState);
		}

		// Decision 31 (N-C, the milestone's acceptance): reloading an unchanged module leaves the scene byte-identical, and a
		// reload with a changed layout migrates its native components by field name and marks the scene modified.
		TEST_CASE("NativeModuleHost: reloading an unchanged module leaves the scene byte-identical, and a changed layout migrates it")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			const NativeLoadReport first = setup.Load();
			CHECK_FALSE(first.Reloaded);
			CHECK(first.LayoutChanged);
			Entity spinner = setup.GetEditor().GetScene().CreateEntity("Spinner");
			REQUIRE(PatchNativeComponents(spinner, *setup.GetHost().GetLoadedTypes(), ParseJson(R"({"Native.TestSpinner": {"DegreesPerSecond": 10}})"), "/components")
					.has_value());
			const Result<std::string> before = SceneSerializer::SaveToString(setup.GetEditor().GetScene());
			REQUIRE(before.has_value());

			REQUIRE(setup.Build().Succeeded);
			const NativeLoadReport unchanged = setup.Load();
			CHECK(unchanged.Reloaded);
			CHECK_FALSE(unchanged.LayoutChanged);
			CHECK_FALSE(unchanged.UndoCleared);
			CHECK_FALSE(unchanged.Scene.Changed);
			CHECK(SceneSerializer::SaveToString(setup.GetEditor().GetScene()).value_or(std::string()) == *before);

			setup.Builds->SetModule(Test::FakeNativeModule::Layout2);
			REQUIRE(setup.Build().Succeeded);
			const NativeLoadReport changed = setup.Load();
			CHECK(changed.Reloaded);
			CHECK(changed.LayoutChanged);
			CHECK(changed.UndoCleared);
			CHECK(changed.Scene.Changed);
			CHECK(setup.GetEditor().IsSceneDirty());
			const NativeEntityComponents migrated = ReadNativeComponents(spinner, setup.GetHost().GetLoadedTypes().get());
			REQUIRE(migrated.Registered.size() == 1);
			CHECK(migrated.Registered[0].second.Get() == ParseJson(R"({"Speed": 10, "Axis": [0, 1, 0], "Turns": 0, "Reversed": false})"));
		}

		// Decision 31 (N-C): a project opening loads the newest build whose source hash is current, and builds nothing; a
		// stale build is not loaded.
		TEST_CASE("NativeModuleHost: a project opening loads the newest current build and never builds")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			setup.GetHost().OnProjectClosed();
			CHECK(setup.GetHost().GetLoadedTypes() == nullptr);

			REQUIRE(setup.GetHost().OnProjectOpened(setup.GetEditor()).has_value());
			CHECK(setup.GetHost().GetLoadedTypes() != nullptr);
			CHECK(setup.Builds->GetStartCount() == 1);

			setup.WriteSource("Native/Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n// changed\n");
			const Result<NativeModuleStatus> status = setup.GetHost().GetStatus(setup.GetEditor());
			REQUIRE(status.has_value());
			REQUIRE(status->LatestBuild.has_value());
			CHECK_FALSE(status->LatestBuild->Current);
			setup.GetHost().OnProjectClosed();
			CHECK(setup.GetHost().OnProjectOpened(setup.GetEditor()).has_value());
			CHECK(setup.GetHost().GetLoadedTypes() == nullptr);
			CHECK(setup.Builds->GetStartCount() == 1);
		}

		// Decision 29 (N-C): turning the preference off unloads the module at the first frame boundary without a play session,
		// never under a running one, and the scene keeps its native components as written.
		TEST_CASE("NativeModuleHost: turning Allow native code off unloads the module at the first frame boundary without a play session")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			static_cast<void>(setup.Load());
			Entity spinner = setup.GetEditor().GetScene().CreateEntity("Spinner");
			REQUIRE(PatchNativeComponents(spinner, *setup.GetHost().GetLoadedTypes(), ParseJson(R"({"Native.TestSpinner": {"DegreesPerSecond": 45}})"), "/components")
					.has_value());
			const Result<std::string> before = SceneSerializer::SaveToString(setup.GetEditor().GetScene());
			REQUIRE(before.has_value());

			REQUIRE(setup.GetEditor().GetPlay().Start(PlayStartOptions{}).has_value());
			setup.Allow(false);
			CHECK_FALSE(setup.GetHost().IsAllowed());
			CHECK(setup.GetHost().GetLoadedTypes() != nullptr);
			REQUIRE(setup.GetEditor().GetPlay().Stop().has_value());
			setup.GetHost().Update(setup.GetEditor());
			CHECK(setup.GetHost().GetLoadedTypes() == nullptr);
			CHECK(SceneSerializer::SaveToString(setup.GetEditor().GetScene()).value_or(std::string()) == *before);
			const Result<EditorPreferences> stored = ReadEditorPreferences(setup.GetEditor().GetVfs());
			REQUIRE(stored.has_value());
			CHECK_FALSE(stored->AllowNativeCode);
		}

		// Decision 31 (N-C): a reload whose layout changed clears the undo history, whose snapshots hold the previous layout's
		// values, without losing the scene's unsaved changes (clearing moves the history's save point).
		TEST_CASE("NativeModuleHost: a reload whose layout changed clears the undo history and keeps the scene's unsaved changes")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			static_cast<void>(setup.Load());
			EditorContext& editor = setup.GetEditor();
			{
				SceneEdit edit(editor, "Create 'Unsaved'");
				static_cast<void>(editor.GetScene().CreateEntity("Unsaved"));
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(editor.GetHistory().CanUndo());
			REQUIRE(editor.IsSceneDirty());

			setup.Builds->SetModule(Test::FakeNativeModule::Layout2);
			REQUIRE(setup.Build().Succeeded);
			const NativeLoadReport changed = setup.Load();
			CHECK(changed.LayoutChanged);
			CHECK_FALSE(changed.Scene.Changed);
			CHECK(changed.UndoCleared);
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK(editor.IsSceneDirty());
		}

		// Decision 31 (N-C): a load whose migration finds a value the new types reject fails as a whole, the module it would
		// have replaced (none, or the loaded one) staying and the scene unchanged.
		TEST_CASE("NativeModuleHost: a load whose migration meets a value its types reject keeps the loaded module and the scene")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			Entity spinner = setup.GetEditor().GetScene().CreateEntity("Spinner");
			// Beyond DegreesPerSecond's maximum (3600), as a scene file may hold it.
			SetRawNativeComponent(spinner, "Native.TestSpinner", R"({"DegreesPerSecond": 5000})");
			const Result<std::string> before = SceneSerializer::SaveToString(setup.GetEditor().GetScene());
			REQUIRE(before.has_value());
			const Result<NativeLoadReport> first = setup.GetHost().Load(setup.GetEditor());
			REQUIRE_FALSE(first.has_value());
			CHECK(first.error().GetCode() == ErrorCode::Validation);
			CHECK(setup.GetHost().GetLoadedTypes() == nullptr);
			CHECK(SceneSerializer::SaveToString(setup.GetEditor().GetScene()).value_or(std::string()) == *before);

			// With the value removed the module loads; a value the next layout rejects keeps it loaded.
			spinner.RemoveComponent<UnknownComponentsComponent>();
			static_cast<void>(setup.Load());
			const Ref<const NativeModuleTypes> loaded = setup.GetHost().GetLoadedTypes();
			REQUIRE(loaded != nullptr);
			SetRawNativeComponent(spinner, "Native.TestSpinner", R"({"DegreesPerSecond": -5000})");
			const Result<std::string> written = SceneSerializer::SaveToString(setup.GetEditor().GetScene());
			REQUIRE(written.has_value());
			setup.Builds->SetModule(Test::FakeNativeModule::Layout2);
			REQUIRE(setup.Build().Succeeded);
			const Result<NativeLoadReport> reload = setup.GetHost().Load(setup.GetEditor());
			REQUIRE_FALSE(reload.has_value());
			CHECK(reload.error().GetCode() == ErrorCode::Validation);
			CHECK(setup.GetHost().GetLoadedTypes() == loaded);
			CHECK(SceneSerializer::SaveToString(setup.GetEditor().GetScene()).value_or(std::string()) == *written);
		}

		// The merge with M19 (Docs/Decisions/0031-m20-contract.md, "Merge with M19"): a load whose layout changed migrates the
		// native components of every open scene, the parked ones included, marks each changed scene modified and clears every
		// open scene's undo history; its report lists each scene's migration in tab order.
		TEST_CASE("NativeModuleHost: a load migrates the native components of every open scene and clears every open scene's undo history")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			EditorContext& editor = setup.GetEditor();
			const OpenSceneId first = editor.GetActiveSceneId();
			// A member the module's layout lacks, as a scene written for an earlier layout holds it, in each open scene, and one
			// undoable edit in each.
			const auto prepare = [&editor](std::string_view name, std::string_view json)
			{
				const UUID id = editor.GetScene().CreateEntity("Spinner").GetUUID();
				SetRawNativeComponent(editor.GetScene().FindEntityByID(id), "Native.TestSpinner", json);
				SceneEdit edit(editor, std::format("Rename to '{}'", name));
				editor.GetScene().FindEntityByID(id).SetName(std::string(name));
				REQUIRE(edit.Commit().has_value());
				return id;
			};
			const UUID wheel = prepare("Wheel", R"({"DegreesPerSecond": 10, "OldField": 3})");
			const Result<OpenSceneId> second = editor.AddScene(editor.CreateScene("Second"), std::nullopt);
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			const UUID rotor = prepare("Rotor", R"({"DegreesPerSecond": 20, "OldField": 4})");
			REQUIRE(editor.GetActiveSceneId() == *second);

			const NativeLoadReport report = setup.Load();
			CHECK(report.LayoutChanged);
			CHECK(report.Scene.Changed);
			CHECK(report.UndoCleared);
			CHECK(report.Scene.ComponentCount == 2);
			REQUIRE(report.Scenes.size() == 2);
			CHECK(report.Scenes[0].Scene == first);
			CHECK(report.Scenes[1].Scene == *second);
			CHECK(report.Scenes[0].Report.Changed);
			CHECK(report.Scenes[1].Report.Changed);
			CHECK(editor.GetActiveSceneId() == *second);

			const Ref<const NativeModuleTypes> types = setup.GetHost().GetLoadedTypes();
			for (const std::pair<OpenSceneId, UUID>& spinner : { std::pair(first, wheel), std::pair(*second, rotor) })
			{
				CAPTURE(spinner.first);
				REQUIRE(editor.ActivateScene(spinner.first).has_value());
				CHECK(editor.IsSceneDirty());
				CHECK_FALSE(editor.GetHistory().CanUndo());
				const NativeEntityComponents components = ReadNativeComponents(editor.GetScene().FindEntityByID(spinner.second), types.get());
				REQUIRE(components.Registered.size() == 1);
				CHECK_FALSE(components.Registered[0].second.Get().contains("OldField"));
			}
		}

		// The merge with M19: every open scene is checked before any is migrated, so a value the new types reject in one open
		// scene fails the load as a whole: the loaded module stays, and no open scene changes, the earlier tabs included.
		TEST_CASE("NativeModuleHost: a load whose migration fails in one open scene changes no open scene")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			static_cast<void>(setup.Load());
			const Ref<const NativeModuleTypes> loaded = setup.GetHost().GetLoadedTypes();
			REQUIRE(loaded != nullptr);
			EditorContext& editor = setup.GetEditor();
			const OpenSceneId first = editor.GetActiveSceneId();
			SetRawNativeComponent(editor.GetScene().CreateEntity("Spinner"), "Native.TestSpinner", R"({"DegreesPerSecond": 10})");
			const Result<OpenSceneId> second = editor.AddScene(editor.CreateScene("Second"), std::nullopt);
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			// Beyond the next layout's range, as a scene file may hold it.
			SetRawNativeComponent(editor.GetScene().CreateEntity("Rejected"), "Native.TestSpinner", R"({"DegreesPerSecond": -5000})");
			const auto save = [&editor](OpenSceneId scene)
			{
				const Scene* open = std::as_const(editor).FindScene(scene);
				REQUIRE(open != nullptr);
				return SceneSerializer::SaveToString(*open).value_or(std::string());
			};
			const std::string firstBefore = save(first);
			const std::string secondBefore = save(*second);
			const uint64_t revision = editor.GetRevision();

			setup.Builds->SetModule(Test::FakeNativeModule::Layout2);
			REQUIRE(setup.Build().Succeeded);
			const Result<NativeLoadReport> reload = setup.GetHost().Load(editor);
			REQUIRE_FALSE(reload.has_value());
			CHECK(reload.error().GetCode() == ErrorCode::Validation);
			CHECK(reload.error().GetMessageText().contains("Second"));
			CHECK(setup.GetHost().GetLoadedTypes() == loaded);
			CHECK(save(first) == firstBefore);
			CHECK(save(*second) == secondBefore);
			CHECK(editor.GetRevision() == revision);
		}

		// Decision 31 (N-C): every build gets a new numbered directory, and the host keeps only the loaded build and the
		// newest, removing the older ones once they are no longer loaded; a failed build stays with its log.
		TEST_CASE("NativeModuleHost: builds go to new numbered directories, and only the loaded build and the newest are kept")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			const std::filesystem::path root = setup.Fixture.GetProjectRoot();
			const std::filesystem::path builds = root / "Library" / "Native" / "Builds";
			const NativeBuildOutcome first = setup.Build();
			REQUIRE(first.Succeeded);
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000001" });
			CHECK(setup.Builds->GetLastRequest().OutputDirectory == builds / "00000001");
			CHECK(setup.Builds->GetLastRequest().Kind == NativeBuildKind::Module);
			static_cast<void>(setup.Load());

			REQUIRE(setup.Build().Succeeded);
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000001", "00000002" });
			REQUIRE(setup.Build().Succeeded);
			// The second is neither loaded nor the newest.
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000001", "00000003" });
			const Result<NativeModuleStatus> status = setup.GetHost().GetStatus(setup.GetEditor());
			REQUIRE(status.has_value());
			REQUIRE(status->LatestBuild.has_value());
			CHECK(status->LatestBuild->Directory == builds / "00000003");
			CHECK(status->LatestBuild->Current);
			REQUIRE(status->LastBuild.has_value());
			CHECK(status->LastBuild->Succeeded);
			CHECK(status->LoadedSourceHash == first.SourceHash);

			// Loading the newest releases the first, which then goes.
			static_cast<void>(setup.Load());
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000003" });
			setup.Builds->SetDiagnostics({ NativeBuildDiagnostic{ .Severity = DiagnosticSeverity::Error,
				.Code = "NATIVE_COMPILE_ERROR",
				.File = "Native/Spinner.cpp",
				.Line = 1,
				.Column = 1,
				.Message = "expected a declaration" } });
			const NativeBuildOutcome failed = setup.Build();
			CHECK_FALSE(failed.Succeeded);
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000003", "00000004" });
			const Result<NativeModuleStatus> after = setup.GetHost().GetStatus(setup.GetEditor());
			REQUIRE(after.has_value());
			REQUIRE(after->LatestBuild.has_value());
			CHECK(after->LatestBuild->Directory == builds / "00000003");
			REQUIRE(after->LastBuild.has_value());
			CHECK_FALSE(after->LastBuild->Succeeded);
			REQUIRE(after->LastBuild->Diagnostics.size() == 1);
			CHECK(after->LastBuild->Diagnostics[0].Code == "NATIVE_COMPILE_ERROR");

			// No build is numbered after the largest sequence: the build is refused rather than wrap around.
			REQUIRE(FileSystem::CreateDirectories(builds / "18446744073709551615").has_value());
			const uint32_t starts = setup.Builds->GetStartCount();
			const Status exhausted = setup.GetHost().StartBuild(setup.GetEditor());
			REQUIRE_FALSE(exhausted.has_value());
			CHECK(exhausted.error().GetCode() == ErrorCode::InvalidState);
			CHECK(setup.Builds->GetStartCount() == starts);
		}

		// Decision 27 (N-C): with a module loaded, a scene opens with its native components canonical, and is then modified.
		TEST_CASE("NativeModuleHost: a scene that opens while a module is loaded gets canonical native components and is modified")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			static_cast<void>(setup.Load());
			EditorContext& editor = setup.GetEditor();
			Scope<Scene> scene = editor.CreateScene("Opened");
			const UUID id = scene->CreateEntity("Spinner").GetUUID();
			SetRawNativeComponent(scene->FindEntityByID(id), "Native.TestSpinner", R"({"DegreesPerSecond": 30})");
			editor.SetScene(std::move(scene), std::nullopt, false);
			CHECK(editor.IsSceneDirty());
			const NativeEntityComponents components = ReadNativeComponents(editor.GetScene().FindEntityByID(id), setup.GetHost().GetLoadedTypes().get());
			REQUIRE(components.Registered.size() == 1);
			CHECK(components.Registered[0].second.Get() == ParseJson(R"({"DegreesPerSecond": 30, "Axis": [0, 1, 0], "Turns": 0})"));

			// A canonical scene opens unmodified.
			Scope<Scene> canonical = editor.CreateScene("Canonical");
			Entity entity = canonical->CreateEntity("Spinner");
			REQUIRE(PatchNativeComponents(entity, *setup.GetHost().GetLoadedTypes(), ParseJson(R"({"Native.TestSpinner": {}})"), "/components").has_value());
			editor.SetScene(std::move(canonical), std::nullopt, false);
			CHECK_FALSE(editor.IsSceneDirty());
		}

		// Decision 27 (N-C): a --strict editor reads the preference from the user's Editor.json, loads the project's newest
		// current build when the project opens, and then refuses the project for every native component of its scenes and
		// prefabs that the module does not register, whose value its type rejects or that would migrate, naming each.
		TEST_CASE("NativeModuleHost: a strict editor refuses a project whose native components its loaded module does not register or accept")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			const NativeBuildOutcome built = setup.Build();
			REQUIRE(built.Succeeded);
			EditorContext& editor = setup.GetEditor();
			Entity rejected = editor.GetScene().CreateEntity("Rejected");
			SetRawNativeComponent(rejected, "Native.TestSpinner", R"({"DegreesPerSecond": 5000})");
			Entity unknown = editor.GetScene().CreateEntity("Unknown");
			SetRawNativeComponent(unknown, "Native.Hovering", R"({"Height": 2})");
			Entity misspelt = editor.GetScene().CreateEntity("Misspelt");
			SetRawNativeComponent(misspelt, "Native.TestMover", R"({"Spped": 1})");
			const Result<std::string> scene = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(scene.has_value());
			setup.WriteSource("Assets/Scenes/Natives.scene", *scene);
			setup.WriteSource("Assets/Prefabs/Ghost.prefab", UnregisteredNativePrefab);
			const std::filesystem::path projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(editor.CloseProject().has_value());

			StrictNativeEditor strict(setup.Fixture.GetDirectory().GetPath(), R"({"Format": "EditorPreferences", "Version": 1, "AllowNativeCode": true})", setup.Builds,
				setup.Loader);
			CHECK(strict.GetEditor().GetNativeModule().IsAllowed());
			const uint32_t loads = setup.Loader->GetLoadCount();
			const Status opened = strict.Open(projectFile);
			REQUIRE_FALSE(opened.has_value());
			CHECK(opened.error().GetCode() == ErrorCode::Validation);
			CHECK(setup.Loader->GetLoadCount() == loads + 1);
			CHECK(setup.Builds->GetStartCount() == 1);
			std::vector<std::string> found;
			for (const ErrorIssue& issue : opened.error().GetIssues())
			{
				for (const std::string_view code : { NativeComponentInvalidCode, NativeComponentUnregisteredCode, NativeFieldDroppedCode })
				{
					if (issue.Message.contains(std::format("({},", code)))
						found.push_back(std::format("{} {}", issue.Message.substr(0, issue.Message.find(' ')), code));
				}
			}
			std::ranges::sort(found);
			CHECK(found
				== std::vector<std::string>{ "Assets/Prefabs/Ghost.prefab NATIVE_COMPONENT_UNREGISTERED", "Assets/Scenes/Natives.scene NATIVE_COMPONENT_INVALID",
					"Assets/Scenes/Natives.scene NATIVE_COMPONENT_UNREGISTERED", "Assets/Scenes/Natives.scene NATIVE_FIELD_DROPPED" });
			CHECK_FALSE(strict.GetEditor().HasProject());
			CHECK(strict.GetEditor().GetNativeModule().GetLoadedTypes() == nullptr);

			// The review of W19.2: when the module does not load as the project opens (here its library is gone), every native
			// component reads as unregistered, and each finding's hint names the failed load rather than steps already taken.
			REQUIRE(FileSystem::Remove(built.Output).has_value());
			const Status unloaded = strict.Open(projectFile);
			REQUIRE_FALSE(unloaded.has_value());
			CHECK(setup.Loader->GetLoadCount() == loads + 2);
			CHECK(strict.GetEditor().GetNativeModule().GetLoadedTypes() == nullptr);
			size_t unregistered = 0;
			for (const ErrorIssue& issue : unloaded.error().GetIssues())
			{
				if (!issue.Message.contains(std::format("({},", NativeComponentUnregisteredCode)))
					continue;
				++unregistered;
				CHECK_MESSAGE(issue.Hint.contains(Test::FakeNativeLibraryName), issue.Hint);
			}
			CHECK(unregistered == 4);
		}

		// Decision 31 (N-C; the review of W19.2): a first load's layout differs from what the undo history was recorded under
		// (no module: the values as written), so it clears the history when it migrates the open scene, and no undo can bring
		// back a member the module's layout dropped.
		TEST_CASE("NativeModuleHost: a first load that migrates the open scene clears the undo history")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			REQUIRE(setup.Build().Succeeded);
			EditorContext& editor = setup.GetEditor();
			const UUID id = editor.GetScene().CreateEntity("Spinner").GetUUID();
			// A member the module's layout lacks, as a scene written for an earlier layout holds it.
			SetRawNativeComponent(editor.GetScene().FindEntityByID(id), "Native.TestSpinner", R"({"DegreesPerSecond": 10, "OldField": 3})");
			{
				SceneEdit edit(editor, "Rename 'Spinner'");
				editor.GetScene().FindEntityByID(id).SetName("Wheel");
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(editor.GetHistory().CanUndo());

			const NativeLoadReport first = setup.Load();
			CHECK_FALSE(first.Reloaded);
			CHECK(first.LayoutChanged);
			CHECK(first.Scene.Changed);
			CHECK(first.UndoCleared);
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK(editor.IsSceneDirty());
			const NativeEntityComponents components = ReadNativeComponents(editor.GetScene().FindEntityByID(id), setup.GetHost().GetLoadedTypes().get());
			REQUIRE(components.Registered.size() == 1);
			CHECK_FALSE(components.Registered[0].second.Get().contains("OldField"));
		}

		// Decisions 30 and 31 (N-C; the review of W19.2): a build is refused while another runs, keeping the running one, for a
		// project without sources, before any build directory is made, and for a read-only project; a load is refused while
		// a build runs and without a successful build of the sources as they are now.
		TEST_CASE("NativeModuleHost: builds and loads are refused while a build runs, without sources or a current build, and for a read-only project")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			EditorContext& editor = setup.GetEditor();
			NativeModuleHost& host = setup.GetHost();
			const std::filesystem::path root = setup.Fixture.GetProjectRoot();

			const Result<NativeLoadReport> unbuilt = host.Load(editor);
			REQUIRE_FALSE(unbuilt.has_value());
			CHECK(unbuilt.error().GetCode() == ErrorCode::InvalidState);

			REQUIRE(host.StartBuild(editor).has_value());
			const Status second = host.StartBuild(editor);
			REQUIRE_FALSE(second.has_value());
			CHECK(second.error().GetCode() == ErrorCode::InvalidState);
			const Result<NativeLoadReport> during = host.Load(editor);
			REQUIRE_FALSE(during.has_value());
			CHECK(during.error().GetCode() == ErrorCode::InvalidState);
			CHECK(setup.Builds->GetStartCount() == 1);
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000001" });
			const std::optional<Result<NativeBuildOutcome>> outcome = host.PollBuild(editor);
			REQUIRE(outcome.has_value());
			REQUIRE(outcome->has_value());
			CHECK((*outcome)->Succeeded);

			// The sources changed since the build: it is stale, so nothing loads.
			setup.WriteSource("Native/Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n// changed\n");
			const Result<NativeLoadReport> stale = host.Load(editor);
			REQUIRE_FALSE(stale.has_value());
			CHECK(stale.error().GetCode() == ErrorCode::InvalidState);
			CHECK(setup.Loader->GetLoadCount() == 0);
			CHECK(host.GetLoadedTypes() == nullptr);

			REQUIRE(FileSystem::Remove(root / "Native" / "Spinner.cpp").has_value());
			const Status sourceless = host.StartBuild(editor);
			REQUIRE_FALSE(sourceless.has_value());
			CHECK(sourceless.error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Builds->GetStartCount() == 1);
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000001" });

			setup.WriteSource("Native/Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n// read-only\n");
			const std::filesystem::path projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(editor.CloseProject().has_value());
			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .StrictUnknowns = false, .ReadOnlyCacheDirectory = setup.Fixture.GetDirectory() / "ReadOnlyCache" }, editor.GetTypeRegistry());
			REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			REQUIRE(editor.IsReadOnly());
			const Status readOnly = host.StartBuild(editor);
			REQUIRE_FALSE(readOnly.has_value());
			CHECK(readOnly.error().GetCode() == ErrorCode::InvalidState);
			CHECK(setup.Builds->GetStartCount() == 1);
			CHECK(ListBuildDirectories(root) == std::vector<std::string>{ "00000001" });
		}

		// Decisions 30 and 31 (N-C; the review of W19.2): cancelling a running build resolves it as Cancelled, which never
		// becomes the last build; closing the project cancels a running build, so the reopened project builds at once; and
		// an editor destroyed during a build cancels it.
		TEST_CASE("NativeModuleHost: cancelling, closing the project or destroying the editor cancels a running build, and the reopened project builds again")
		{
			NativeHostSetup setup;
			setup.Allow(true);
			EditorContext& editor = setup.GetEditor();
			NativeModuleHost& host = setup.GetHost();
			REQUIRE(setup.Build().Succeeded);

			setup.Builds->SetPending(true);
			REQUIRE(host.StartBuild(editor).has_value());
			CHECK_FALSE(host.PollBuild(editor).has_value());
			host.CancelBuild();
			CHECK(setup.Builds->GetCancelCount() == 1);
			const std::optional<Result<NativeBuildOutcome>> cancelled = host.PollBuild(editor);
			REQUIRE(cancelled.has_value());
			REQUIRE_FALSE(cancelled->has_value());
			CHECK(cancelled->error().GetCode() == ErrorCode::Cancelled);
			const Result<NativeModuleStatus> afterCancel = host.GetStatus(editor);
			REQUIRE(afterCancel.has_value());
			CHECK_FALSE(afterCancel->Building);
			REQUIRE(afterCancel->LastBuild.has_value());
			CHECK(afterCancel->LastBuild->Succeeded);

			// This build's end is not seen when it is cancelled (a killed process): the host resolves it as Cancelled itself.
			setup.Builds->SetPending(true, false);
			REQUIRE(host.StartBuild(editor).has_value());
			const Result<NativeModuleStatus> running = host.GetStatus(editor);
			REQUIRE(running.has_value());
			CHECK(running->Building);
			const std::filesystem::path projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(editor.CloseProject().has_value());
			CHECK(setup.Builds->GetCancelCount() == 2);
			const std::optional<Result<NativeBuildOutcome>> closed = host.PollBuild(editor);
			REQUIRE(closed.has_value());
			REQUIRE_FALSE(closed->has_value());
			CHECK(closed->error().GetCode() == ErrorCode::Cancelled);
			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(projectFile, {}, editor.GetTypeRegistry());
			REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			const Result<NativeModuleStatus> reopened = host.GetStatus(editor);
			REQUIRE(reopened.has_value());
			CHECK_FALSE(reopened->Building);
			CHECK_FALSE(reopened->LastBuild.has_value());
			setup.Builds->SetPending(false);
			CHECK(setup.Build().Succeeded);
			CHECK(setup.Builds->GetStartCount() == 4);

			{
				Test::EditorTestFixture other{ "NativeHostDestroyed", {}, nullptr, std::nullopt, false, Test::EditorTestNativeBackends{ .Build = setup.Builds, .Loader = setup.Loader } };
				other.CreateAndOpenProject();
				const std::filesystem::path source = other.GetProjectRoot() / "Native" / "Spinner.cpp";
				REQUIRE(FileSystem::CreateDirectories(source.parent_path()).has_value());
				constexpr std::string_view Source = "#include \"Engine/Native/NativeApi.h\"\n";
				REQUIRE(FileSystem::WriteFileAtomic(source, std::as_bytes(std::span(Source.data(), Source.size()))).has_value());
				NativeModuleHost& otherHost = other.GetEditor().GetNativeModule();
				otherHost.QueuePreferenceChange(true);
				otherHost.Update(other.GetEditor());
				setup.Builds->SetPending(true);
				REQUIRE(otherHost.StartBuild(other.GetEditor()).has_value());
			}
			CHECK(setup.Builds->GetCancelCount() == 3);
		}

		// Decision 29 (N-C; the review of W19.2): a change the host cannot save into user://Editor.json is not applied, so the
		// preference stays as it was, and the failure is logged for the Console.
		TEST_CASE("NativeModuleHost: a preference change that cannot be saved leaves the preference as it was")
		{
			NativeHostSetup setup;
			EditorContext& editor = setup.GetEditor();
			VirtualFileSystem& vfs = editor.GetVfs();
			const Result<VfsPath> preferences = VfsPath::Parse("user://Editor.json");
			REQUIRE(preferences.has_value());
			if (vfs.Exists(*preferences))
				REQUIRE(vfs.Remove(*preferences).has_value());
			// A folder where the file goes, which can be neither read nor replaced.
			REQUIRE(vfs.CreateDirectories(*preferences).has_value());
			{
				const Test::ExpectLog failed(LogLevel::Error, "Allow native code");
				setup.Allow(true);
			}
			CHECK_FALSE(setup.GetHost().IsAllowed());
			const Status build = setup.GetHost().StartBuild(editor);
			REQUIRE_FALSE(build.has_value());
			CHECK(build.error().GetCode() == ErrorCode::PermissionDenied);
		}

		// Decision 31 (N-C): the platform's loader resolves the two functions ENGINE_NATIVE_MODULE exports, so a library that
		// exports neither is refused, naming what it lacks, and a library that is not there is NotFound; where the platform
		// loads no native module (decision 34), every load is Unsupported.
		TEST_CASE("NativeModuleHost: the platform's loader refuses a library that is not a native module")
		{
			const Scope<INativeLibraryLoader> loader = CreateNativeLibraryLoader();
			if (!SharedLibrary::CheckSupported().has_value())
			{
				const Result<NativeLoadedLibrary> refused = loader->Load(std::filesystem::path("/usr/lib/libm.so"));
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Unsupported);
				return;
			}
			const Test::SystemLibraryProbe probe = Test::GetSystemLibraryProbe();
			REQUIRE_FALSE(probe.Library.empty());
			const Result<NativeLoadedLibrary> system = loader->Load(probe.Library);
			REQUIRE_FALSE(system.has_value());
			CHECK(system.error().GetCode() == ErrorCode::NotFound);
			CHECK(system.error().GetMessageText().contains("EngineNativeModule"));
			const Test::TempDirectory directory("NativeLoader");
			const Result<NativeLoadedLibrary> absent = loader->Load(directory / "Absent.dll");
			REQUIRE_FALSE(absent.has_value());
			CHECK(absent.error().GetCode() == ErrorCode::NotFound);
		}
	}

}
