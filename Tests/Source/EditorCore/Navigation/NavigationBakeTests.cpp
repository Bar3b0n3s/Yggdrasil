#include "TestsPCH.h"

#include "EditorCore/Navigation/NavigationBake.h"

#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/NavigationTestScene.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// navigation.bake's work (EditorCore/Navigation/NavigationBake.h; Docs/Decisions/0045-m30-navigation.md decision 8): its
// refusals, the acceptance "two bakes give identical bytes" (the first bake writes, imports and assigns the mesh in one undo
// step; the second writes nothing and records no step), a changed scene rewriting the file, its write and import failures,
// and the bake's independence from a renderer. The method's round trip is Automation/NavigationMethodsTests.cpp.

namespace Engine {

	namespace {

		// The bytes of the project file `relative` of `fixture`'s project, or nullopt.
		std::optional<Buffer> ReadProjectBytes(Test::EditorTestFixture& fixture, std::string_view relative)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(fixture.GetProjectRoot() / FileSystem::PathFromUtf8(relative));
			if (!bytes.has_value())
				return std::nullopt;
			return std::move(*bytes);
		}

		// The NavMesh the active scene's Navigation entity references.
		AssetHandle GetBakedHandle(EditorContext& editor)
		{
			return editor.GetScene().FindEntityByPath("/Navigation").GetComponent<NavigationComponent>().NavMesh.GetHandle();
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("NavigationBake: an unsaved scene, the play scene and a scene without a Navigation component are refused, writing nothing")
		{
			Test::EditorTestFixture fixture("NavigationBakeRefusals");
			EditorContext& editor = fixture.GetEditor();
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const OpenSceneId saved = editor.GetActiveSceneId();

			// Without a Navigation component: NotFound, naming the component.
			const Result<NavigationBakeResult> none = BakeNavigation(editor, saved);
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::NotFound);

			// A scene never saved.
			const Result<OpenSceneId> unsaved = editor.AddScene(editor.CreateScene("Unsaved"), std::nullopt);
			REQUIRE(unsaved.has_value());
			static_cast<void>(Test::AddNavigationArena(editor.GetScene()));
			const Result<NavigationBakeResult> refused = BakeNavigation(editor, *unsaved);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);

			// The play scene.
			REQUIRE(editor.ActivateScene(saved).has_value());
			static_cast<void>(Test::AddNavigationArena(editor.GetScene()));
			Test::AutomationTestClient client(editor, Test::MakeTestServerSpecification());
			REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Result<NavigationBakeResult> playing = BakeNavigation(editor, saved);
			REQUIRE_FALSE(playing.has_value());
			CHECK(playing.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(client.Call("play.stop", Json::object()).has_value());

			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Assets" / "Navigation", error));
		}

