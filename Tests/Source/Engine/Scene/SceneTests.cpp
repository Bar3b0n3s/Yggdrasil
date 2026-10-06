#include "TestsPCH.h"

#include "Engine/Scene/Scene.h"

#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/DeathTest.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	static std::vector<std::string> CanonicalNames(Scene& scene)
	{
		std::vector<std::string> names;
		scene.ForEachCanonical([&](Entity entity)
		{
			names.push_back(entity.GetName());
		});
		return names;
	}

	static std::vector<UUID> CanonicalOrder(const Scene& scene)
	{
		return std::vector<UUID>(scene.GetCanonicalOrder().begin(), scene.GetCanonicalOrder().end());
	}

	ENGINE_DEATH_TEST("Scene/CreateEntityWithUsedIDAsserts")
	{
		Test::SceneTestFixture fixture;
		static_cast<void>(fixture.GetScene().CreateEntityWithID(UUID(0x1234), "First"));
		static_cast<void>(fixture.GetScene().CreateEntityWithID(UUID(0x1234), "Second"));
	}

	ENGINE_DEATH_TEST("Scene/CreateEntityWithZeroIDAsserts")
	{
		Test::SceneTestFixture fixture;
		static_cast<void>(fixture.GetScene().CreateEntityWithID(UUID(), "Zero"));
	}

	ENGINE_DEATH_TEST("Scene/UnfrozenRegistryAsserts")
	{
		const TypeRegistry registry;
		UUIDGenerator generator = UUIDGenerator::CreateDeterministic(1);
		SceneSpecification specification;
		specification.Registry = &registry;
		specification.IdGenerator = &generator;
		static_cast<void>(Scene::Create(specification));
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Scene: canonical order is independent of insertion history")
		{
			// Scene A: create in hierarchy order.
			Test::SceneTestFixture fixtureA;
			Scene& a = fixtureA.GetScene();
			const Entity gameA = a.CreateEntityWithID(UUID(0x10), "Game");
			static_cast<void>(a.CreateEntityWithID(UUID(0x11), "Board", gameA));
			static_cast<void>(a.CreateEntityWithID(UUID(0x12), "Hud", gameA));
			static_cast<void>(a.CreateEntityWithID(UUID(0x20), "Camera"));

			// Scene B: same hierarchy built in another order, with a create/destroy in between, so EnTT storage order differs.
			Test::SceneTestFixture fixtureB;
			Scene& b = fixtureB.GetScene();
			const Entity cameraB = b.CreateEntityWithID(UUID(0x20), "Camera");
			const Entity temporary = b.CreateEntity("Temporary");
			const Entity hudB = b.CreateEntityWithID(UUID(0x12), "Hud");
			const Entity boardB = b.CreateEntityWithID(UUID(0x11), "Board");
			b.DestroyEntity(temporary);
			const Entity gameB = b.CreateEntityWithID(UUID(0x10), "Game");
			REQUIRE(b.SetParent(gameB, Entity{}, 0u).has_value());
			REQUIRE(b.SetParent(boardB, gameB, 0u).has_value());
			REQUIRE(b.SetParent(hudB, gameB).has_value());
			CHECK(cameraB.GetSiblingIndex() == 1);

			const std::vector<std::string> expected = { "Game", "Board", "Hud", "Camera" };
			CHECK(CanonicalNames(a) == expected);
			CHECK(CanonicalNames(b) == expected);
			CHECK(CanonicalOrder(a) == CanonicalOrder(b));

			// The const overload visits the same entities through read-only handles.
			std::vector<UUID> constOrder;
			std::as_const(b).ForEachCanonical([&](ConstEntity entity)
			{
				constOrder.push_back(entity.GetUUID());
			});
			CHECK(constOrder == CanonicalOrder(a));
		}

		TEST_CASE("Scene: canonical order is depth-first and follows hierarchy changes")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity a = scene.CreateEntity("A");
			const Entity a1 = scene.CreateEntity("A1", a);
			static_cast<void>(scene.CreateEntity("A1x", a1));
			static_cast<void>(scene.CreateEntity("A2", a));
			const Entity b = scene.CreateEntity("B");
			CHECK(CanonicalNames(scene) == std::vector<std::string>{ "A", "A1", "A1x", "A2", "B" });

			REQUIRE(scene.SetParent(b, a1, 0u).has_value());
			CHECK(CanonicalNames(scene) == std::vector<std::string>{ "A", "A1", "B", "A1x", "A2" });

			scene.DestroyEntity(a1);
			CHECK(CanonicalNames(scene) == std::vector<std::string>{ "A", "A2" });
			CHECK(scene.GetCanonicalOrder().size() == scene.GetEntityCount());
		}

		TEST_CASE("Scene: ForEachCanonical visits a snapshot of the order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(scene.CreateEntity("First"));
			const Entity second = scene.CreateEntity("Second");
			static_cast<void>(scene.CreateEntity("Third"));

			std::vector<std::string> visited;
			scene.ForEachCanonical([&](Entity entity)
			{
				visited.push_back(entity.GetName());
				if (entity.GetName() == "First")
				{
					scene.DestroyEntity(second);                      // destroyed later in the order: skipped
					static_cast<void>(scene.CreateEntity("Created")); // created during the iteration: not visited
				}
			});
			CHECK(visited == std::vector<std::string>{ "First", "Third" });
			CHECK(CanonicalNames(scene) == std::vector<std::string>{ "First", "Third", "Created" });
		}

		TEST_CASE("Scene: reparenting rejects cycles")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity root = scene.CreateEntity("Root");
			const Entity child = scene.CreateEntity("Child", root);
			const Entity grandchild = scene.CreateEntity("Grandchild", child);
			const uint64_t revision = scene.GetRevision();

			const Status self = scene.SetParent(root, root);
			REQUIRE_FALSE(self.has_value());
			CHECK(self.error().GetCode() == ErrorCode::InvalidArgument);

			const Status cycle = scene.SetParent(root, grandchild);
			REQUIRE_FALSE(cycle.has_value());
			CHECK(cycle.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(cycle.error().GetLocation().Entity == root.GetUUID());

			CHECK(grandchild.GetParent() == child);
			CHECK(child.GetParent() == root);
			CHECK_FALSE(root.GetParent().IsValid());
			CHECK(scene.GetRevision() == revision); // a rejected move changes nothing
		}

		TEST_CASE("Scene: reparenting rejects entities that are invalid or of another scene")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntity("Entity");
			const Entity doomed = scene.CreateEntity("Doomed");
			scene.DestroyEntity(doomed);

			const Scope<Scene> other = fixture.CreateEmptyScene();
			const Entity foreign = other->CreateEntity("Foreign");
			const uint64_t revision = scene.GetRevision();

			for (const Status& status : { scene.SetParent(entity, doomed), scene.SetParent(entity, foreign), scene.SetParent(doomed, Entity{}),
					 scene.SetParent(foreign, Entity{}) })
			{
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK(scene.GetRevision() == revision);
			CHECK(other->GetRootEntities().size() == 1);
		}

		TEST_CASE("Scene: reparenting places the child at the requested sibling index")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity a = scene.CreateEntity("A", parent);
			const Entity b = scene.CreateEntity("B", parent);
			const Entity mover = scene.CreateEntity("Mover");

			REQUIRE(scene.SetParent(mover, parent, 1u).has_value());
			CHECK(mover.GetSiblingIndex() == 1);
			CHECK(b.GetSiblingIndex() == 2);
			CHECK(scene.GetRootEntities().size() == 1);

			REQUIRE(scene.SetParent(mover, parent, 99u).has_value()); // clamped to the end
			CHECK(mover.GetSiblingIndex() == 2);

			// Moving to the current place is not a mutation.
			const uint64_t revision = scene.GetRevision();
			REQUIRE(scene.SetParent(mover, parent).has_value());
			REQUIRE(scene.SetParent(a, parent, 0u).has_value());
			CHECK(scene.GetRevision() == revision);

			REQUIRE(scene.SetParent(a, Entity{}, 0u).has_value());
			CHECK(scene.GetRootEntities()[0] == a.GetUUID());
			CHECK_FALSE(a.GetParent().IsValid());
			CHECK(scene.GetRevision() > revision);
		}

		TEST_CASE("Scene: reparenting keeps the world transform unless asked not to")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
				transform.Scale = glm::vec3(2.0f);
			});
			const Entity child = scene.CreateEntity("Child");
			child.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(4.0f, 2.0f, 0.0f);
			});

			REQUIRE(scene.SetParent(child, parent).has_value());
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldPosition(child), glm::vec3(4.0f, 2.0f, 0.0f)));
			CHECK(Test::ApproxEqual(child.GetComponent<TransformComponent>().Translation, glm::vec3(-3.0f, 1.0f, 0.0f)));
			CHECK(Test::ApproxEqual(child.GetComponent<TransformComponent>().Scale, glm::vec3(0.5f)));

			REQUIRE(scene.SetParent(child, Entity{}, {}, false).has_value());
			CHECK(Test::ApproxEqual(child.GetComponent<TransformComponent>().Translation, glm::vec3(-3.0f, 1.0f, 0.0f)));

			// A world scale of 0.05 under a parent scaled 1000 would need a local scale of 5e-5, below the Transform.Scale
			// minimum that loading enforces: the move is rejected and nothing changes.
			const Entity giant = scene.CreateEntity("Giant");
			giant.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1000.0f);
			});
			const Entity speck = scene.CreateEntity("Speck");
			speck.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(0.05f);
			});
			const uint64_t revision = scene.GetRevision();
			const Status unrepresentable = scene.SetParent(speck, giant);
			REQUIRE_FALSE(unrepresentable.has_value());
			CHECK(unrepresentable.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(speck.GetParent().IsValid());
			CHECK(speck.GetComponent<TransformComponent>().Scale == glm::vec3(0.05f));
			CHECK(scene.GetRevision() == revision);
			REQUIRE(scene.SetParent(speck, giant, {}, false).has_value()); // keeping the local transform is always representable
		}

		TEST_CASE("Scene: entities are found by ID and by path")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity track = scene.CreateEntityWithID(UUID(0x100), "Track");
			std::vector<Entity> pieces;
			for (uint64_t i = 0; i < 4; ++i)
				pieces.push_back(scene.CreateEntityWithID(UUID(0x200 + i), "Piece", track));
			const Entity goal = scene.CreateEntityWithID(UUID(0x300), "Goal", track);

			CHECK(scene.FindEntityByID(UUID(0x300)) == goal);
			CHECK_FALSE(scene.FindEntityByID(UUID(0x999)).IsValid());
			CHECK_FALSE(scene.FindEntityByID(UUID()).IsValid());

			CHECK(scene.FindEntityByPath("/Track") == track);
			CHECK(scene.FindEntityByPath("/Track/Goal") == goal);
			CHECK(scene.FindEntityByPath("/Track/Piece[3]") == pieces[3]);
			CHECK(scene.FindEntityByPath("/Track[0]/Goal[0]") == goal);       // an index on a unique name is accepted
			CHECK_FALSE(scene.FindEntityByPath("/Track/Piece").IsValid());    // ambiguous
			CHECK_FALSE(scene.FindEntityByPath("/Track/Piece[4]").IsValid()); // out of range
			CHECK_FALSE(scene.FindEntityByPath("Track/Goal").IsValid());      // not absolute
			CHECK_FALSE(scene.FindEntityByPath("").IsValid());
			CHECK_FALSE(scene.FindEntityByPath("/Track/Goal/").IsValid()); // an empty last segment names a nameless child
			CHECK(scene.GetEntityPath(pieces[2]) == "/Track/Piece[2]");
			CHECK(scene.GetEntityPath(goal) == "/Track/Goal");
			CHECK(scene.GetEntityPath(track) == "/Track");

			const Result<Entity> ambiguous = scene.ResolveEntityPath("/Track/Piece");
			REQUIRE_FALSE(ambiguous.has_value());
			CHECK(ambiguous.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(ambiguous.error().GetIssues().size() == 4);
			for (size_t i = 0; i < 4; ++i)
			{
				const ErrorIssue& issue = ambiguous.error().GetIssues()[i];
				REQUIRE(issue.Suggestions.size() == 1);
				CHECK(issue.Suggestions[0] == std::format("/Track/Piece[{}]", i));
			}

			const Result<Entity> outOfRange = scene.ResolveEntityPath("/Track/Piece[7]");
			REQUIRE_FALSE(outOfRange.has_value());
			CHECK(outOfRange.error().GetCode() == ErrorCode::NotFound);

			const Result<Entity> malformed = scene.ResolveEntityPath("Track");
			REQUIRE_FALSE(malformed.has_value());
			CHECK(malformed.error().GetCode() == ErrorCode::InvalidArgument);

			// The const overloads find the same entities and hand out read-only handles.
			const Scene& constScene = scene;
			static_assert(std::is_same_v<decltype(constScene.FindEntityByID(UUID())), ConstEntity>);
			static_assert(std::is_same_v<decltype(constScene.FindEntityByPath("/")), ConstEntity>);
			static_assert(std::is_same_v<decltype(constScene.ResolveEntityPath("/")), Result<ConstEntity>>);
			CHECK(constScene.FindEntityByID(UUID(0x300)) == ConstEntity(goal));
			CHECK(constScene.FindEntityByPath("/Track/Piece[3]").GetUUID() == UUID(0x203));
			const Result<ConstEntity> constGoal = constScene.ResolveEntityPath("/Track/Goal");
			REQUIRE(constGoal.has_value());
			CHECK(*constGoal == ConstEntity(goal));
		}

		TEST_CASE("Scene: unmatched path segments fail with suggestions")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity track = scene.CreateEntityWithID(UUID(0x100), "Track");
			static_cast<void>(scene.CreateEntityWithID(UUID(0x300), "Goal", track));
			static_cast<void>(scene.CreateEntityWithID(UUID(0x301), "Start", track));

			const Result<Entity> missing = scene.ResolveEntityPath("/Track/Gaol");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetHint() == "did you mean 'Goal'?");
			REQUIRE(missing.error().GetIssues().size() == 1);
			CHECK(missing.error().GetIssues()[0].Suggestions == std::vector<std::string>{ "Goal" });

			const Result<Entity> missingRoot = scene.ResolveEntityPath("/Trak/Goal");
			REQUIRE_FALSE(missingRoot.has_value());
			CHECK(missingRoot.error().GetCode() == ErrorCode::NotFound);
			CHECK(missingRoot.error().GetHint() == "did you mean 'Track'?");
		}

		TEST_CASE("Scene: entity paths escape names that contain path syntax")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity slash = scene.CreateEntityWithID(UUID(0x10), "A/B");
			const Entity bracket = scene.CreateEntityWithID(UUID(0x11), "Piece[3]");
			const Entity backslash = scene.CreateEntityWithID(UUID(0x12), "C:\\Temp");
			const Entity first = scene.CreateEntityWithID(UUID(0x13), "Piece[3]", bracket);
			static_cast<void>(scene.CreateEntityWithID(UUID(0x14), "Piece[3]", bracket));
			const Entity nameless = scene.CreateEntityWithID(UUID(0x15), "", slash);
			const Entity spaced = scene.CreateEntityWithID(UUID(0x16), " Spaced  name ", nameless);

			CHECK(scene.GetEntityPath(slash) == "/A\\/B");
			CHECK(scene.GetEntityPath(bracket) == "/Piece\\[3\\]");
			CHECK(scene.GetEntityPath(backslash) == "/C:\\\\Temp");
			CHECK(scene.GetEntityPath(first) == "/Piece\\[3\\]/Piece\\[3\\][0]");
			CHECK(scene.GetEntityPath(nameless) == "/A\\/B/");
			CHECK(scene.GetEntityPath(spaced) == "/A\\/B// Spaced  name ");

			// Every generated path resolves back to its entity.
			for (const Entity entity : { slash, bracket, backslash, first, nameless, spaced })
				CHECK(scene.FindEntityByPath(scene.GetEntityPath(entity)) == entity);

			CHECK_FALSE(scene.FindEntityByPath("/A/B").IsValid());              // "A" has no child "B"
			CHECK_FALSE(scene.FindEntityByPath("/Piece[3]").IsValid());         // index 3 of the siblings named "Piece"
			const Result<Entity> malformed = scene.ResolveEntityPath("/A\\xB"); // a backslash before an ordinary character
			REQUIRE_FALSE(malformed.has_value());
			CHECK(malformed.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(scene.FindEntityByPath("/Piece\\[3\\]/Piece\\[3\\][01]").IsValid()); // leading zero

			const std::array<std::string_view, 8> malformedPaths = {
				"/A\\",
				"/Piece]",
				"/Piece[",
				"/Piece[]",
				"/Piece[x]",
				"/Piece[-1]",
				"/Piece[1]x",
				"/Piece[1][2]",
			};
			for (const std::string_view bad : malformedPaths)
			{
				INFO(std::string(bad));
				const Result<Entity> result = scene.ResolveEntityPath(bad);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK_FALSE(scene.FindEntityByPath("/Piece\\[3\\][99999999999999999999999]").IsValid()); // saturated index
		}

		TEST_CASE("Scene: destroying an entity destroys its subtree")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity root = scene.CreateEntity("Root");
			const Entity child = scene.CreateEntity("Child", root);
			const Entity sibling = scene.CreateEntity("Sibling");
			const Entity grandchild = scene.CreateEntity("Grandchild", child);
			const UUID grandchildID = grandchild.GetUUID();
			REQUIRE(scene.GetEntityCount() == 4);

			scene.DestroyEntity(root);
			CHECK(scene.GetEntityCount() == 1);
			CHECK_FALSE(root.IsValid());
			CHECK_FALSE(child.IsValid());
			CHECK_FALSE(grandchild.IsValid());
			CHECK_FALSE(scene.FindEntityByID(grandchildID).IsValid());
			CHECK(sibling.IsValid());
			CHECK(scene.GetRootEntities().size() == 1);
			CHECK(scene.GetCanonicalOrder().size() == 1);

			// Destroying a child removes it from its parent's list.
			const Entity parent = scene.CreateEntity("Parent");
			const Entity a = scene.CreateEntity("A", parent);
			const Entity b = scene.CreateEntity("B", parent);
			scene.DestroyEntity(a);
			REQUIRE(parent.GetChildren().size() == 1);
			CHECK(parent.GetChildren()[0] == b.GetUUID());
			CHECK(b.GetSiblingIndex() == 0);
		}

		TEST_CASE("Scene: runtime scenes defer destruction to the flush point")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			const Entity ball = scene.CreateEntity("Ball");
			const Entity trail = scene.CreateEntity("Trail", ball);
			const Entity other = scene.CreateEntity("Other");
			const UUID id = ball.GetUUID();

			scene.DestroyEntity(ball);
			CHECK_FALSE(ball.IsValid());
			CHECK_FALSE(trail.IsValid());
			CHECK_FALSE(scene.FindEntityByID(id).IsValid());
			CHECK_FALSE(scene.FindEntityByPath("/Ball").IsValid());
			CHECK(scene.GetEntityCount() == 1);
			CHECK(scene.GetCanonicalOrder().size() == 1);
			CHECK(scene.GetRegistry().valid(ball.GetHandle())); // still alive until the flush
			CHECK(scene.GetRegistry().valid(trail.GetHandle()));

			scene.FlushPendingDestroys();
			CHECK_FALSE(scene.GetRegistry().valid(ball.GetHandle()));
			CHECK_FALSE(scene.GetRegistry().valid(trail.GetHandle()));
			CHECK(other.IsValid());

			// Edit scenes have nothing to flush.
			Test::SceneTestFixture editFixture;
			const Entity kept = editFixture.GetScene().CreateEntity("Kept");
			editFixture.GetScene().FlushPendingDestroys();
			CHECK(kept.IsValid());
		}

		TEST_CASE("Scene: structural changes increment the revision")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			uint64_t revision = scene.GetRevision();
			const Entity entity = scene.CreateEntity("Entity");
			CHECK(scene.GetRevision() > revision);

			const Entity parent = scene.CreateEntity("Parent");
			revision = scene.GetRevision();
			REQUIRE(scene.SetParent(entity, parent).has_value());
			CHECK(scene.GetRevision() > revision);

			revision = scene.GetRevision();
			entity.SetActive(false);
			CHECK(scene.GetRevision() > revision);

			revision = scene.GetRevision();
			entity.SetActive(false); // no change, no mutation
			CHECK(scene.GetRevision() == revision);

			revision = scene.GetRevision();
			scene.SetName("Renamed");
			scene.SetSeed(42);
			CHECK(scene.GetRevision() == revision + 2);

			revision = scene.GetRevision();
			scene.DestroyEntity(entity);
			CHECK(scene.GetRevision() > revision);

			revision = scene.GetRevision();
			scene.Clear();
			CHECK(scene.GetRevision() > revision);
		}

		// Needs streams A and D: component patches are counted when the registry knows the component.
		TEST_CASE("Scene: every mutation increments the revision")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			uint64_t revision = scene.GetRevision();
			const Entity entity = scene.CreateEntity("Entity");
			CHECK(scene.GetRevision() > revision);

			revision = scene.GetRevision();
			entity.SetName("Renamed");
			CHECK(scene.GetRevision() > revision);

			revision = scene.GetRevision();
			entity.SetName("Renamed"); // no change, no mutation
			CHECK(scene.GetRevision() == revision);

			revision = scene.GetRevision();
			entity.AddTag("Player");
			CHECK(scene.GetRevision() > revision);

			revision = scene.GetRevision();
			entity.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x = 1.0f;
			});
			CHECK(scene.GetRevision() > revision);

			// Runtime-only components are maintained by systems and never count (TransformSystem::Update on an edit scene).
			revision = scene.GetRevision();
			TransformSystem::Update(scene);
			CHECK(entity.HasComponent<WorldTransformComponent>());
			CHECK(scene.GetRevision() == revision);
		}

		TEST_CASE("Scene: CreateEntity draws unused IDs from the scene's generator")
		{
			Test::SceneTestFixture fixture(9);
			Scene& scene = fixture.GetScene();

			// The same generator seed gives the same IDs (§4.8).
			UUIDGenerator reference = UUIDGenerator::CreateDeterministic(9);
			const UUID first = reference.Next();
			const UUID second = reference.Next();
			const UUID third = reference.Next();
			CHECK(scene.CreateEntity("A").GetUUID() == first);

			// An ID that is already used is skipped: the generator is drawn again.
			static_cast<void>(scene.CreateEntityWithID(third, "Taken"));
			CHECK(scene.CreateEntity("B").GetUUID() == second);
			const Entity c = scene.CreateEntity("C");
			CHECK(c.GetUUID() != third);
			CHECK(c.GetUUID() == reference.Next());
			CHECK(scene.GetEntityCount() == 4);
		}

		TEST_CASE("Scene: Clear destroys every entity and keeps name and seed")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			scene.SetSeed(7);
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			const Entity pending = scene.CreateEntity("Pending");
			const UUID parentID = parent.GetUUID();
			scene.DestroyEntity(pending);

			scene.Clear();
			CHECK(scene.GetEntityCount() == 0);
			CHECK(scene.GetRootEntities().empty());
			CHECK(scene.GetCanonicalOrder().empty());
			CHECK_FALSE(parent.IsValid());
			CHECK_FALSE(child.IsValid());
			CHECK_FALSE(scene.GetRegistry().valid(pending.GetHandle()));
			CHECK(scene.GetName() == "Test");
			CHECK(scene.GetSeed() == 7u);

			// The scene is usable afterwards, IDs included.
			const Entity again = scene.CreateEntityWithID(parentID, "Again");
			CHECK(again.IsValid());
			CHECK(scene.FindEntityByPath("/Again") == again);
		}

		TEST_CASE("Scene: the interpolation alpha is stored")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			CHECK(scene.GetInterpolationAlpha() == 1.0f);
			scene.SetInterpolationAlpha(0.25f);
			CHECK(scene.GetInterpolationAlpha() == 0.25f);
		}

		TEST_CASE("Scene: creating entities with used or zero IDs asserts")
		{
			ENGINE_CHECK_DEATH("Scene/CreateEntityWithUsedIDAsserts", "is already used");
			ENGINE_CHECK_DEATH("Scene/CreateEntityWithZeroIDAsserts", "needs a valid ID");
		}

		TEST_CASE("Scene: construction asserts a frozen registry")
		{
			ENGINE_CHECK_DEATH("Scene/UnfrozenRegistryAsserts", "frozen type registry");
		}
	}

}
