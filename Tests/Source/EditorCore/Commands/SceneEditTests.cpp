#include "TestsPCH.h"

#include "EditorCore/Commands/SceneEdit.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
#include "Support/DeathTest.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static std::string SaveTrackedScene(const Scene& scene)
	{
		const Result<std::string> text = SceneSerializer::SaveToString(scene);
		REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
		return *text;
	}

	// A real imported script and prefab, independent of the automation handlers that also edit prefab instances.
	static Entity CreateTrackedScriptPrefab(EditorContext& editor)
	{
		const auto scriptPath = VfsPath::Create("project", "Assets/Links.luau");
		REQUIRE(scriptPath.has_value());
		REQUIRE(editor.GetScriptService() != nullptr);
		const auto written = editor.GetScriptService()->Write(*scriptPath,
			"return Script.Define(\"Links\", { Fields = { Target = Field.Entity(), Targets = Field.Array(Field.Entity()), Amount = Field.Number(1) } })");
		REQUIRE_MESSAGE(written.has_value(), (written ? std::string() : written.error().ToString()));
		const auto schemas = editor.GetScriptSchemaSnapshot();
		REQUIRE(schemas.has_value());
		REQUIRE((*schemas)->FindSchema(written->Script, "Target").has_value());

		Scope<Scene> source = editor.CreateScene("PrefabSource");
		const Entity root = source->CreateEntity("Links");
		const UUID first = source->CreateEntity("First", root).GetUUID();
		const UUID second = source->CreateEntity("Second", root).GetUUID();
		ScriptComponent script;
		script.Script.SetHandle(written->Script);
		script.Fields["Target"] = VariantValue(Json(first.ToString()));
		script.Fields["Targets"] = VariantValue(Json::array({ first.ToString(), second.ToString() }));
		script.Fields["Amount"] = VariantValue(Json(1.0f));
		root.AddComponent<ScriptComponent>(script);
		const auto prefab = Prefab::CreateFromEntity(root, "Links");
		REQUIRE(prefab.has_value());
		const auto text = prefab->SaveToString();
		REQUIRE(text.has_value());
		const auto path = VfsPath::Create("project", "Assets/Links.prefab");
		REQUIRE(path.has_value());
		REQUIRE(editor.WriteProjectFile(*path, AsBytes(*text)).has_value());
		REQUIRE(editor.GetAssets().Refresh().has_value());
		const auto handle = editor.GetAssets().Resolve("Assets/Links.prefab");
		REQUIRE(handle.has_value());
		LoadReport report;
		const auto instance = InstantiatePrefabAsset(editor.GetScene(), editor.GetAssets(),
			PrefabInstantiateOptions{ .PrefabHandle = *handle, .RootID = UUID(0x1234a), .Parent = {}, .SiblingIndex = {}, .RootTransform = {} },
			PrefabOptions{ .Schemas = schemas->get() }, report);
		REQUIRE_MESSAGE(instance.has_value(), (instance ? std::string() : instance.error().ToString()));
		editor.GetHistory().Clear();
		return *instance;
	}

	static Json TrackedScriptField(ConstEntity entity, std::string_view name)
	{
		const auto& fields = entity.GetComponent<ScriptComponent>().Fields;
		const auto field = fields.find(std::string(name));
		REQUIRE(field != fields.end());
		return field->second.Get();
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
		TEST_CASE("SceneEdit: script entity overrides use prefab-local IDs and undo with their values")
		{
			Test::EditorTestFixture fixture("SceneEditScriptPrefab");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			const UUID rootID = CreateTrackedScriptPrefab(editor).GetUUID();
			const Entity first = scene.FindEntityByPath("/Links/First");
			const Entity second = scene.FindEntityByPath("/Links/Second");
			REQUIRE(first.IsValid());
			REQUIRE(second.IsValid());
			const UUID firstID = first.GetUUID();
			const UUID secondID = second.GetUUID();
			const UUID prefabFirst = first.GetComponent<PrefabLinkComponent>().PrefabEntityID;
			const UUID prefabSecond = second.GetComponent<PrefabLinkComponent>().PrefabEntityID;
			bool playing = false;
			SUBCASE("edit mode")
			{
			}
			SUBCASE("paused play session")
			{
				playing = true;
			}
			if (playing)
				REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true, .Paused = true }).has_value());

			// Touching only an implicit root override must not mistake remapped script references for authored changes.
			{
				SceneEdit edit(editor, "Rename instance");
				scene.FindEntityByID(rootID).SetName("Renamed");
				REQUIRE(edit.Commit().has_value());
			}
			REQUIRE(scene.FindEntityByID(rootID).GetComponent<PrefabInstanceComponent>().Overrides.empty());
			const std::string before = SaveTrackedScene(scene);
			{
				SceneEdit edit(editor, "Change script targets");
				scene.FindEntityByID(rootID).Patch<ScriptComponent>([firstID, secondID](ScriptComponent& script)
				{
					script.Fields["Target"] = VariantValue(Json(secondID.ToString()));
					script.Fields["Targets"] = VariantValue(Json::array({ secondID.ToString(), firstID.ToString(), secondID.ToString() }));
				});
				REQUIRE(edit.Commit().has_value());
			}
			const auto overrides = scene.FindEntityByID(rootID).GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(overrides.size() == 1);
			CHECK(overrides[0].Kind == PrefabOverrideKind::Field);
			CHECK(overrides[0].Component == "Script");
			CHECK(overrides[0].Field == "Fields");
			CHECK(overrides[0].Value.Get() == Json{ { "Target", prefabSecond.ToString() }, { "Targets", Json::array({ prefabSecond.ToString(), prefabFirst.ToString(), prefabSecond.ToString() }) } });
			const std::string after = SaveTrackedScene(scene);
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(SaveTrackedScene(scene) == before);
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			CHECK(SaveTrackedScene(scene) == after);

			if (playing)
			{
				Scene& play = editor.GetPlay().GetSession()->GetScene();
				CHECK(TrackedScriptField(play.FindEntityByID(rootID), "Target") == Json(firstID.ToString()));
				const size_t undoCount = editor.GetHistory().GetUndoCount();
				{
					SceneEdit edit(editor, "Transient script target");
					play.FindEntityByID(rootID).Patch<ScriptComponent>([secondID](ScriptComponent& script)
					{
						script.Fields["Target"] = VariantValue(Json(secondID.ToString()));
					});
					const auto committed = edit.Commit();
					REQUIRE(committed.has_value());
					CHECK(*committed == 0);
				}
				CHECK(editor.GetHistory().GetUndoCount() == undoCount);
				CHECK(play.FindEntityByID(rootID).GetComponent<PrefabInstanceComponent>().Overrides.empty());
				CHECK(SaveTrackedScene(scene) == after);
				REQUIRE(editor.GetPlay().Stop().has_value());
			}
		}

		TEST_CASE("SceneEdit: prefab rebuilds within and outside an edit retain script references and authored overrides")
		{
			Test::EditorTestFixture fixture("EditorScriptPrefabRebuild");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			const UUID rootID = CreateTrackedScriptPrefab(editor).GetUUID();
			const UUID first = scene.FindEntityByPath("/Links/First").GetUUID();
			const UUID second = scene.FindEntityByPath("/Links/Second").GetUUID();
			{
				SceneEdit edit(editor, "Change amount");
				scene.FindEntityByID(rootID).Patch<ScriptComponent>([](ScriptComponent& script)
				{
					script.Fields["Amount"] = VariantValue(Json(2.0f));
				});
				REQUIRE(edit.Commit().has_value());
			}
			const std::string before = SaveTrackedScene(scene);
			SUBCASE("without an active edit")
			{
				const auto rebuilt = editor.UpdatePrefabInstances(scene, {});
				REQUIRE_MESSAGE(rebuilt.has_value(), (rebuilt ? std::string() : rebuilt.error().ToString()));
				CHECK_FALSE(*rebuilt);
			}
			SUBCASE("inside an active edit")
			{
				SceneEdit edit(editor, "Rebuild instances");
				const auto rebuilt = editor.UpdatePrefabInstances(scene, {});
				REQUIRE_MESSAGE(rebuilt.has_value(), (rebuilt ? std::string() : rebuilt.error().ToString()));
				CHECK_FALSE(*rebuilt);
				const auto committed = edit.Commit();
				REQUIRE(committed.has_value());
			}
			CHECK(SaveTrackedScene(scene) == before);
			const Entity root = scene.FindEntityByID(rootID);
			CHECK(TrackedScriptField(root, "Target") == Json(first.ToString()));
			CHECK(TrackedScriptField(root, "Targets") == Json::array({ first.ToString(), second.ToString() }));
			CHECK(TrackedScriptField(root, "Amount") == Json(2));
			const auto& overrides = root.GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(overrides.size() == 1);
			CHECK(overrides[0].Value.Get() == Json{ { "Amount", 2 } });
		}

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
