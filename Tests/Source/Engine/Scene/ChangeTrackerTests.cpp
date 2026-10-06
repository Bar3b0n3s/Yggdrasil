#include "TestsPCH.h"

#include "Engine/Scene/ChangeTracker.h"

#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

// The snapshot contents (EntityChange::Before) come from the serializer (Scene::CaptureEntitySnapshot, stream C) and are
// checked in SceneSerializerTests.cpp; these cases cover what the tracker and the scene's hooks record themselves.

namespace Engine {

	static const EntityChange* FindChange(const std::vector<EntityChange>& changes, UUID id)
	{
		for (const EntityChange& change : changes)
		{
			if (change.EntityID == id)
				return &change;
		}
		return nullptr;
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ChangeTracker: records each touched entity once with its place before the edit" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity untouched = scene.CreateEntity("Untouched");
			const Entity moved = scene.CreateEntity("Moved");
			const Entity parent = scene.CreateEntity("Parent");

			ChangeTracker& tracker = scene.GetChangeTracker();
			tracker.Begin();
			CHECK(tracker.IsTracking());
			moved.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
			moved.AddComponent<RigidBodyComponent>();
			REQUIRE(scene.SetParent(moved, parent).has_value());
			const Entity created = scene.CreateEntity("Created");
			const std::vector<EntityChange> changes = tracker.End();
			CHECK_FALSE(tracker.IsTracking());

			CHECK(FindChange(changes, untouched.GetUUID()) == nullptr);

			const EntityChange* movedChange = FindChange(changes, moved.GetUUID());
			REQUIRE(movedChange != nullptr);
			CHECK(movedChange->Kind == EntityChangeKind::Modified);
			CHECK_FALSE(movedChange->ParentBefore.IsValid());
			CHECK(movedChange->SiblingIndexBefore == 1);
			CHECK(std::find(movedChange->Components.begin(), movedChange->Components.end(), "Transform") != movedChange->Components.end());
			CHECK(std::find(movedChange->Components.begin(), movedChange->Components.end(), "RigidBody") != movedChange->Components.end());

			const EntityChange* createdChange = FindChange(changes, created.GetUUID());
			REQUIRE(createdChange != nullptr);
			CHECK(createdChange->Kind == EntityChangeKind::Created);
			CHECK(createdChange->Before == nullptr);

			CHECK(std::is_sorted(changes.begin(), changes.end(), [](const EntityChange& a, const EntityChange& b)
			{
				return a.EntityID < b.EntityID;
			}));
		}

		TEST_CASE("ChangeTracker: an entity created and destroyed in one edit does not appear" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity doomed = scene.CreateEntity("Doomed");
			const UUID doomedID = doomed.GetUUID();

			scene.GetChangeTracker().Begin();
			const Entity temporary = scene.CreateEntity("Temporary");
			scene.DestroyEntity(temporary);
			scene.DestroyEntity(doomed);
			const std::vector<EntityChange> changes = scene.GetChangeTracker().End();

			REQUIRE(changes.size() == 1);
			CHECK(changes[0].EntityID == doomedID);
			CHECK(changes[0].Kind == EntityChangeKind::Destroyed);
		}

		TEST_CASE("ChangeTracker: sibling indices refer to the child lists at the start of the edit" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// UUID order is the reverse of sibling order, so End()'s sort by UUID cannot hide an index taken too late.
			static_cast<void>(scene.CreateEntityWithID(UUID(0x40), "A"));
			static_cast<void>(scene.CreateEntityWithID(UUID(0x30), "B"));
			const Entity c = scene.CreateEntityWithID(UUID(0x20), "C");
			const Entity d = scene.CreateEntityWithID(UUID(0x10), "D");

			// Delete a multi-selection, then group the survivors under a new entity, all in one edit.
			scene.GetChangeTracker().Begin();
			scene.DestroyEntity(scene.FindEntityByID(UUID(0x40)));
			scene.DestroyEntity(scene.FindEntityByID(UUID(0x30)));
			const Entity group = scene.CreateEntity("Group");
			REQUIRE(scene.SetParent(d, group).has_value());
			REQUIRE(scene.SetParent(c, group).has_value());
			const std::vector<EntityChange> changes = scene.GetChangeTracker().End();

			const std::pair<UUID, uint32_t> expected[] = {
				{ UUID(0x40), 0u }, // A
				{ UUID(0x30), 1u }, // B, although A was already gone when B was destroyed
				{ UUID(0x20), 2u }, // C
				{ UUID(0x10), 3u }, // D, although A and B were gone and C had moved when D moved
			};
			for (const auto& [id, index] : expected)
			{
				INFO(id.ToString());
				const EntityChange* change = FindChange(changes, id);
				REQUIRE(change != nullptr);
				CHECK_FALSE(change->ParentBefore.IsValid());
				CHECK(change->SiblingIndexBefore == index);
			}
		}

		TEST_CASE("ChangeTracker: runtime writes through GetComponent are not tracked" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntity("Entity");
			scene.GetChangeTracker().Begin();
			entity.GetComponent<TransformComponent>().Translation.x = 5.0f;
			CHECK(scene.GetChangeTracker().End().empty());
		}

		TEST_CASE("ChangeTracker: runtime-only components are neither tracked nor counted in the revision" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntity("Entity");
			const uint64_t revision = scene.GetRevision();

			scene.GetChangeTracker().Begin();
			entity.AddComponent<WorldTransformComponent>();
			entity.AddComponent<InterpolationResetTag>();
			entity.RemoveComponent<InterpolationResetTag>();
			CHECK(scene.GetChangeTracker().End().empty());
			CHECK(scene.GetRevision() == revision);

			// DisabledTag is not reflected but is the entity key "Active", so it is both.
			scene.GetChangeTracker().Begin();
			entity.SetActive(false);
			const std::vector<EntityChange> changes = scene.GetChangeTracker().End();
			REQUIRE(changes.size() == 1);
			CHECK(changes[0].Components == std::vector<std::string>{ "Active" });
			CHECK(scene.GetRevision() > revision);
		}
	}

}
