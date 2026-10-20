#include "TestsPCH.h"
#include "EditorCore/Native/NativeModuleHost.h"

#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
#include "Support/EditorTestFixture.h"
#include "Support/FakeNativeBackends.h"

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>

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

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0030-m19-contract.md decision 29 (N-C): while the user's preference is off, build and load are refused
		// with PermissionDenied; the Native panel's queued change turns it on, persisted in user://Editor.json, and turning it
		// off unloads the module.
		TEST_CASE("NativeModuleHost: build and load are refused while Allow native code is off, and the Native panel's change turns it on" * doctest::skip(true))
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
		TEST_CASE("NativeModuleHost: a load that fails keeps the previous module loaded and the scene unchanged" * doctest::skip(true))
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
		TEST_CASE("NativeModuleHost: reloading an unchanged module leaves the scene byte-identical, and a changed layout migrates it" * doctest::skip(true))
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
		TEST_CASE("NativeModuleHost: a project opening loads the newest current build and never builds" * doctest::skip(true))
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
	}

}
