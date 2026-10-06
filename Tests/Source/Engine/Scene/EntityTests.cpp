#include "TestsPCH.h"

#include "Engine/Scene/Entity.h"

#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/NameComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
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

	TEST_SUITE("Scene")
	{
		TEST_CASE("Entity: a new entity has the required components" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Ball");
			REQUIRE(entity.IsValid());
			CHECK(entity.HasComponent<IDComponent>());
			CHECK(entity.HasComponent<NameComponent>());
			CHECK(entity.HasComponent<TagsComponent>());
			CHECK(entity.HasComponent<RelationshipComponent>());
			CHECK(entity.HasComponent<TransformComponent>());
			CHECK(entity.GetUUID().IsValid());
			CHECK(entity.GetName() == "Ball");
			CHECK(entity.IsActive());
			CHECK(entity.GetTags().empty());
		}

		TEST_CASE("Entity: components are added, patched and removed" * doctest::skip(true))
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

		TEST_CASE("Entity: the active state follows the hierarchy" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			const Entity child = scene.CreateEntity("Child", parent);

			parent.SetActive(false);
			CHECK(parent.HasComponent<DisabledTag>());
			CHECK_FALSE(parent.IsActiveSelf());
			CHECK(child.IsActiveSelf());
			CHECK_FALSE(child.IsActive());

			parent.SetActive(true);
			CHECK(child.IsActive());
			CHECK_FALSE(parent.HasComponent<DisabledTag>());
		}

		TEST_CASE("Entity: tags are unique and keep insertion order" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Player");
			entity.AddTag("Player");
			entity.AddTag("Hero");
			entity.AddTag("Player");
			CHECK(std::vector<std::string>(entity.GetTags().begin(), entity.GetTags().end()) == std::vector<std::string>{ "Player", "Hero" });
			CHECK(entity.HasTag("Hero"));
			entity.RemoveTag("Player");
			entity.RemoveTag("Missing");
			CHECK_FALSE(entity.HasTag("Player"));
			CHECK(entity.GetTags().size() == 1);
		}

		TEST_CASE("Entity: children keep sibling order" * doctest::skip(true))
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
		}

		TEST_CASE("Entity: invalid and destroyed entities assert on access" * doctest::skip(true))
		{
			const Entity invalid;
			CHECK_FALSE(invalid.IsValid());
			CHECK_FALSE(static_cast<bool>(invalid));
			ENGINE_CHECK_DEATH("Scene/InvalidEntityAccessAsserts", "invalid");
			ENGINE_CHECK_DEATH("Scene/DestroyedEntityAccessAsserts", "invalid");
		}
	}

}
