#include "TestsPCH.h"

#include "EditorCore/Commands/SceneEdit.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
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
		TEST_CASE("SceneEdit: commit records one command and returns its undo index")
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

		TEST_CASE("SceneEdit: an edit dropped without commit restores the scene")
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

		TEST_CASE("SceneEdit: an edit that changes nothing records nothing")
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

		TEST_CASE("SceneEdit: a second edit while one is active asserts")
		{
			ENGINE_CHECK_DEATH("EditorCore/NestedSceneEditAsserts", "SceneEdit");
		}

		TEST_CASE("SceneEdit: play-scene changes are transient: no command, undo index 0, the edit scene untouched")
		{
			Test::EditorTestFixture fixture("SceneEditPlay");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			static_cast<void>(editor.GetScene().CreateEntity("Ball"));
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }).has_value());
			const std::string editBefore = SaveTrackedScene(editor.GetScene());
			const uint64_t revision = editor.GetRevision();
			const size_t undoCount = editor.GetHistory().GetUndoCount();
			Scene& play = editor.GetPlay().GetSession()->GetScene();
			{
				SceneEdit edit(editor, "Move Ball");
				play.FindEntityByPath("/Ball").Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation.x = 3.0f;
				});
				static_cast<void>(play.CreateEntity("Spawned"));
				const Result<uint64_t> undoIndex = edit.Commit();
				REQUIRE_MESSAGE(undoIndex.has_value(), undoIndex.error().ToString());
				CHECK(*undoIndex == 0);
			}
			CHECK(play.FindEntityByPath("/Spawned").IsValid());
			CHECK(play.FindEntityByPath("/Ball").GetComponent<TransformComponent>().Translation.x == 3.0f);
			CHECK(SaveTrackedScene(editor.GetScene()) == editBefore);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount);

			// A dropped edit rolls the play scene back like the edit scene.
			{
				SceneEdit edit(editor, "Abandoned");
				static_cast<void>(play.CreateEntity("Temporary"));
			}
			CHECK_FALSE(play.FindEntityByPath("/Temporary").IsValid());

			// Stop discards the play-scene changes with the session.
			REQUIRE(editor.GetPlay().Stop().has_value());
			CHECK(SaveTrackedScene(editor.GetScene()) == editBefore);
		}

		TEST_CASE("SceneEdit: a dry run and a rolled-back transaction take back their play-scene changes; a committed one keeps them")
		{
			Test::EditorTestFixture fixture("SceneEditPlayUndo");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }).has_value());
			Scene& play = editor.GetPlay().GetSession()->GetScene();
			const std::string playBefore = SaveTrackedScene(play);
			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE_MESSAGE(dryRun.has_value(), dryRun.error().ToString());
				SceneEdit edit(editor, "Dry Run");
				static_cast<void>(play.CreateEntity("DryRun"));
				REQUIRE(edit.Commit().has_value());
				// Still there until the dry run ends, so its result can name what it would create.
				CHECK(play.FindEntityByPath("/DryRun").IsValid());
			}
			CHECK(SaveTrackedScene(play) == playBefore);

			{
				EditorTransaction transaction(editor, "Abandoned Batch");
				SceneEdit edit(editor, "Batched");
				static_cast<void>(play.CreateEntity("Batched"));
				REQUIRE(edit.Commit().has_value());
			}
			CHECK(SaveTrackedScene(play) == playBefore);

			{
				EditorTransaction transaction(editor, "Kept Batch");
				{
					SceneEdit edit(editor, "Kept");
					static_cast<void>(play.CreateEntity("Kept"));
					REQUIRE(edit.Commit().has_value());
				}
				CHECK(transaction.Commit() == 0);
			}
			CHECK(play.FindEntityByPath("/Kept").IsValid());
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("SceneEdit: taking back play-scene changes winds the session's id generator back, so the state hash is unchanged")
		{
			// §13.4: a dry run leaves no trace and a failed transaction none either; a play-scene creation draws a runtime id
			// from the session's seeded generator (§4.8), whose draw count the state hash includes.
			Test::EditorTestFixture fixture("SceneEditPlayIds");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			static_cast<void>(editor.GetScene().CreateEntity("Ball"));
			const PlayStartOptions options{ .Lockstep = true, .Seed = 42 };
			REQUIRE(editor.GetPlay().Start(options).has_value());
			PlaySession& session = *editor.GetPlay().GetSession();
			const uint64_t hashBefore = session.ComputeStateHash();
			const uint64_t drawsBefore = session.GetIdGenerator().GetDrawCount();
			const auto checkUnchanged = [&session, hashBefore, drawsBefore]()
			{
				CHECK(session.GetIdGenerator().GetDrawCount() == drawsBefore);
				CHECK(session.ComputeStateHash() == hashBefore);
			};

			{
				Result<Scope<EditorDryRunScope>> dryRun = EditorDryRunScope::Begin(editor);
				REQUIRE_MESSAGE(dryRun.has_value(), dryRun.error().ToString());
				SceneEdit edit(editor, "Dry Run");
				static_cast<void>(session.GetScene().CreateEntity("DryRun"));
				REQUIRE(edit.Commit().has_value());
			}
			checkUnchanged();

			{
				EditorTransaction transaction(editor, "Abandoned Batch");
				SceneEdit edit(editor, "Batched");
				static_cast<void>(session.GetScene().CreateEntity("Batched"));
				REQUIRE(edit.Commit().has_value());
			}
			checkUnchanged();

			{
				SceneEdit edit(editor, "Abandoned");
				static_cast<void>(session.GetScene().CreateEntity("Abandoned"));
			}
			checkUnchanged();

			// The first creation that stays gets the id a fresh session of the same seed gives its first creation.
			UUID kept;
			{
				SceneEdit edit(editor, "Kept");
				kept = session.GetScene().CreateEntity("Kept").GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(editor.GetPlay().Stop().has_value());
			REQUIRE(editor.GetPlay().Start(options).has_value());
			CHECK(editor.GetPlay().GetSession()->GetScene().CreateEntity("Fresh").GetUUID() == kept);
		}

		TEST_CASE("SceneEdit: a transient undo of an ended session leaves its successor alone")
		{
			Test::EditorTestFixture fixture("SceneEditPlaySuccessor");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .Seed = 5 }).has_value());
			{
				EditorTransaction transaction(editor, "Outlives Its Session");
				{
					SceneEdit edit(editor, "Spawned");
					static_cast<void>(play.GetSession()->GetScene().CreateEntity("Spawned"));
					REQUIRE(edit.Commit().has_value());
				}
				// The session ends and another starts (possibly at the same address) before the transaction rolls back.
				REQUIRE(play.Stop().has_value());
				REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .Seed = 5 }).has_value());
				static_cast<void>(play.GetSession()->GetScene().CreateEntity("Successor"));
			}
			// The rollback's undo belonged to the first session: the second keeps its entity and its draws.
			CHECK(play.GetSession()->GetScene().FindEntityByPath("/Successor").IsValid());
			CHECK(play.GetSession()->GetIdGenerator().GetDrawCount() == 1);
		}
	}

}
