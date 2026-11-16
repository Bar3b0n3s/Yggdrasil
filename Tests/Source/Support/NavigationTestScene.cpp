#include "TestsPCH.h"
#include "Support/NavigationTestScene.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/NavModifierComponent.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace Engine {

	namespace Test {

		namespace {

			Entity AddBox(Scene& scene, std::string_view name, const glm::vec3& center, const glm::vec3& halfExtents,
				const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
			{
				Entity entity = scene.CreateEntity(name);
				entity.Patch<TransformComponent>([&center, &rotation](TransformComponent& transform)
				{
					transform.Translation = center;
					transform.Rotation = rotation;
				});
				entity.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = halfExtents });
				return entity;
			}

			void AddModifier(Scene& scene, std::string_view name, const glm::vec3& center, const glm::vec3& halfExtents, NavModifierKind kind, float cost)
			{
				Entity entity = scene.CreateEntity(name);
				entity.Patch<TransformComponent>([&center](TransformComponent& transform)
				{
					transform.Translation = center;
				});
				entity.AddComponent<NavModifierComponent>(NavModifierComponent{ .Kind = kind, .Cost = cost, .HalfExtents = halfExtents });
			}

		}

		Entity AddNavigationArena(Scene& scene)
		{
			static_cast<void>(AddBox(scene, "Floor", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(10.0f, 0.5f, 10.0f)));
			static_cast<void>(AddBox(scene, "Pillar", NavArenaPillarCenter, NavArenaPillarHalfExtents));
			static_cast<void>(AddBox(scene, "Platform", NavArenaPlatformCenter, NavArenaPlatformHalfExtents));
			// The ramp's top face runs from (-3, 0) to (3, 2) in XY: a slab 0.2 m thick, its centre half a thickness below
			// the top face's middle (0, 1), turned up by atan(2 / 6) about +Z.
			const float angle = std::atan2(2.0f, 6.0f);
			const glm::vec3 halfExtents(std::sqrt(40.0f) / 2.0f, 0.1f, 1.5f);
			const glm::quat rotation = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
			const glm::vec3 center = glm::vec3(0.0f, 1.0f, 6.0f) - rotation * glm::vec3(0.0f, 0.1f, 0.0f);
			static_cast<void>(AddBox(scene, "Ramp", center, halfExtents, rotation));
			AddModifier(scene, "Lava", NavArenaLavaCenter, NavArenaLavaHalfExtents, NavModifierKind::NotWalkable, 1.0f);
			AddModifier(scene, "Mud", NavArenaCostCenter, NavArenaCostHalfExtents, NavModifierKind::Cost, NavArenaCost);

			Entity navigation = scene.CreateEntity("Navigation");
			NavigationComponent settings;
			settings.AgentTypes = { NavAgentType{ .Name = "Small", .Radius = 0.4f, .Height = 1.8f, .MaxSlope = 45.0f, .MaxStep = 0.4f },
				NavAgentType{ .Name = "Large", .Radius = 1.0f, .Height = 2.0f, .MaxSlope = 45.0f, .MaxStep = 0.4f } };
			settings.CellSize = 0.2f;
			settings.CellHeight = 0.1f;
			navigation.AddComponent<NavigationComponent>(settings);
			return navigation;
		}

		bool IsInsideFootprint(const glm::vec3& point, const glm::vec3& center, const glm::vec3& halfExtents, float margin)
		{
			return std::abs(point.x - center.x) < halfExtents.x + margin && std::abs(point.z - center.z) < halfExtents.z + margin;
		}

	}

}
