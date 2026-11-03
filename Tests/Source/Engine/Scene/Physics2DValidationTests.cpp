#include "TestsPCH.h"

#include "Engine/Scene/Physics2DValidation.h"

#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/PolygonCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The 2D physics validation codes (Docs/Decisions/0032-m21-contract.md decision 12, stream P2; Roadmap M21 "An entity holding
// both 2D and 3D physics components is a validation error").

namespace Engine {

	namespace {

		Entity CreatePlanarEntity(Scene& scene, std::string_view name, const glm::vec2& position)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([position](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(position, 0.0f);
			});
			return entity;
		}

		Entity CreatePlanarBody(Scene& scene, std::string_view name, const glm::vec2& position, BodyType type)
		{
			Entity entity = CreatePlanarEntity(scene, name, position);
			entity.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = type });
			return entity;
		}

		PhysicsLayerTable MakeDefaultLayers()
		{
			Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(std::vector<std::string>{ "Default" }, std::vector<std::vector<std::string>>{ { "Default", "Default" } });
			REQUIRE(layers.has_value());
			return std::move(*layers);
		}

		// The one diagnostic of `diagnostics` with `code` on `entity`, or nullopt (none, or more than one).
		std::optional<PhysicsDiagnostic> FindDiagnostic(std::span<const PhysicsDiagnostic> diagnostics, std::string_view code, UUID entity)
		{
			std::optional<PhysicsDiagnostic> found;
			for (const PhysicsDiagnostic& diagnostic : diagnostics)
			{
				if (diagnostic.Code != code || diagnostic.Entity != entity)
					continue;
				if (found.has_value())
					return std::nullopt;
				found = diagnostic;
			}
			return found;
		}

		// Checks that `diagnostics` holds one diagnostic of `code` on `entity`, with `severity`, `component` and `field`.
		void CheckReported(std::span<const PhysicsDiagnostic> diagnostics, std::string_view code, UUID entity, DiagnosticSeverity severity,
			std::string_view component, std::string_view field)
		{
			CAPTURE(code);
			const std::optional<PhysicsDiagnostic> found = FindDiagnostic(diagnostics, code, entity);
			REQUIRE(found.has_value());
			CHECK(found->Severity == severity);
			CHECK(found->Component == component);
			CHECK(found->Field == field);
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Physics2DValidation: a mixed entity and an off-plane body are reported")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity mixed = CreatePlanarEntity(scene, "Mixed", glm::vec2(0.0f));
			mixed.AddComponent<RigidBody2DComponent>();
			mixed.AddComponent<CircleCollider2DComponent>();
			mixed.AddComponent<BoxColliderComponent>();
			Entity tilted = CreatePlanarEntity(scene, "Tilted", glm::vec2(4.0f, 0.0f));
			tilted.AddComponent<RigidBody2DComponent>();
			tilted.AddComponent<BoxCollider2DComponent>();
			tilted.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			});
			Entity spun = CreatePlanarEntity(scene, "Spun", glm::vec2(8.0f, 0.0f));
			spun.AddComponent<RigidBody2DComponent>();
			spun.AddComponent<BoxCollider2DComponent>();
			spun.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			});
			TransformSystem::Update(scene);

			Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(std::vector<std::string>{ "Default" }, std::vector<std::vector<std::string>>{ { "Default", "Default" } });
			REQUIRE(layers.has_value());
			const std::vector<PhysicsDiagnostic> diagnostics = ValidateScenePhysics2D(scene, *layers);
			REQUIRE(diagnostics.size() == 2);
			CHECK(diagnostics[0].Code == Physics2DMixedWorldsCode);
			CHECK(diagnostics[0].Entity == mixed.GetUUID());
			CHECK(diagnostics[1].Code == Physics2DOffPlaneCode);
			CHECK(diagnostics[1].Entity == tilted.GetUUID());
			CHECK(diagnostics[1].Severity == DiagnosticSeverity::Warning);
		}

		// Decision 12 (W21.1's review): the 2D world reports the rules it shares with the 3D one under the 3D codes, naming the
		// 2D component and field, both in project.validate's check (ValidateScenePhysics2D) and in the play session's system,
		// which refuses the bodies the 3D world would refuse.
		TEST_CASE("Physics2DValidation: the shared PHYSICS_* rules are reported for 2D bodies, naming the 2D component")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			// An undeclared layer: reported on the RigidBody2D's Layer; the body is made on layer 0.
			Entity lost = CreatePlanarBody(scene, "Lost", glm::vec2(0.0f), BodyType::Dynamic);
			lost.Patch<RigidBody2DComponent>([](RigidBody2DComponent& body)
			{
				body.Layer = "Nowhere";
			});
			lost.AddComponent<BoxCollider2DComponent>();
			// Colliders that disagree on IsTrigger: refused.
			Entity mixed = CreatePlanarBody(scene, "Mixed", glm::vec2(4.0f, 0.0f), BodyType::Static);
			mixed.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .IsTrigger = true });
			mixed.AddComponent<CircleCollider2DComponent>();
			// A Dynamic body of triggers: refused; a Static one is a Kinematic sensor.
			Entity sensor = CreatePlanarBody(scene, "Sensor", glm::vec2(8.0f, 0.0f), BodyType::Dynamic);
			sensor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .IsTrigger = true });
			Entity gate = CreatePlanarBody(scene, "Gate", glm::vec2(12.0f, 0.0f), BodyType::Static);
			gate.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .IsTrigger = true });
			// A Dynamic body under a Kinematic one: a warning, the body simulates in world space.
			Entity platform = CreatePlanarBody(scene, "Platform", glm::vec2(16.0f, 0.0f), BodyType::Kinematic);
			platform.AddComponent<BoxCollider2DComponent>();
			Entity crate = CreatePlanarBody(scene, "Crate", glm::vec2(0.0f, 2.0f), BodyType::Dynamic);
			crate.AddComponent<BoxCollider2DComponent>();
			REQUIRE(scene.SetParent(crate, platform, {}, false).has_value());
			// A circle under a scale whose X and Y differ: a warning naming the CircleCollider2D; a scale that differs only in Z
			// is uniform in the plane.
			Entity egg = CreatePlanarBody(scene, "Egg", glm::vec2(20.0f, 0.0f), BodyType::Static);
			egg.AddComponent<CircleCollider2DComponent>();
			egg.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f, 1.0f, 1.0f);
			});
			Entity ball = CreatePlanarBody(scene, "Ball", glm::vec2(24.0f, 0.0f), BodyType::Static);
			ball.AddComponent<CircleCollider2DComponent>();
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f, 2.0f, 7.0f);
			});
			// A polygon whose points lie on one line has no hull: an invalid shape, named on its collider entity.
			Entity sliver = CreatePlanarBody(scene, "Sliver", glm::vec2(28.0f, 0.0f), BodyType::Static);
			Entity edge = CreatePlanarEntity(scene, "Edge", glm::vec2(0.0f));
			edge.AddComponent<PolygonCollider2DComponent>(PolygonCollider2DComponent{ .Points = { glm::vec2(-1.0f, 0.0f), glm::vec2(0.0f), glm::vec2(1.0f, 0.0f) } });
			REQUIRE(scene.SetParent(edge, sliver, {}, false).has_value());
			TransformSystem::Update(scene);

			const PhysicsLayerTable layers = MakeDefaultLayers();
			const std::vector<PhysicsDiagnostic> validated = ValidateScenePhysics2D(scene, layers);
			CheckReported(validated, PhysicsUnknownLayerCode, lost.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Layer");
			CheckReported(validated, PhysicsMixedTriggerCode, mixed.GetUUID(), DiagnosticSeverity::Error, "", "IsTrigger");
			CheckReported(validated, PhysicsDynamicTriggerCode, sensor.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Type");
			CheckReported(validated, PhysicsDynamicUnderMovingParentCode, crate.GetUUID(), DiagnosticSeverity::Warning, "RigidBody2D", "Type");
			CheckReported(validated, PhysicsNonuniformScaleCode, egg.GetUUID(), DiagnosticSeverity::Warning, "CircleCollider2D", "");
			CheckReported(validated, PhysicsInvalidShapeCode, edge.GetUUID(), DiagnosticSeverity::Error, "PolygonCollider2D", "");
			CHECK_FALSE(FindDiagnostic(validated, PhysicsNonuniformScaleCode, ball.GetUUID()).has_value());
			CHECK(validated.size() == 6);

			// The play session's system reports the same rules and creates the bodies the rules allow.
			const Test::ExpectLog unknownLogged(LogLevel::Error, PhysicsUnknownLayerCode);
			const Test::ExpectLog mixedLogged(LogLevel::Error, PhysicsMixedTriggerCode);
			const Test::ExpectLog triggerLogged(LogLevel::Error, PhysicsDynamicTriggerCode);
			const Test::ExpectLog shapeLogged(LogLevel::Error, PhysicsInvalidShapeCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const std::span<const PhysicsDiagnostic> raised = (*system)->GetDiagnostics();
			CheckReported(raised, PhysicsUnknownLayerCode, lost.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Layer");
			CheckReported(raised, PhysicsMixedTriggerCode, mixed.GetUUID(), DiagnosticSeverity::Error, "", "IsTrigger");
			CheckReported(raised, PhysicsDynamicTriggerCode, sensor.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Type");
			CheckReported(raised, PhysicsDynamicUnderMovingParentCode, crate.GetUUID(), DiagnosticSeverity::Warning, "RigidBody2D", "Type");
			CheckReported(raised, PhysicsNonuniformScaleCode, egg.GetUUID(), DiagnosticSeverity::Warning, "CircleCollider2D", "");
			CheckReported(raised, PhysicsInvalidShapeCode, edge.GetUUID(), DiagnosticSeverity::Error, "PolygonCollider2D", "");
			CHECK((*system)->GetBodyBounds(lost.GetUUID()).has_value());
			CHECK_FALSE((*system)->GetBodyBounds(mixed.GetUUID()).has_value());
			CHECK_FALSE((*system)->GetBodyBounds(sensor.GetUUID()).has_value());
			CHECK((*system)->GetMotionType(gate.GetUUID()).value_or(PhysicsMotionType::Static) == PhysicsMotionType::Kinematic);
			CHECK((*system)->GetBodyBounds(crate.GetUUID()).has_value());
			CHECK((*system)->GetBodyBounds(egg.GetUUID()).has_value());
			CHECK_FALSE((*system)->GetBodyBounds(sliver.GetUUID()).has_value());
		}

		// Decision 12 (W21.1's review): the 2D bodies beyond the world's limit are refused in canonical order, one
		// PHYSICS_LIMIT_EXCEEDED Error per refused owner, subject "bodies", as the 3D world's.
		TEST_CASE("Physics2DValidation: 2D bodies beyond the body limit are refused with PHYSICS_LIMIT_EXCEEDED in canonical order")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			std::vector<Entity> boxes;
			for (int index = 0; index < 5; ++index)
			{
				Entity box = CreatePlanarBody(scene, "Box", glm::vec2(static_cast<float>(index) * 2.0f, 0.0f), BodyType::Dynamic);
				box.AddComponent<BoxCollider2DComponent>();
				boxes.push_back(box);
			}
			TransformSystem::Update(scene);
			const Test::ExpectLog limitLogged(LogLevel::Error, PhysicsLimitExceededCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Limits = PhysicsWorldLimits{ .MaxBodies = 3 } });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			CHECK((*system)->GetStats().BodyCount == 3);
			for (size_t index = 0; index < boxes.size(); ++index)
			{
				CAPTURE(index);
				const std::optional<PhysicsDiagnostic> limit = FindDiagnostic((*system)->GetDiagnostics(), PhysicsLimitExceededCode, boxes[index].GetUUID());
				CHECK((*system)->GetBodyBounds(boxes[index].GetUUID()).has_value() == (index < 3));
				REQUIRE(limit.has_value() == (index >= 3));
				if (limit.has_value())
				{
					CHECK(limit->Severity == DiagnosticSeverity::Error);
					CHECK(limit->Subject == "bodies");
				}
			}
		}
	}

}
