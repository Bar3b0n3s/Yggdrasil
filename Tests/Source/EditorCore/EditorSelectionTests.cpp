#include "TestsPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>

#include <chrono>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorSelection: a dry-run entity selection preserves the selected asset")
		{
			Test::EditorTestFixture fixture("SelectionAssetDryRun");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const UUID entity = editor.GetScene().CreateEntity("Entity").GetUUID();
			const AssetHandle asset(987);
			editor.GetUiState().SetSelectedAsset(asset);
			{
				auto dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun);
				REQUIRE(editor.SetSelection({ entity }, SceneTarget::Edit));
				CHECK(editor.GetUiState().GetSelectedAsset() == AssetHandle{});
			}
			CHECK(editor.GetSelection().empty());
			CHECK(editor.GetUiState().GetSelectedAsset() == asset);
		}

		TEST_CASE("EditorSelection: dry runs restore the selected scene as well as its UUIDs")
		{
			Test::EditorTestFixture fixture("SelectionDryRun");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const UUID original = editor.GetScene().CreateEntity("Original").GetUUID();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			REQUIRE(editor.SetSelection({ original }, SceneTarget::Play));
			{
				auto dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE(dryRun);
				REQUIRE(editor.SetSelection({ original }, SceneTarget::Edit));
				CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			}
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == original);
		}

		TEST_CASE("EditorSelection: an absent target rejects even an empty selection")
		{
			Test::EditorTestFixture fixture("SelectionAbsent");
			const auto edit = fixture.GetEditor().SetSelection({}, SceneTarget::Edit);
			REQUIRE_FALSE(edit);
			CHECK(edit.error().GetCode() == ErrorCode::InvalidState);
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const auto play = fixture.GetEditor().SetSelection({}, SceneTarget::Play);
			REQUIRE_FALSE(play);
			CHECK(play.error().GetCode() == ErrorCode::InvalidState);
			CHECK(fixture.GetEditor().GetSelectionTarget() == SceneTarget::Edit);
		}

		TEST_CASE("EditorSelection: a runtime-only UUID can be selected without touching the edit scene")
		{
			Test::EditorTestFixture fixture("SelectionRuntime");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			auto entity = editor.GetPlay().GetSession()->CreateEntity("Runtime");
			REQUIRE(entity);
			const uint64_t revision = editor.GetRevision();
			REQUIRE(editor.SetSelection({ entity->GetUUID() }, SceneTarget::Play));
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == entity->GetUUID());
			CHECK_FALSE(editor.GetScene().FindEntityByID(entity->GetUUID()).IsValid());
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("EditorSelection: invalid selection is atomic and Stop restores edit UUIDs")
		{
			Test::EditorTestFixture fixture("SelectionAtomic");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const UUID original = editor.GetScene().CreateEntity("Original").GetUUID();
			REQUIRE(editor.SetSelection({ original }, SceneTarget::Edit));
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			auto spawned = editor.GetPlay().GetSession()->CreateEntity("Runtime");
			REQUIRE(spawned);
			REQUIRE(editor.SetSelection({ original, spawned->GetUUID(), original }, SceneTarget::Play));
			CHECK(editor.GetSelection().size() == 2);
			const auto invalid = editor.SetSelection({ original, UUID(123) }, SceneTarget::Edit);
			REQUIRE_FALSE(invalid);
			CHECK(invalid.error().GetCode() == ErrorCode::NotFound);
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			CHECK(editor.GetSelection().size() == 2);
			REQUIRE(editor.GetPlay().Stop());
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == original);
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
		}

		TEST_CASE("EditorSelection: Play carries an edit selection into the runtime scene by UUID")
		{
			Test::EditorTestFixture fixture("SelectionFollowsPlay");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const UUID kept = editor.GetScene().CreateEntity("Kept").GetUUID();
			REQUIRE(editor.SetSelection({ kept }, SceneTarget::Edit));
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == kept);
			REQUIRE(editor.GetPlay().Stop());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == kept);
			// An asset selection (no entity selected) is not touched by starting play.
			const AssetHandle asset(4242);
			REQUIRE(editor.SetSelection({}, SceneTarget::Edit));
			editor.GetUiState().SetSelectedAsset(asset);
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			CHECK(editor.GetUiState().GetSelectedAsset() == asset);
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			REQUIRE(editor.GetPlay().Stop());
		}

		TEST_CASE("EditorSelection: Scene.Load and a session restart clear a runtime selection")
		{
			Test::AutomationFixture fixture("SelectionSceneReplacement");
			auto& editor = fixture.GetEditor();
			REQUIRE(fixture.Call("entity.create", Json{ { "name", "Enemy" } }));
			const UUID enemy = editor.GetScene().GetRootEntities().front();
			REQUIRE(fixture.Call("scene.save", Json::object()));
			REQUIRE(editor.GetAssets().Refresh());
			editor.GetAssets().WaitIdle();
			REQUIRE(fixture.Call("play.start", Json{ { "paused", true } }));
			REQUIRE(editor.SetSelection({ enemy }, SceneTarget::Play));
			const uint64_t serial = editor.GetPlay().GetSession()->GetSerial();
			SUBCASE("a gameplay Scene.Load swaps the runtime scene within the session")
			{
				REQUIRE(fixture.Call("script.eval", Json{ { "context", "play" }, { "code", "Scene.Load(Assets.Load('Assets/Scenes/Main.scene'), {})" } }));
				REQUIRE(fixture.Call("play.step", Json{ { "ticks", 1 } }));
				CHECK(editor.GetPlay().GetSession()->GetSerial() == serial);
			}
			SUBCASE("a recording restart replaces the session")
			{
				REQUIRE(fixture.Call("input.record", Json{ { "action", "start" }, { "restart", true } }));
				CHECK(editor.GetPlay().GetSession()->GetSerial() != serial);
			}
			// The replacement holds an entity with the same UUID, which must not become selected by accident.
			REQUIRE(editor.GetPlay().GetSession()->GetScene().FindEntityByID(enemy).IsValid());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			CHECK(editor.GetSelection().empty());
			const auto reported = fixture.Call("edit.getSelection", Json::object());
			REQUIRE(reported);
			CHECK((*reported)["selection"].empty());
			// Stop restores only what the runtime selection still held: nothing.
			REQUIRE(editor.GetPlay().Stop());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			CHECK(editor.GetSelection().empty());
		}

		TEST_CASE("EditorSelection: a finished test run restores the selection without ids that vanished during it")
		{
			Test::EditorTestFixture fixture("SelectionTestRunRestore");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), std::move(specification));
			REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Tests/Selection.test.luau" }, { "source", "return Test.Suite('Selection', function() Test.Case('long', function() Test.WaitTicks(1000) end) end)" } }));
			REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Selection.test.luau" }, { "Scene", "" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } }));
			auto& editor = fixture.GetEditor();
			const UUID kept = editor.GetScene().CreateEntity("Kept").GetUUID();
			const UUID vanishing = editor.GetScene().CreateEntity("Vanishing").GetUUID();
			REQUIRE(editor.SetSelection({ kept, vanishing }, SceneTarget::Edit));
			static_cast<void>(client.Submit("test.run", Json::object()));
			client.GetServer().Pump();
			REQUIRE(editor.GetPlay().IsTestRunActive());
			// A change no command path makes during the lease, applied straight through the scene API.
			editor.GetScene().DestroyEntity(editor.GetScene().FindEntityByID(vanishing));
			client.GetServer().DisconnectInProcess(client.GetClient());
			REQUIRE_FALSE(editor.GetPlay().IsTestRunActive());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == kept);
		}

		TEST_CASE("EditorSelection: pruning drops destroyed entities and keeps the remaining selection")
		{
			Test::AutomationFixture fixture("SelectionPrune");
			auto& editor = fixture.GetEditor();
			const UUID first = editor.GetScene().CreateEntity("First").GetUUID();
			const UUID second = editor.GetScene().CreateEntity("Second").GetUUID();
			REQUIRE(editor.SetSelection({ first, second }, SceneTarget::Edit));
			REQUIRE(fixture.Call("entity.destroy", Json{ { "entities", Json::array({ second.ToString() }) } }));
			editor.Update(0.0);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == first);
			REQUIRE(fixture.Call("edit.undo", Json::object()));
			editor.PruneSelection();
			CHECK(editor.GetSelection().size() == 1);
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			auto runtime = editor.GetPlay().GetSession()->CreateEntity("Runtime");
			REQUIRE(runtime);
			REQUIRE(editor.SetSelection({ first, runtime->GetUUID() }, SceneTarget::Play));
			editor.GetPlay().GetSession()->GetScene().DestroyEntity(*runtime);
			editor.GetPlay().GetSession()->GetScene().FlushPendingDestroys();
			editor.PruneSelection();
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == first);
			REQUIRE(editor.GetPlay().Stop());
		}
	}

}
