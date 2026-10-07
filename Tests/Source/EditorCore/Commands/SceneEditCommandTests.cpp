#include "TestsPCH.h"

#include "EditorCore/Commands/SceneEditCommand.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static std::string SaveEditScene(const Scene& scene)
	{
		const Result<std::string> text = SceneSerializer::SaveToString(scene);
		REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
		return *text;
	}

	static Json ParseEditJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	// One random edit of the open scene through the normal Scene, Entity and ComponentAccess APIs, inside one SceneEdit: a
	// rejected change (a cycle, a missing requirement) cancels the edit, which must leave the scene unchanged.
	static void ApplyRandomEdit(EditorContext& editor, Random& random)
	{
		Scene& scene = editor.GetScene();
		const std::span<const UUID> order = scene.GetCanonicalOrder();
		const auto pick = [&scene, &random, order]() -> Entity
		{
			if (order.empty())
				return {};
			return scene.FindEntityByID(order[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(order.size()) - 1))]);
		};
		static constexpr std::array<std::string_view, 5> Components = { "Camera", "PointLight", "RigidBody", "BoxCollider", "AudioSource" };

		SceneEdit edit(editor, "Random edit");
		bool applied = true;
		switch (random.RangeInt(0, 7))
		{
			case 0:
			{
				const Entity parent = random.NextBool() ? pick() : Entity();
				const Entity created = parent.IsValid() ? scene.CreateEntity("Created", parent) : scene.CreateEntity("Created");
				applied = created.IsValid();
				break;
			}
			case 1:
			{
				const Entity victim = pick();
				if (victim.IsValid())
					scene.DestroyEntity(victim);
				break;
			}
			case 2:
			{
				const Entity child = pick();
				const Entity parent = random.NextBool() ? pick() : Entity();
				applied = child.IsValid() && scene.SetParent(child, parent, std::nullopt, random.NextBool()).has_value();
				break;
			}
			case 3:
			{
				if (const Entity entity = pick(); entity.IsValid())
					entity.SetName(std::format("Name{}", random.RangeInt(0, 99)));
				break;
			}
			case 4:
			{
				if (const Entity entity = pick(); entity.IsValid())
					entity.SetActive(!entity.IsActiveSelf());
				break;
			}
			case 5:
			{
				if (const Entity entity = pick(); entity.IsValid())
				{
					const std::string tag = std::format("Tag{}", random.RangeInt(0, 3));
					if (entity.HasTag(tag))
						entity.RemoveTag(tag);
					else
						entity.AddTag(tag);
				}
				break;
			}
			case 6:
			{
				const Entity entity = pick();
				const std::string_view component = Components[static_cast<size_t>(random.RangeInt(0, Components.size() - 1))];
				if (!entity.IsValid())
					break;
				if (ComponentAccess::GetComponentJson(entity, component).has_value())
					applied = ComponentAccess::RemoveComponent(entity, component).has_value();
				else
					applied = ComponentAccess::AddComponent(entity, component, nullptr).has_value();
				break;
			}
			default:
			{
				if (const Entity entity = pick(); entity.IsValid())
				{
					const Json patch = ParseEditJson(std::format(R"({{"Translation":[{},{},{}]}})", random.RangeInt(-50, 50),
						random.RangeInt(-50, 50), random.RangeInt(-50, 50)));
					applied = ComponentAccess::PatchComponentJson(entity, "Transform", patch).has_value();
				}
				break;
			}
		}
		if (!applied)
		{
			edit.Cancel();
			return;
		}
		const Result<uint64_t> committed = edit.Commit();
		REQUIRE_MESSAGE(committed.has_value(), committed.error().ToString());
	}

	TEST_SUITE("EditorCore")
	{
		// Debug builds check every untouched entity after each of the 10,000 edits (SceneEdit), which takes far longer than a
		// typical test on slow CI machines, hence the generous limit.
		TEST_CASE("SceneEditCommand: 10,000 random operations undo to byte-identical JSON and redo to the final state" * doctest::timeout(600.0))
		{
			Test::EditorTestFixture fixture("SceneEditProperty", CommandHistoryLimits{ .MaxEntries = 20000, .MaxBytes = 1ull << 34 });
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();

			Random random(20261006);
			for (int index = 0; index < 8; ++index)
				ApplyRandomEdit(editor, random);
			const std::string initialScene = SaveEditScene(editor.GetScene());
			const size_t initialUndo = editor.GetHistory().GetUndoCount();

			for (int index = 0; index < 10000; ++index)
				ApplyRandomEdit(editor, random);
			const std::string finalScene = SaveEditScene(editor.GetScene());
			const size_t steps = editor.GetHistory().GetUndoCount() - initialUndo;

			CHECK(editor.GetHistory().Undo(editor, steps) == steps);
			CHECK(SaveEditScene(editor.GetScene()) == initialScene);
			const Result<size_t> redone = editor.GetHistory().Redo(editor, steps);
			REQUIRE(redone.has_value());
			CHECK(*redone == steps);
			CHECK(SaveEditScene(editor.GetScene()) == finalScene);
		}

		TEST_CASE("SceneEditCommand: Execute, Undo and Execute again reproduce each operation exactly")
		{
			Test::EditorTestFixture fixture("SceneEditRoundTrip");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Random random(7);
			for (int operation = 0; operation < 200; ++operation)
			{
				const std::string before = SaveEditScene(editor.GetScene());
				const size_t undoCount = editor.GetHistory().GetUndoCount();
				ApplyRandomEdit(editor, random);
				if (editor.GetHistory().GetUndoCount() == undoCount)
				{
					CHECK(SaveEditScene(editor.GetScene()) == before); // a cancelled edit changed nothing
					continue;
				}
				const std::string after = SaveEditScene(editor.GetScene());
				CHECK(editor.GetHistory().Undo(editor) == 1);
				CHECK(SaveEditScene(editor.GetScene()) == before);
				REQUIRE(editor.GetHistory().Redo(editor).has_value());
				CHECK(SaveEditScene(editor.GetScene()) == after);
			}
		}

		TEST_CASE("SceneEditCommand: undo restores the sibling order after a multi-entity destroy")
		{
			Test::EditorTestFixture fixture("SceneEditSiblings");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			{
				SceneEdit edit(editor, "Create children");
				const Entity root = scene.CreateEntity("Root");
				for (int index = 0; index < 5; ++index)
					static_cast<void>(scene.CreateEntity(std::format("Child{}", index), root));
				REQUIRE(edit.Commit().has_value());
			}
			const std::string before = SaveEditScene(scene);
			{
				SceneEdit edit(editor, "Destroy 1 and 3");
				scene.DestroyEntity(scene.FindEntityByPath("/Root/Child3"));
				scene.DestroyEntity(scene.FindEntityByPath("/Root/Child1"));
				REQUIRE(edit.Commit().has_value());
			}
			CHECK(editor.GetHistory().Undo(editor) == 1);
			CHECK(SaveEditScene(scene) == before);
		}

		TEST_CASE("SceneEditCommand: merging keeps the first Before and the last After")
		{
			Test::EditorTestFixture fixture("SceneEditMerge");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			Entity entity;
			{
				SceneEdit edit(editor, "Create");
				entity = scene.CreateEntity("Dragged");
				REQUIRE(edit.Commit().has_value());
			}
			const std::string before = SaveEditScene(scene);
			const std::string mergeKey = "SceneEdit:Transform:" + entity.GetUUID().ToString();
			for (int step = 1; step <= 3; ++step)
			{
				SceneEdit edit(editor, "Move", mergeKey);
				REQUIRE(ComponentAccess::PatchComponentJson(entity, "Transform",
					ParseEditJson(std::format(R"({{"Translation":[{},0,0]}})", step)))
						.has_value());
				REQUIRE(edit.Commit().has_value());
			}
			CHECK(editor.GetHistory().GetUndoCount() == 2);
			CHECK(editor.GetHistory().Undo(editor) == 1);
			CHECK(SaveEditScene(scene) == before);
		}

		TEST_CASE("SceneEditCommand: a constructed command reports its changes and starts applied")
		{
			const SceneEntityChange change{ .EntityID = UUID(0x1234),
				.Before = nullptr,
				.After = CreateRef<const Json>(ParseEditJson(R"({"ID":"0000000000001234"})")),
				.ParentBefore = UUID(),
				.SiblingIndexBefore = 0,
				.ParentAfter = UUID(),
				.SiblingIndexAfter = 0 };
			SceneEditCommand command("Create Entity", { change }, "key");
			CHECK(command.IsApplied());
			CHECK(command.GetLabel() == "Create Entity");
			CHECK(command.GetMergeKey() == "key");
			REQUIRE(command.GetChanges().size() == 1);
			CHECK(command.GetChanges()[0].EntityID == UUID(0x1234));
			CHECK(command.GetMemorySize() > 0);
		}

		TEST_CASE("SceneEditCommand: a unique-per-scene component moved between entities in one edit undoes and redoes")
		{
			Test::EditorTestFixture fixture("SceneEditUnique");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			Entity first;
			Entity second;
			{
				SceneEdit edit(editor, "Create");
				first = scene.CreateEntity("First");
				second = scene.CreateEntity("Second");
				REQUIRE(ComponentAccess::AddComponent(first, "Environment", nullptr).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			const std::string before = SaveEditScene(scene);
			{
				SceneEdit edit(editor, "Move the environment");
				REQUIRE(ComponentAccess::RemoveComponent(first, "Environment").has_value());
				REQUIRE(ComponentAccess::AddComponent(second, "Environment", nullptr).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			const std::string after = SaveEditScene(scene);
			CHECK(editor.GetHistory().Undo(editor) == 1);
			CHECK(SaveEditScene(scene) == before);
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			CHECK(SaveEditScene(scene) == after);
		}

		TEST_CASE("SceneEditCommand: merged edits that touch different entities restore both child orders")
		{
			Test::EditorTestFixture fixture("SceneEditMergeOrder");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			Entity root;
			{
				SceneEdit edit(editor, "Create");
				root = scene.CreateEntity("Root");
				static_cast<void>(scene.CreateEntity("X", root));
				static_cast<void>(scene.CreateEntity("Y", root));
				REQUIRE(edit.Commit().has_value());
			}
			const std::string before = SaveEditScene(scene);
			{
				SceneEdit edit(editor, "Arrange", "Arrange:Root");
				REQUIRE(scene.SetParent(scene.FindEntityByPath("/Root/Y"), root, 0u).has_value()); // Y, X
				REQUIRE(edit.Commit().has_value());
			}
			{
				SceneEdit edit(editor, "Arrange", "Arrange:Root");
				scene.FindEntityByPath("/Root/X").SetName("Renamed");
				const Entity added = scene.CreateEntity("Z", root);
				REQUIRE(scene.SetParent(added, root, 0u).has_value()); // Z, Y, Renamed
				REQUIRE(edit.Commit().has_value());
			}
			const std::string after = SaveEditScene(scene);
			CHECK(editor.GetHistory().GetUndoCount() == 2); // the two arrangements merged
			CHECK(editor.GetHistory().Undo(editor) == 1);
			CHECK(SaveEditScene(scene) == before);
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			CHECK(SaveEditScene(scene) == after);
		}

		TEST_CASE("SceneEditCommand: prefab instances undo and redo with their instance links")
		{
			Test::EditorTestFixture fixture("SceneEditPrefab");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			Entity source;
			{
				SceneEdit edit(editor, "Create the source");
				source = scene.CreateEntity("Block");
				static_cast<void>(scene.CreateEntity("Part", source));
				REQUIRE(edit.Commit().has_value());
			}
			const Result<Prefab> prefab = Prefab::CreateFromEntity(source, "Block");
			REQUIRE_MESSAGE(prefab.has_value(), prefab.error().ToString());

			const std::string empty = SaveEditScene(scene);
			UUID rootID;
			{
				SceneEdit edit(editor, "Instantiate 'Block'");
				LoadReport report;
				const Result<Entity> instance = PrefabInstantiator::Instantiate(scene, *prefab,
					{ .PrefabHandle = {}, .RootID = editor.GetIdGenerator().Next(), .Parent = {}, .SiblingIndex = std::nullopt, .RootTransform = std::nullopt }, {},
					report);
				REQUIRE_MESSAGE(instance.has_value(), instance.error().ToString());
				rootID = instance->GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			const std::string instantiated = SaveEditScene(scene);
			CHECK(instantiated.contains("\"PrefabLink\""));
			{
				SceneEdit edit(editor, "Destroy the instance");
				scene.DestroyEntity(scene.FindEntityByID(rootID));
				REQUIRE(edit.Commit().has_value());
			}
			const std::string destroyed = SaveEditScene(scene);

			CHECK(editor.GetHistory().Undo(editor) == 1);
			CHECK(SaveEditScene(scene) == instantiated);
			CHECK(editor.GetHistory().Undo(editor) == 1);
			CHECK(SaveEditScene(scene) == empty);
			const Result<size_t> redone = editor.GetHistory().Redo(editor, 2);
			REQUIRE(redone.has_value());
			CHECK(*redone == 2);
			CHECK(SaveEditScene(scene) == destroyed);
		}

		TEST_CASE("SceneEditCommand: the history entry records the revisions before and after the edit")
		{
			Test::EditorTestFixture fixture("SceneEditRevisions");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const uint64_t before = editor.GetRevision();
			{
				SceneEdit edit(editor, "Create");
				static_cast<void>(editor.GetScene().CreateEntity("Board"));
				REQUIRE(edit.Commit().has_value());
			}
			const std::vector<CommandHistoryEntry> entries = editor.GetHistory().GetEntries(1);
			REQUIRE(entries.size() == 1);
			CHECK(entries[0].RevisionBefore == before);
			CHECK(entries[0].RevisionAfter == editor.GetRevision());
			CHECK(entries[0].RevisionAfter > before);
		}

		TEST_CASE("SceneEditCommand: edits, undo and redo append entity and component events")
		{
			Test::EditorTestFixture fixture("SceneEditEvents");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			const EventLog& log = fixture.GetEngine().GetEventLog();
			uint64_t cursor = log.GetNextSeq();
			// The events since the last call, as "<Type> <name>" (the entity is checked separately).
			std::vector<UUID> ids;
			const auto takeEvents = [&log, &cursor, &ids]()
			{
				const EventReadResult read = log.Read(cursor);
				cursor = read.NextCursor;
				ids.clear();
				std::vector<std::string> described;
				for (const EngineEvent& event : read.Events)
				{
					described.push_back(std::format("{} {}", EngineEventTypeToString(event.Type), event.Name));
					ids.push_back(event.Id);
				}
				return described;
			};

			Entity board;
			{
				SceneEdit edit(editor, "Create Entity 'Board'");
				board = scene.CreateEntity("Board");
				REQUIRE(edit.Commit().has_value());
			}
			const UUID id = board.GetUUID();
			CHECK(takeEvents() == std::vector<std::string>{ "EntityCreated Board" });
			CHECK(ids == std::vector<UUID>{ id });
			{
				SceneEdit edit(editor, "Move 'Board'");
				REQUIRE(ComponentAccess::PatchComponentJson(board, "Transform", ParseEditJson(R"({"Translation":[1,2,3]})")).has_value());
				REQUIRE(edit.Commit().has_value());
			}
			CHECK(takeEvents() == std::vector<std::string>{ "ComponentChanged Transform" });
			CHECK(ids == std::vector<UUID>{ id });
			{
				SceneEdit edit(editor, "Rename 'Board'");
				board.SetName("Grid");
				REQUIRE(edit.Commit().has_value());
			}
			CHECK(takeEvents().empty()); // a name has no event type (§4.9)

			REQUIRE(editor.GetHistory().Undo(editor, 3) == 3);
			CHECK(takeEvents() == std::vector<std::string>{ "ComponentChanged Transform", "EntityDestroyed " });
			REQUIRE(editor.GetHistory().Redo(editor, 2).has_value());
			CHECK(takeEvents() == std::vector<std::string>{ "EntityCreated Board", "ComponentChanged Transform" });
			CHECK(ids == std::vector<UUID>{ id, id });
			{
				SceneEdit edit(editor, "Destroy 'Board'");
				scene.DestroyEntity(scene.FindEntityByID(id));
				REQUIRE(edit.Commit().has_value());
			}
			CHECK(takeEvents() == std::vector<std::string>{ "EntityDestroyed " });
		}
	}

}
