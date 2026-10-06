#include "TestsPCH.h"

#include "Engine/Scene/Scene.h"

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

	TEST_SUITE("Scene")
	{
		TEST_CASE("Scene: canonical order is independent of insertion history" * doctest::skip(true))
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
			CHECK(std::vector<UUID>(a.GetCanonicalOrder().begin(), a.GetCanonicalOrder().end())
				== std::vector<UUID>(b.GetCanonicalOrder().begin(), b.GetCanonicalOrder().end()));
		}

		TEST_CASE("Scene: reparenting rejects cycles" * doctest::skip(true))
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

			CHECK(grandchild.GetParent() == child);
			CHECK(child.GetParent() == root);
			CHECK_FALSE(root.GetParent().IsValid());
			CHECK(scene.GetRevision() == revision); // a rejected move changes nothing
		}

		TEST_CASE("Scene: reparenting keeps the world transform unless asked not to" * doctest::skip(true))
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

		TEST_CASE("Scene: entities are found by ID and by path" * doctest::skip(true))
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

			CHECK(scene.FindEntityByPath("/Track/Goal") == goal);
			CHECK(scene.FindEntityByPath("/Track/Piece[3]") == pieces[3]);
			CHECK_FALSE(scene.FindEntityByPath("/Track/Piece").IsValid()); // ambiguous
			CHECK_FALSE(scene.FindEntityByPath("Track/Goal").IsValid());   // not absolute
			CHECK(scene.GetEntityPath(pieces[2]) == "/Track/Piece[2]");
			CHECK(scene.GetEntityPath(goal) == "/Track/Goal");

			const Result<Entity> ambiguous = scene.ResolveEntityPath("/Track/Piece");
			REQUIRE_FALSE(ambiguous.has_value());
			CHECK(ambiguous.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(ambiguous.error().GetIssues().size() == 4);

			const Result<Entity> missing = scene.ResolveEntityPath("/Track/Gaol");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetHint().find("Goal") != std::string::npos);

			// The const overloads find the same entities and hand out read-only handles.
			const Scene& constScene = scene;
			static_assert(std::is_same_v<decltype(constScene.FindEntityByID(UUID())), ConstEntity>);
			static_assert(std::is_same_v<decltype(constScene.ResolveEntityPath("/")), Result<ConstEntity>>);
			CHECK(constScene.FindEntityByID(UUID(0x300)) == ConstEntity(goal));
			CHECK(constScene.FindEntityByPath("/Track/Piece[3]").GetUUID() == UUID(0x203));
		}

		TEST_CASE("Scene: entity paths escape names that contain path syntax" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity slash = scene.CreateEntityWithID(UUID(0x10), "A/B");
			const Entity bracket = scene.CreateEntityWithID(UUID(0x11), "Piece[3]");
			const Entity backslash = scene.CreateEntityWithID(UUID(0x12), "C:\\Temp");
			const Entity first = scene.CreateEntityWithID(UUID(0x13), "Piece[3]", bracket);
			static_cast<void>(scene.CreateEntityWithID(UUID(0x14), "Piece[3]", bracket));

			CHECK(scene.GetEntityPath(slash) == "/A\\/B");
			CHECK(scene.GetEntityPath(bracket) == "/Piece\\[3\\]");
			CHECK(scene.GetEntityPath(backslash) == "/C:\\\\Temp");
			CHECK(scene.GetEntityPath(first) == "/Piece\\[3\\]/Piece\\[3\\][0]");

			// Every generated path resolves back to its entity.
			for (const Entity entity : { slash, bracket, backslash, first })
				CHECK(scene.FindEntityByPath(scene.GetEntityPath(entity)) == entity);

			CHECK_FALSE(scene.FindEntityByPath("/A/B").IsValid());              // "A" has no child "B"
			CHECK_FALSE(scene.FindEntityByPath("/Piece[3]").IsValid());         // index 3 of the siblings named "Piece"
			const Result<Entity> malformed = scene.ResolveEntityPath("/A\\xB"); // a backslash before an ordinary character
			REQUIRE_FALSE(malformed.has_value());
			CHECK(malformed.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(scene.FindEntityByPath("/Piece\\[3\\]/Piece\\[3\\][01]").IsValid()); // leading zero
		}

		TEST_CASE("Scene: destroying an entity destroys its subtree" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity root = scene.CreateEntity("Root");
			const Entity child = scene.CreateEntity("Child", root);
			const Entity sibling = scene.CreateEntity("Sibling");
			static_cast<void>(scene.CreateEntity("Grandchild", child));
			REQUIRE(scene.GetEntityCount() == 4);

			scene.DestroyEntity(root);
			CHECK(scene.GetEntityCount() == 1);
			CHECK_FALSE(child.IsValid());
			CHECK(sibling.IsValid());
			CHECK(scene.GetRootEntities().size() == 1);
		}

		TEST_CASE("Scene: runtime scenes defer destruction to the flush point" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			const Entity ball = scene.CreateEntity("Ball");
			const UUID id = ball.GetUUID();

			scene.DestroyEntity(ball);
			CHECK_FALSE(ball.IsValid());
			CHECK_FALSE(scene.FindEntityByID(id).IsValid());
			CHECK(scene.GetEntityCount() == 0);
			CHECK(scene.GetRegistry().valid(ball.GetHandle())); // still alive until the flush

			scene.FlushPendingDestroys();
			CHECK_FALSE(scene.GetRegistry().valid(ball.GetHandle()));
		}

		TEST_CASE("Scene: every mutation increments the revision" * doctest::skip(true))
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
			entity.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x = 1.0f;
			});
			CHECK(scene.GetRevision() > revision);
		}

		TEST_CASE("Scene: creating entities with used or zero IDs asserts" * doctest::skip(true))
		{
			ENGINE_CHECK_DEATH("Scene/CreateEntityWithUsedIDAsserts", "is already used");
			ENGINE_CHECK_DEATH("Scene/CreateEntityWithZeroIDAsserts", "needs a valid ID");
		}
	}

}
