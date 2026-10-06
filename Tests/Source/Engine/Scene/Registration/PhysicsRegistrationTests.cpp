#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsRegistration: RigidBody has EnhancedInternalEdgeRemoval and its relations" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* body = registry->FindComponent("RigidBody");
			REQUIRE(body != nullptr);

			const FieldInfo* edgeRemoval = body->FindField("EnhancedInternalEdgeRemoval");
			REQUIRE(edgeRemoval != nullptr);
			CHECK(edgeRemoval->GetKind() == FieldType::Bool);

			REQUIRE(body->GetRequires().size() == 1);
			CHECK(body->GetRequires()[0] == registry->FindComponent("Transform"));
			REQUIRE(body->GetExcludes().size() == 1);
			CHECK(body->GetExcludes()[0] == registry->FindComponent("CharacterController"));

			const ComponentInfo* character = registry->FindComponent("CharacterController");
			REQUIRE(character != nullptr);
			REQUIRE(character->GetExcludes().size() == 1);
			CHECK(character->GetExcludes()[0] == body);

			REQUIRE(body->FindField("Mass")->GetMeta().Min.has_value());
			CHECK(*body->FindField("Mass")->GetMeta().Min == doctest::Approx(0.001));
			CHECK(body->FindField("Mass")->GetMeta().Unit == "kg");
			CHECK(body->FindField("LockTranslation")->GetKind() == FieldType::Bool3);
		}

		TEST_CASE("PhysicsRegistration: collider dimensions have the 1 mm minimum" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::pair<std::string_view, std::string_view>> dimensions = {
				{ "BoxCollider", "HalfExtents" },
				{ "SphereCollider", "Radius" },
				{ "CapsuleCollider", "Radius" },
				{ "CapsuleCollider", "HalfHeight" },
			};
			for (const auto& [component, field] : dimensions)
			{
				INFO(std::string(component), ".", std::string(field));
				const FieldInfo* info = registry->FindComponent(component)->FindField(field);
				REQUIRE(info != nullptr);
				REQUIRE(info->GetMeta().Min.has_value());
				CHECK(*info->GetMeta().Min == doctest::Approx(MinColliderDimension));
			}
			for (const std::string_view collider : { "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider" })
			{
				INFO(std::string(collider));
				const ComponentInfo* info = registry->FindComponent(collider);
				REQUIRE(info != nullptr);
				REQUIRE(info->GetRequires().size() == 1);
				CHECK(info->GetRequires()[0] == registry->FindComponent("Transform"));
			}
		}

		TEST_CASE("PhysicsRegistration: BodyType and MotionQuality values are registered by name" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const EnumInfo* bodyType = registry->FindEnum("BodyType");
			REQUIRE(bodyType != nullptr);
			CHECK(bodyType->GetEntries().size() == 3);
			CHECK(bodyType->FindByName("Kinematic") != nullptr);
			const EnumInfo* motion = registry->FindEnum("MotionQuality");
			REQUIRE(motion != nullptr);
			CHECK(motion->FindByName("LinearCast") != nullptr);
		}
	}

}
