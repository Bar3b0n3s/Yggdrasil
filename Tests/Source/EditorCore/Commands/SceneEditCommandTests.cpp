#include "TestsPCH.h"

#include "EditorCore/Commands/SceneEditCommand.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
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
		TEST_CASE("SceneEditCommand: 10,000 random operations undo to byte-identical JSON and redo to the final state" * doctest::skip(true))
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

		TEST_CASE("SceneEditCommand: Execute, Undo and Execute again reproduce each operation exactly" * doctest::skip(true))
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

		TEST_CASE("SceneEditCommand: undo restores the sibling order after a multi-entity destroy" * doctest::skip(true))
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

		TEST_CASE("SceneEditCommand: merging keeps the first Before and the last After" * doctest::skip(true))
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

		TEST_CASE("SceneEditCommand: a constructed command reports its changes and starts applied" * doctest::skip(true))
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
	}

}
