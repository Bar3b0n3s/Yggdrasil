#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	static const FieldMeta& FindPhysicsMeta(const TypeRegistry& registry, std::string_view component, std::string_view field)
	{
		const ComponentInfo* info = registry.FindComponent(component);
		REQUIRE(info != nullptr);
		const FieldInfo* fieldInfo = info->FindField(field);
		REQUIRE(fieldInfo != nullptr);
		return fieldInfo->GetMeta();
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsRegistration: RigidBody has EnhancedInternalEdgeRemoval and its relations")
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
			REQUIRE(character->GetRequires().size() == 1);
			CHECK(character->GetRequires()[0] == registry->FindComponent("Transform"));

			REQUIRE(body->FindField("Mass")->GetMeta().Min.has_value());
			CHECK(*body->FindField("Mass")->GetMeta().Min == doctest::Approx(0.001));
			CHECK(body->FindField("Mass")->GetMeta().Unit == "kg");
			CHECK(body->FindField("LockTranslation")->GetKind() == FieldType::Bool3);
		}

		TEST_CASE("PhysicsRegistration: collider dimensions have the 1 mm minimum")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::pair<std::string_view, std::string_view>> dimensions = {
				{ "BoxCollider", "HalfExtents" },
				{ "SphereCollider", "Radius" },
				{ "CapsuleCollider", "Radius" },
				{ "CapsuleCollider", "HalfHeight" },
				{ "CharacterController", "Height" },
				{ "CharacterController", "Radius" },
			};
			for (const auto& [component, field] : dimensions)
			{
				INFO(std::string(component), ".", std::string(field));
				const FieldInfo* info = registry->FindComponent(component)->FindField(field);
				REQUIRE(info != nullptr);
				REQUIRE(info->GetMeta().Min.has_value());
				CHECK(*info->GetMeta().Min == doctest::Approx(MinColliderDimension));
				CHECK(info->GetMeta().Unit == "m");
			}
			for (const std::string_view collider : { "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider" })
			{
				INFO(std::string(collider));
				const ComponentInfo* info = registry->FindComponent(collider);
				REQUIRE(info != nullptr);
				REQUIRE(info->GetRequires().size() == 1);
				CHECK(info->GetRequires()[0] == registry->FindComponent("Transform"));
				CHECK(info->GetExcludes().empty());
				CHECK(info->GetCategory() == "Physics");
			}
			CHECK(registry->FindComponent("MeshCollider")->FindField("Mesh")->GetMeta().AssetFilter == "Mesh");
		}

		TEST_CASE("PhysicsRegistration: body and character bounds keep every value valid for Jolt")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			for (const std::string_view field : { "Friction", "LinearDamping", "AngularDamping", "MaxLinearVelocity", "MaxAngularVelocity" })
			{
				INFO(std::string(field));
				const FieldMeta& meta = FindPhysicsMeta(*registry, "RigidBody", field);
				CHECK(meta.Min == 0.0);
				CHECK_FALSE(meta.Max.has_value());
			}
			CHECK(FindPhysicsMeta(*registry, "RigidBody", "Restitution").Min == 0.0);
			CHECK(FindPhysicsMeta(*registry, "RigidBody", "Restitution").Max == 1.0);
			CHECK(FindPhysicsMeta(*registry, "RigidBody", "MaxLinearVelocity").Unit == "m/s");
			CHECK(FindPhysicsMeta(*registry, "RigidBody", "MaxAngularVelocity").Unit == "rad/s");
			CHECK(FindPhysicsMeta(*registry, "RigidBody", "InitialAngularVelocity").Unit == "rad/s");
			// A body may float or fall upwards.
			CHECK_FALSE(FindPhysicsMeta(*registry, "RigidBody", "GravityFactor").Min.has_value());

			CHECK(FindPhysicsMeta(*registry, "CharacterController", "MaxSlopeAngle").Max == 90.0);
			CHECK(FindPhysicsMeta(*registry, "CharacterController", "MaxSlopeAngle").Unit == "deg");
			CHECK(FindPhysicsMeta(*registry, "CharacterController", "StepHeight").Min == 0.0);
			CHECK(*FindPhysicsMeta(*registry, "CharacterController", "Mass").Min == doctest::Approx(0.001));
		}

		TEST_CASE("PhysicsRegistration: BodyType and MotionQuality values are registered by name")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const EnumInfo* bodyType = registry->FindEnum("BodyType");
			REQUIRE(bodyType != nullptr);
			CHECK(bodyType->GetEntries().size() == 3);
			CHECK(bodyType->FindByName("Kinematic") != nullptr);
			CHECK(bodyType->FindByName("Dynamic")->Value == static_cast<int64_t>(BodyType::Dynamic));
			const EnumInfo* motion = registry->FindEnum("MotionQuality");
			REQUIRE(motion != nullptr);
			CHECK(motion->FindByName("LinearCast") != nullptr);
			CHECK(registry->FindComponent("RigidBody")->FindField("MotionQuality")->GetType().GetEnum() == motion);
		}
	}

}