		// Roadmap M30's acceptance "two bakes give identical bytes" and World Tree's brief 1 ("the method writes the baked data
		// beside the scene, imports it and references it from the scene in one undoable step"). The fixture's editor has no
		// renderer: the bake is CPU only.
		TEST_CASE("NavigationBake: a bake writes, imports and assigns the mesh in one undo step, and a rebake writes identical bytes and no step")
		{
			Test::EditorTestFixture fixture("NavigationBakeTwice");
			EditorContext& editor = fixture.GetEditor();
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Arena.scene");
			static_cast<void>(Test::AddNavigationArena(editor.GetScene()));
			const size_t steps = editor.GetHistory().GetUndoCount();

			const Result<NavigationBakeResult> first = BakeNavigation(editor, editor.GetActiveSceneId());
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK(first->Path == "Assets/Navigation/Scenes/Arena.navmesh");
			CHECK(first->Changed);
			CHECK(first->Asset.IsValid());
			CHECK(first->Entity == editor.GetScene().FindEntityByPath("/Navigation").GetUUID());
			REQUIRE(first->Agents.size() == 2);
			CHECK(first->Agents[0].Name == "Small");
			CHECK(first->Agents[0].Polygons > 0);
			CHECK(first->Agents[1].Name == "Large");
			CHECK(first->SkippedBodies.empty());
			CHECK(GetBakedHandle(editor) == first->Asset);
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
			CHECK(editor.GetHistory().GetUndoLabel().find("Bake Navigation") != std::string::npos);
			// The written file is the mesh the asset loads.
			const std::optional<Buffer> bytes = ReadProjectBytes(fixture, first->Path);
			REQUIRE(bytes.has_value());
			const Result<NavMeshData> read = ReadNavMeshSource(*bytes);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->InputHash == first->InputHash);
			const Result<AssetRef<Asset>> loaded = editor.GetAssets().Load(first->Asset);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->GetAssetType() == AssetType::NavMesh);

			// A rebake of the unchanged scene: the same bytes, the same asset, no component change and no undo step.
			const Result<NavigationBakeResult> second = BakeNavigation(editor, editor.GetActiveSceneId());
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			CHECK_FALSE(second->Changed);
			CHECK(second->Asset == first->Asset);
			CHECK(second->InputHash == first->InputHash);
			CHECK(ReadProjectBytes(fixture, first->Path) == bytes);
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);

			// Moving a wall changes the input, so the next bake rewrites the file under the same asset.
			editor.GetScene().FindEntityByPath("/Pillar").Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.z += 2.0f;
			});
			const Result<NavigationBakeResult> moved = BakeNavigation(editor, editor.GetActiveSceneId());
			REQUIRE_MESSAGE(moved.has_value(), moved.error().ToString());
			CHECK(moved->Changed);
			CHECK(moved->Asset == first->Asset);
			CHECK(moved->InputHash != first->InputHash);
			CHECK(ReadProjectBytes(fixture, first->Path) != bytes);

			// Undoing the bake's step takes the assignment back, and redoing it assigns the mesh again.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK_FALSE(GetBakedHandle(editor).IsValid());
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			CHECK(GetBakedHandle(editor) == first->Asset);
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
		}

		// A write that fails (a file where the bake's directory goes: AlreadyExists, naming it) leaves everything as it was: no
		// file, no .meta, the component's NavMesh and the history unchanged.
		TEST_CASE("NavigationBake: a write that fails writes nothing and changes no component or history")
		{
			Test::EditorTestFixture fixture("NavigationBakeWriteFails");
			EditorContext& editor = fixture.GetEditor();
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Arena.scene");
			static_cast<void>(Test::AddNavigationArena(editor.GetScene()));
			const std::span<const std::byte> text = AsBytes(std::string_view("not a directory"));
			const Buffer blocker(text.begin(), text.end());
			REQUIRE(FileSystem::WriteFileAtomic(fixture.GetProjectRoot() / "Assets" / "Navigation", blocker).has_value());
			const size_t steps = editor.GetHistory().GetUndoCount();

			const Result<NavigationBakeResult> failed = BakeNavigation(editor, editor.GetActiveSceneId());
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK(ReadProjectBytes(fixture, "Assets/Navigation") == std::optional<Buffer>(blocker));
			CHECK_FALSE(GetBakedHandle(editor).IsValid());
			CHECK(editor.GetHistory().GetUndoCount() == steps);
		}

		// An import that fails (the .meta names an importer this build does not have) returns its error after the write: the
		// written file stays, and the component and the history are unchanged.
		TEST_CASE("NavigationBake: an import that fails returns its error, keeping the written file and changing no component or history")
		{
			Test::EditorTestFixture fixture("NavigationBakeImportFails");
			EditorContext& editor = fixture.GetEditor();
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Arena.scene");
			static_cast<void>(Test::AddNavigationArena(editor.GetScene()));
			// A source the registry knows, whose .meta names a missing importer.
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x4e415601u);
			metadata.Type = AssetType::NavMesh;
			metadata.Importer = "Missing";
			metadata.ImporterVersion = 1;
			const std::filesystem::path directory = fixture.GetProjectRoot() / "Assets" / "Navigation" / "Scenes";
			REQUIRE(FileSystem::CreateDirectories(directory).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Arena.navmesh", AsBytes(std::string_view("stale"))).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Arena.navmesh.meta", AsBytes(SerializeAssetMetadata(metadata))).has_value());
			const Test::ExpectLog importFailure(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Navigation/Scenes/Arena.navmesh");
			Test::AutomationTestClient client(editor, Test::MakeTestServerSpecification());
			REQUIRE(client.Call("project.refreshAssets", Json::object()).has_value());
			const size_t steps = editor.GetHistory().GetUndoCount();

			const Result<NavigationBakeResult> failed = BakeNavigation(editor, editor.GetActiveSceneId());
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::ImportFailed);
			const std::optional<Buffer> written = ReadProjectBytes(fixture, "Assets/Navigation/Scenes/Arena.navmesh");
			REQUIRE(written.has_value());
			CHECK(ReadNavMeshSource(*written).has_value());
			CHECK_FALSE(GetBakedHandle(editor).IsValid());
			CHECK(editor.GetHistory().GetUndoCount() == steps);
		}
	}

}
