#include "TestsPCH.h"

#include "Engine/Scene/Entity.h"

#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/NameComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TagsComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Support/DeathTest.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	ENGINE_DEATH_TEST("Scene/InvalidEntityAccessAsserts")
	{
		const Entity entity;
		static_cast<void>(entity.GetUUID());
	}

	ENGINE_DEATH_TEST("Scene/DestroyedEntityAccessAsserts")
	{
		Test::SceneTestFixture fixture;
		const Entity entity = fixture.GetScene().CreateEntity("Doomed");
		fixture.GetScene().DestroyEntity(entity);
		static_cast<void>(entity.GetComponent<TransformComponent>());
	}

	ENGINE_DEATH_TEST("Scene/PendingDestroyEntityAccessAsserts")
	{
		Test::SceneTestFixture fixture(1, true);
		const Entity entity = fixture.GetScene().CreateEntity("Doomed");
		fixture.GetScene().DestroyEntity(entity);
		static_cast<void>(entity.GetName());
	}

	ENGINE_DEATH_TEST("Scene/RemovingRequiredComponentAsserts")
	{
		Test::SceneTestFixture fixture;
		const Entity entity = fixture.GetScene().CreateEntity("Entity");
		entity.RemoveComponent<TransformComponent>();
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Entity: a new entity has the required components")
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Ball");
			REQUIRE(entity.IsValid());
			CHECK(static_cast<bool>(entity));
			CHECK(entity.HasComponent<IDComponent>());
			CHECK(entity.HasComponent<NameComponent>());
			CHECK(entity.HasComponent<TagsComponent>());
			CHECK(entity.HasComponent<RelationshipComponent>());
			CHECK(entity.HasComponent<TransformComponent>());
			CHECK_FALSE(entity.HasComponent<DisabledTag>());
			CHECK(entity.GetUUID().IsValid());
			CHECK(entity.GetComponent<IDComponent>().ID == entity.GetUUID());
			CHECK(entity.GetName() == "Ball");
			CHECK(entity.IsActive());
			CHECK(entity.GetTags().empty());
			CHECK(entity.GetChildren().empty());
			CHECK_FALSE(entity.GetParent().IsValid());
			CHECK(entity.GetScene() == &fixture.GetScene());

			const TransformComponent& transform = entity.GetComponent<TransformComponent>();
			CHECK(transform.Translation == glm::vec3(0.0f));
			CHECK(transform.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(transform.Scale == glm::vec3(1.0f));
			CHECK(fixture.GetScene().CreateEntity().GetName() == "Entity");
		}

		TEST_CASE("Entity: components are added, patched and removed")
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Box");
			RigidBodyComponent& body = entity.AddComponent<RigidBodyComponent>();
			CHECK(body.Mass == 1.0f);
			CHECK(entity.HasComponent<RigidBodyComponent>());

			entity.Patch<RigidBodyComponent>([](RigidBodyComponent& component)
			{
				component.Mass = 5.0f;
			});
			CHECK(entity.GetComponent<RigidBodyComponent>().Mass == 5.0f);
			REQUIRE(entity.TryGetComponent<RigidBodyComponent>() != nullptr);

			entity.RemoveComponent<RigidBodyComponent>();
			CHECK_FALSE(entity.HasComponent<RigidBodyComponent>());
			CHECK(entity.TryGetComponent<RigidBodyComponent>() == nullptr);
		}

		TEST_CASE("Entity: the read-only handle sees the same entity")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			child.AddTag("Tagged");
			child.SetActive(false);

			const ConstEntity readOnly = child;
			REQUIRE(readOnly.IsValid());
			CHECK(readOnly.GetHandle() == child.GetHandle());
			CHECK(readOnly.GetScene() == &scene);
			CHECK(readOnly.GetUUID() == child.GetUUID());
			CHECK(readOnly.GetName() == "Child");
			CHECK(readOnly.GetParent() == ConstEntity(parent));
			CHECK(ConstEntity(parent).GetChildren().size() == 1);
			CHECK(readOnly.GetSiblingIndex() == 0);
			CHECK_FALSE(readOnly.IsActiveSelf());
			CHECK_FALSE(readOnly.IsActive());
			CHECK(readOnly.HasTag("Tagged"));
			CHECK(readOnly.GetTags().size() == 1);
			CHECK(readOnly.HasComponent<TransformComponent>());
			CHECK(readOnly.TryGetComponent<RigidBodyComponent>() == nullptr);
			CHECK(&readOnly.GetComponent<NameComponent>() == &child.GetComponent<NameComponent>());
			CHECK_FALSE(ConstEntity().IsValid());
			CHECK_FALSE(static_cast<bool>(ConstEntity()));
		}

		TEST_CASE("Entity: names are set")
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Before");
			entity.SetName("After");
			CHECK(entity.GetName() == "After");
			CHECK(entity.GetComponent<NameComponent>().Name == "After");
			entity.SetName("");
			CHECK(entity.GetName().empty());
		}

		TEST_CASE("Entity: the active state follows the hierarchy")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			const Entity grandchild = scene.CreateEntity("Grandchild", child);

			parent.SetActive(false);
			CHECK(parent.HasComponent<DisabledTag>());
			CHECK_FALSE(parent.IsActiveSelf());
			CHECK(child.IsActiveSelf());
			CHECK_FALSE(child.IsActive());
			CHECK_FALSE(grandchild.IsActive());

			parent.SetActive(false); // already inactive: nothing changes
			CHECK(parent.HasComponent<DisabledTag>());

			parent.SetActive(true);
			CHECK(child.IsActive());
			CHECK(grandchild.IsActive());
			CHECK_FALSE(parent.HasComponent<DisabledTag>());

			child.SetActive(false);
			CHECK(parent.IsActive());
			CHECK_FALSE(grandchild.IsActive());
			CHECK(grandchild.IsActiveSelf());
		}

		TEST_CASE("Entity: HierarchyDisabledTag caches the effective active state")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);
			const Entity grandchild = scene.CreateEntity("Grandchild", child);
			const Entity other = scene.CreateEntity("Other");
			const std::array<Entity, 4> all = { parent, child, grandchild, other };
			const auto matchesIsActive = [&all]()
			{
				return std::ranges::all_of(all, [](Entity entity)
				{
					return entity.HasComponent<HierarchyDisabledTag>() == !entity.IsActive();
				});
			};
			CHECK_FALSE(child.HasComponent<HierarchyDisabledTag>());

			parent.SetActive(false);
			CHECK(grandchild.HasComponent<HierarchyDisabledTag>());
			CHECK(matchesIsActive());

			child.SetActive(false);
			parent.SetActive(true); // the child stays disabled itself, and so does its subtree
			CHECK_FALSE(parent.HasComponent<HierarchyDisabledTag>());
			CHECK(child.HasComponent<HierarchyDisabledTag>());
			CHECK(grandchild.HasComponent<HierarchyDisabledTag>());
			CHECK(matchesIsActive());

			// Moving and creating follow the new parent's state.
			REQUIRE(scene.SetParent(grandchild, other).has_value());
			CHECK_FALSE(grandchild.HasComponent<HierarchyDisabledTag>());
			const Entity created = scene.CreateEntity("Created", child);
			CHECK(created.HasComponent<HierarchyDisabledTag>());
			REQUIRE(scene.SetParent(other, child).has_value());
			CHECK(grandchild.HasComponent<HierarchyDisabledTag>());
			CHECK(matchesIsActive());
			CHECK_FALSE(created.IsActive());

			// The cache is runtime-only: maintaining it is not a mutation of its own.
			const uint64_t revision = scene.GetRevision();
			child.SetActive(true);
			CHECK(scene.GetRevision() == revision + 1);
			CHECK(matchesIsActive());
			CHECK_FALSE(created.HasComponent<HierarchyDisabledTag>());
		}

		TEST_CASE("Entity: tags are unique and keep insertion order")
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Player");
			entity.AddTag("Player");
			entity.AddTag("Hero");
			entity.AddTag("Player");
			CHECK(std::vector<std::string>(entity.GetTags().begin(), entity.GetTags().end()) == std::vector<std::string>{ "Player", "Hero" });
			CHECK(entity.HasTag("Hero"));
			CHECK_FALSE(entity.HasTag("hero")); // case-sensitive
			entity.RemoveTag("Player");
			entity.RemoveTag("Missing");
			CHECK_FALSE(entity.HasTag("Player"));
			CHECK(entity.GetTags().size() == 1);
			CHECK(entity.GetComponent<TagsComponent>().Tags == std::vector<std::string>{ "Hero" });
		}

		TEST_CASE("Entity: children keep sibling order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity a = scene.CreateEntity("A", parent);
			const Entity b = scene.CreateEntity("B", parent);
			const Entity c = scene.CreateEntity("C", parent);
			REQUIRE(scene.SetParent(c, parent, 0u).has_value());

			REQUIRE(parent.GetChildren().size() == 3);
			CHECK(parent.GetChildren()[0] == c.GetUUID());
			CHECK(parent.GetChildren()[1] == a.GetUUID());
			CHECK(parent.GetChildren()[2] == b.GetUUID());
			CHECK(b.GetSiblingIndex() == 2);
			CHECK(b.GetParent() == parent);
			CHECK(b.GetComponent<RelationshipComponent>().Parent == parent.GetUUID());
			CHECK(parent.GetSiblingIndex() == 0);
		}

		TEST_CASE("Entity: handles compare by entity and scene")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntity("Entity");
			CHECK(scene.FindEntityByID(entity.GetUUID()) == entity);
			CHECK(Entity(entity.GetHandle(), &scene) == entity);
			CHECK_FALSE(Entity(entity.GetHandle(), nullptr) == entity);
			CHECK(Entity() == Entity());
		}

		TEST_CASE("Entity: invalid and destroyed entities assert on access")
		{
			const Entity invalid;
			CHECK_FALSE(invalid.IsValid());
			CHECK_FALSE(static_cast<bool>(invalid));
			ENGINE_CHECK_DEATH("Scene/InvalidEntityAccessAsserts", "invalid");
			ENGINE_CHECK_DEATH("Scene/DestroyedEntityAccessAsserts", "invalid");
			ENGINE_CHECK_DEATH("Scene/PendingDestroyEntityAccessAsserts", "invalid");
		}

		TEST_CASE("Entity: removing a component every entity has asserts")
		{
			ENGINE_CHECK_DEATH("Scene/RemovingRequiredComponentAsserts", "are never removed from an entity");
		}
	}

}
