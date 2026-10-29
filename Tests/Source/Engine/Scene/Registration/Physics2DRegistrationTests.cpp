#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <limits>
#include <string_view>

// The Physics2D category (Docs/Decisions/0032-m21-contract.md decision 9): the contract registers M21's 2D physics components,
// so these cases are live; the 2D world's behaviour is frozen by the skipped cases of Physics2DWorld and Physics2DSystem.

namespace Engine {

	namespace {

		const ComponentInfo& RequirePhysics2DComponent(const TypeRegistry& registry, std::string_view name)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			REQUIRE(info != nullptr);
			return *info;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Physics2DRegistration: the 2D physics components are registered after the sprite ones, beside no 3D exclusion")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& camera = RequirePhysics2DComponent(*registry, "PixelPerfectCamera");
			size_t expected = camera.GetIndex() + 1;
			for (const std::string_view name : { "RigidBody2D", "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D" })
			{
				CAPTURE(name);
				const ComponentInfo& info = RequirePhysics2DComponent(*registry, name);
				CHECK(info.GetIndex() == expected++);
				CHECK(info.GetCategory() == "Physics2D");
				CHECK(info.GetVersion() == 1);
				REQUIRE(info.GetRequires().size() == 1);
				CHECK(info.GetRequires()[0] == registry->FindComponent("Transform"));
				// One world per entity is a validation error (PHYSICS2D_MIXED_WORLDS), never a registry exclusion.
				CHECK(info.GetExcludes().empty());
			}
			const ComponentInfo& body = RequirePhysics2DComponent(*registry, "RigidBody2D");
			CHECK(body.FindField("Type")->GetKind() == FieldType::Enum);
			CHECK(body.FindField("EnhancedInternalEdgeRemoval")->GetKind() == FieldType::Bool);
			CHECK(body.FindField("InitialLinearVelocity")->GetKind() == FieldType::Vec2);
			CHECK(body.FindField("InitialAngularVelocity")->GetKind() == FieldType::Float);
			REQUIRE(RequirePhysics2DComponent(*registry, "CircleCollider2D").FindField("Radius")->GetMeta().Min.has_value());
			CHECK(*RequirePhysics2DComponent(*registry, "CircleCollider2D").FindField("Radius")->GetMeta().Min == doctest::Approx(0.001));
			CHECK(RequirePhysics2DComponent(*registry, "PolygonCollider2D").FindField("Points")->GetKind() == FieldType::Array);
		}

		TEST_CASE("Physics2DRegistration: a polygon collider needs 3 to 64 finite points")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& polygon = RequirePhysics2DComponent(*registry, "PolygonCollider2D");
			ResolveContext resolve;
			resolve.Registry = registry.get();
			const auto countErrors = [&polygon, &resolve](const PolygonCollider2DComponent& component)
			{
				ValidationContext context;
				polygon.Validate(&component, resolve, context);
				return context.GetErrorCount();
			};
			PolygonCollider2DComponent component;
			CHECK(countErrors(component) == 0);
			component.Points.resize(2);
			CHECK(countErrors(component) == 1);
			component.Points.assign(MaxPolygonCollider2DPoints + 1, glm::vec2(0.0f));
			CHECK(countErrors(component) == 1);
			component.Points.assign(3, glm::vec2(0.0f));
			component.Points[1].x = std::numeric_limits<float>::infinity();
			CHECK(countErrors(component) >= 1);
		}
	}

}
