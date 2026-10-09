#include "TestsPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>

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
	}

}
