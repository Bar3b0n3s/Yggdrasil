#include "TestsPCH.h"

#include "Engine/Scene/Physics2DValidation.h"

#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

// The 2D physics validation codes (Docs/Decisions/0032-m21-contract.md decision 12, stream P2; Roadmap M21 "An entity holding
// both 2D and 3D physics components is a validation error"): skipped until stream P2 implements them.

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

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Physics2DValidation: a mixed entity and an off-plane body are reported" * doctest::skip(true))
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
	}

}
