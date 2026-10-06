#include "TestsPCH.h"

#include "EditorCore/Commands/SceneEdit.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/DeathTest.h"
#include "Support/EditorTestFixture.h"

namespace Engine {

	static std::string SaveTrackedScene(const Scene& scene)
	{
		const Result<std::string> text = SceneSerializer::SaveToString(scene);
		REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
		return *text;
	}

	ENGINE_DEATH_TEST("EditorCore/NestedSceneEditAsserts")
	{
		Test::EditorTestFixture fixture("NestedSceneEdit");
		fixture.CreateAndOpenProject();
		fixture.CreateAndOpenScene();
		SceneEdit outer(fixture.GetEditor(), "Outer");
		SceneEdit inner(fixture.GetEditor(), "Inner");
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("SceneEdit: commit records one command and returns its undo index" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SceneEditCommit");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			SceneEdit edit(editor, "Create Entity 'Board'");
			CHECK(edit.IsActive());
			CHECK(&edit.GetScene() == &editor.GetScene());
			static_cast<void>(editor.GetScene().CreateEntity("Board"));
			const Result<uint64_t> undoIndex = edit.Commit();
			REQUIRE(undoIndex.has_value());
			CHECK(*undoIndex == editor.GetHistory().GetCurrentSequence());
			CHECK_FALSE(edit.IsActive());
			CHECK(editor.GetHistory().GetUndoLabel() == "Create Entity 'Board'");
			CHECK(editor.IsSceneDirty());
		}

		TEST_CASE("SceneEdit: an edit dropped without commit restores the scene" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SceneEditRollback");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const std::string before = SaveTrackedScene(editor.GetScene());
			const uint64_t revision = editor.GetScene().GetRevision();
			{
				SceneEdit edit(editor, "Abandoned");
				const Entity entity = editor.GetScene().CreateEntity("Temporary");
				entity.AddTag("Gone");
			}
			CHECK(SaveTrackedScene(editor.GetScene()) == before);
			CHECK(editor.GetScene().GetRevision() > revision); // the rollback is itself a change of the scene
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			CHECK_FALSE(editor.IsSceneDirty());
		}

		TEST_CASE("SceneEdit: an edit that changes nothing records nothing" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SceneEditEmpty");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			SceneEdit edit(fixture.GetEditor(), "Nothing");
			const Result<uint64_t> undoIndex = edit.Commit();
			REQUIRE(undoIndex.has_value());
			CHECK(*undoIndex == 0);
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("SceneEdit: a second edit while one is active asserts" * doctest::skip(true))
		{
			ENGINE_CHECK_DEATH("EditorCore/NestedSceneEditAsserts", "SceneEdit");
		}
	}

}
