#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSurfaceValidation.h"

#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/ColliderSurfaceComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <set>
#include <vector>

// The collider surface check (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision 6; Architecture §13.7
// PHYSICS_SURFACE_UNUSED): a ColliderSurface that applies to no collider, with cases each way.

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsSurfaceValidation: PHYSICS_SURFACE_UNUSED names each ColliderSurface that applies to no collider")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const auto withSurface = [](Entity entity)
			{
				entity.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{});
				return entity;
			};

			// Used: a body's own collider, an implicit static body's, a collider gathered into an ancestor's compound.
			const Entity body = withSurface(Test::AddBoxBody(scene, "Body", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(withSurface(Test::AddBoxBody(scene, "Static", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt)));
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			static_cast<void>(withSurface(Test::AddBoxBody(scene, "Piece", glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt, level)));

			// Unused: no collider; a trigger only; a character's capsule; 2D colliders only; a body a physics error refuses (a
			// Dynamic RigidBody with a triangle mesh); a RigidBody whose colliders are its children's, not its own.
			const Entity bare = withSurface(scene.CreateEntity("Bare"));
			Entity trigger = withSurface(Test::AddBoxBody(scene, "Trigger", glm::vec3(15.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt));
			trigger.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Entity character = withSurface(scene.CreateEntity("Character"));
			character.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			Entity flat = withSurface(scene.CreateEntity("Flat"));
			flat.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{});
			flat.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{});
			Entity refused = withSurface(scene.CreateEntity("Refused"));
			refused.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Dynamic });
			refused.AddComponent<MeshColliderComponent>(MeshColliderComponent{}); // not convex
			const Entity parent = withSurface(level);

			// Not reported: a disabled entity, whose surface applies once it is enabled.
			Entity disabled = withSurface(scene.CreateEntity("Disabled"));
			disabled.SetActive(false);

			const std::vector<PhysicsDiagnostic> diagnostics = ValidateSceneSurfaces(scene, PhysicsLayerTable());
			std::set<UUID> reported;
			for (const PhysicsDiagnostic& diagnostic : diagnostics)
			{
				CAPTURE(diagnostic.Message);
				CHECK(diagnostic.Code == PhysicsSurfaceUnusedCode);
				CHECK(diagnostic.Severity == DiagnosticSeverity::Warning);
				CHECK(diagnostic.Component == "ColliderSurface");
				CHECK_FALSE(diagnostic.AutoFixable);
				CHECK_FALSE(diagnostic.Hint.empty());
				reported.insert(diagnostic.Entity);
			}
			CHECK(reported
				== std::set<UUID>{ bare.GetUUID(), trigger.GetUUID(), character.GetUUID(), flat.GetUUID(), refused.GetUUID(), parent.GetUUID() });
			CHECK(diagnostics.size() == reported.size());
			CHECK_FALSE(reported.contains(body.GetUUID()));
			CHECK(std::is_sorted(diagnostics.begin(), diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
			{
				return a.Entity < b.Entity;
			}));
		}
	}

}
