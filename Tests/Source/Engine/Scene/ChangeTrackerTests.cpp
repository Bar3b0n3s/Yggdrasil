#include "TestsPCH.h"

#include "Engine/Scene/ChangeTracker.h"

#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/DeathTest.h"
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

	static bool ContainsComponent(const EntityChange& change, std::string_view component)
	{
		return std::find(change.Components.begin(), change.Components.end(), component) != change.Components.end();
	}

	ENGINE_DEATH_TEST("Scene/ChangeTrackerBeginTwiceAsserts")
	{
		ChangeTracker tracker;
		tracker.Begin();
		tracker.Begin();
	}

	ENGINE_DEATH_TEST("Scene/ChangeTrackerEndWithoutBeginAsserts")
	{
		ChangeTracker tracker;
		static_cast<void>(tracker.End());
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ChangeTracker: records snapshots, components and lifetimes per entity")
		{
			ChangeTracker tracker;
			CHECK_FALSE(tracker.IsTracking());
			CHECK_FALSE(tracker.NeedsSnapshot(UUID(1))); // idle: nothing to record
			CHECK_FALSE(tracker.NeedsChildOrder(UUID()));

			tracker.Begin();
			REQUIRE(tracker.IsTracking());
			CHECK(tracker.NeedsSnapshot(UUID(0x30)));

			const Ref<const Json> before = CreateRef<const Json>(Json::object());
			tracker.RecordSnapshot(UUID(0x30), before, UUID(0x99), 4);
			CHECK_FALSE(tracker.NeedsSnapshot(UUID(0x30)));
			tracker.RecordComponent(UUID(0x30), "Transform");
			tracker.RecordComponent(UUID(0x30), "RigidBody");
			tracker.RecordComponent(UUID(0x30), "Transform");

			tracker.RecordCreated(UUID(0x20));
			CHECK_FALSE(tracker.NeedsSnapshot(UUID(0x20)));
			tracker.RecordComponent(UUID(0x20), "Camera");

			tracker.RecordSnapshot(UUID(0x10), nullptr, UUID(), 2);
			tracker.RecordDestroyed(UUID(0x10));

			// Destroyed and created again under the same ID: it existed before and after.
			tracker.RecordSnapshot(UUID(0x40), before, UUID(), 0);
			tracker.RecordDestroyed(UUID(0x40));
			tracker.RecordCreated(UUID(0x40));

			const std::vector<EntityChange> changes = tracker.End();
			CHECK_FALSE(tracker.IsTracking());
			REQUIRE(changes.size() == 4);
			CHECK(changes[0].EntityID == UUID(0x10)); // sorted by UUID
			CHECK(changes[0].Kind == EntityChangeKind::Destroyed);
			CHECK(changes[0].SiblingIndexBefore == 2);
			CHECK(changes[1].EntityID == UUID(0x20));
			CHECK(changes[1].Kind == EntityChangeKind::Created);
			CHECK(changes[1].Before == nullptr);
			CHECK_FALSE(changes[1].ParentBefore.IsValid());
			CHECK(changes[1].Components == std::vector<std::string>{ "Camera" });
			CHECK(changes[2].EntityID == UUID(0x30));
			CHECK(changes[2].Kind == EntityChangeKind::Modified);
			CHECK(changes[2].Before == before);
			CHECK(changes[2].ParentBefore == UUID(0x99));
			CHECK(changes[2].SiblingIndexBefore == 4);
			CHECK(changes[2].Components == std::vector<std::string>{ "Transform", "RigidBody" });
			CHECK(changes[3].EntityID == UUID(0x40));
			CHECK(changes[3].Kind == EntityChangeKind::Modified);
			CHECK(changes[3].Before == before);

			// The next edit starts empty.
			tracker.Begin();
			CHECK(tracker.NeedsSnapshot(UUID(0x30)));
			CHECK(tracker.End().empty());
		}

		TEST_CASE("ChangeTracker: recorded child orders define the sibling indices")
		{
			ChangeTracker tracker;
			tracker.Begin();
			const std::vector<UUID> rootsAtBegin = { UUID(0x1), UUID(0x2), UUID(0x3) };
			REQUIRE(tracker.NeedsChildOrder(UUID()));
			tracker.RecordChildOrder(UUID(), rootsAtBegin);
			CHECK_FALSE(tracker.NeedsChildOrder(UUID()));
			CHECK(tracker.NeedsChildOrder(UUID(0x1)));

			// Snapshotted after its earlier siblings were removed: the index of the recorded list wins.
			tracker.RecordSnapshot(UUID(0x3), nullptr, UUID(), 0);
			// A parent whose list was never recorded keeps the index given with the snapshot.
			tracker.RecordSnapshot(UUID(0x7), nullptr, UUID(0x1), 5);

			const std::vector<EntityChange> changes = tracker.End();
			REQUIRE(changes.size() == 2);
			CHECK(changes[0].EntityID == UUID(0x3));
			CHECK(changes[0].SiblingIndexBefore == 2);
			CHECK(changes[1].EntityID == UUID(0x7));
			CHECK(changes[1].SiblingIndexBefore == 5);
		}

		// Needs streams A and D: component names come from the registry.
		TEST_CASE("ChangeTracker: records each touched entity once with its place before the edit")
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
			created.SetName("Renamed");
			const std::vector<EntityChange> changes = tracker.End();
			CHECK_FALSE(tracker.IsTracking());

			CHECK(FindChange(changes, untouched.GetUUID()) == nullptr);
			CHECK(FindChange(changes, parent.GetUUID()) == nullptr); // its child list is recorded, not the entity

			const EntityChange* movedChange = FindChange(changes, moved.GetUUID());
			REQUIRE(movedChange != nullptr);
			CHECK(movedChange->Kind == EntityChangeKind::Modified);
			CHECK_FALSE(movedChange->ParentBefore.IsValid());
			CHECK(movedChange->SiblingIndexBefore == 1);
			CHECK(movedChange->Components == std::vector<std::string>{ "Transform", "RigidBody", "Relationship" });

			const EntityChange* createdChange = FindChange(changes, created.GetUUID());
			REQUIRE(createdChange != nullptr);
			CHECK(createdChange->Kind == EntityChangeKind::Created);
			CHECK(createdChange->Before == nullptr);
			CHECK(createdChange->Components == std::vector<std::string>{ "Name" });

			CHECK(std::is_sorted(changes.begin(), changes.end(), [](const EntityChange& a, const EntityChange& b)
			{
				return a.EntityID < b.EntityID;
			}));
		}

		TEST_CASE("ChangeTracker: an entity created and destroyed in one edit does not appear")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity doomed = scene.CreateEntity("Doomed");
			const UUID doomedID = doomed.GetUUID();

			scene.GetChangeTracker().Begin();
			const Entity temporary = scene.CreateEntity("Temporary");
			static_cast<void>(scene.CreateEntity("TemporaryChild", temporary));
			scene.DestroyEntity(temporary);
			scene.DestroyEntity(doomed);
			const std::vector<EntityChange> changes = scene.GetChangeTracker().End();

			REQUIRE(changes.size() == 1);
			CHECK(changes[0].EntityID == doomedID);
			CHECK(changes[0].Kind == EntityChangeKind::Destroyed);
			CHECK(changes[0].SiblingIndexBefore == 0);
		}

		TEST_CASE("ChangeTracker: a destroyed subtree records every member in its place")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(scene.CreateEntity("First"));
			const Entity root = scene.CreateEntity("Root");
			const Entity a = scene.CreateEntity("A", root);
			const Entity b = scene.CreateEntity("B", root);
			const Entity b1 = scene.CreateEntity("B1", b);
			const std::array<UUID, 4> ids = { root.GetUUID(), a.GetUUID(), b.GetUUID(), b1.GetUUID() };

			scene.GetChangeTracker().Begin();
			scene.DestroyEntity(root);
			const std::vector<EntityChange> changes = scene.GetChangeTracker().End();

			REQUIRE(changes.size() == 4);
			const std::array<std::pair<UUID, uint32_t>, 4> expected = {
				std::pair{ UUID(), 1u }, // Root, after First
				std::pair{ ids[0], 0u }, // A
				std::pair{ ids[0], 1u }, // B
				std::pair{ ids[2], 0u }, // B1
			};
			for (size_t i = 0; i < ids.size(); ++i)
			{
				INFO(ids[i].ToString());
				const EntityChange* change = FindChange(changes, ids[i]);
				REQUIRE(change != nullptr);
				CHECK(change->Kind == EntityChangeKind::Destroyed);
				CHECK(change->ParentBefore == expected[i].first);
				CHECK(change->SiblingIndexBefore == expected[i].second);
			}
		}

		TEST_CASE("ChangeTracker: sibling indices refer to the child lists at the start of the edit")
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

			const EntityChange* groupChange = FindChange(changes, group.GetUUID());
			REQUIRE(groupChange != nullptr);
			CHECK(groupChange->Kind == EntityChangeKind::Created);
			CHECK(changes.size() == 5);
		}

		TEST_CASE("ChangeTracker: runtime writes through GetComponent are not tracked")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntity("Entity");
			scene.GetChangeTracker().Begin();
			entity.GetComponent<TransformComponent>().Translation.x = 5.0f;
			CHECK(scene.GetChangeTracker().End().empty());
		}

		TEST_CASE("ChangeTracker: runtime-only components are neither tracked nor counted in the revision")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntity("Entity");
			const uint64_t revision = scene.GetRevision();

			scene.GetChangeTracker().Begin();
			entity.AddComponent<WorldTransformComponent>();
			entity.Patch<WorldTransformComponent>([](WorldTransformComponent& world)
			{
				world.Matrix[3].x = 1.0f;
			});
			entity.AddComponent<InterpolationResetTag>();
			entity.RemoveComponent<InterpolationResetTag>();
			entity.RemoveComponent<WorldTransformComponent>();
			CHECK(scene.GetChangeTracker().End().empty());
			CHECK(scene.GetRevision() == revision);

			// DisabledTag is not reflected but is the entity key "Active", so it is both.
			scene.GetChangeTracker().Begin();
			entity.SetActive(false);
			entity.SetActive(true);
			const std::vector<EntityChange> changes = scene.GetChangeTracker().End();
			REQUIRE(changes.size() == 1);
			CHECK(changes[0].Kind == EntityChangeKind::Modified);
			CHECK(changes[0].Components == std::vector<std::string>{ "Active" });
			CHECK(scene.GetRevision() > revision);
			CHECK(ContainsComponent(changes[0], "Active"));
		}

		TEST_CASE("ChangeTracker: Begin twice and End without Begin assert")
		{
			ENGINE_CHECK_DEATH("Scene/ChangeTrackerBeginTwiceAsserts", "already being tracked");
			ENGINE_CHECK_DEATH("Scene/ChangeTrackerEndWithoutBeginAsserts", "without a tracked edit");
		}
	}

}
