#include "TestsPCH.h"
#include "Support/PhysicsTestScene.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <iterator>

namespace Engine {

	namespace Test {

		namespace Utils {

			// A new entity `name` at `position`, under `parent` when valid.
			static Entity CreatePlacedEntity(Scene& scene, std::string_view name, const glm::vec3& position, Entity parent)
			{
				Entity entity = parent.IsValid() ? scene.CreateEntity(name, parent) : scene.CreateEntity(name);
				entity.Patch<TransformComponent>([&position](TransformComponent& transform)
				{
					transform.Translation = position;
				});
				return entity;
			}

			static void AddRigidBody(Entity entity, std::optional<BodyType> type)
			{
				if (type.has_value())
					entity.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = *type });
			}

		}

		Entity AddBoxBody(Scene& scene, std::string_view name, const glm::vec3& position, const glm::vec3& halfExtents, std::optional<BodyType> type,
			Entity parent)
		{
			Entity entity = Utils::CreatePlacedEntity(scene, name, position, parent);
			entity.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = halfExtents });
			Utils::AddRigidBody(entity, type);
			return entity;
		}

		Entity AddSphereBody(Scene& scene, std::string_view name, const glm::vec3& position, float radius, std::optional<BodyType> type, Entity parent)
		{
			Entity entity = Utils::CreatePlacedEntity(scene, name, position, parent);
			entity.AddComponent<SphereColliderComponent>(SphereColliderComponent{ .Radius = radius });
			Utils::AddRigidBody(entity, type);
			return entity;
		}

		Entity AddGround(Scene& scene, std::string_view name)
		{
			return AddBoxBody(scene, name, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(50.0f, 0.5f, 50.0f), BodyType::Static);
		}

		void PatchRigidBody(Entity entity, const std::function<void(RigidBodyComponent&)>& patch)
		{
			ENGINE_CORE_ASSERT(entity.HasComponent<RigidBodyComponent>(), "PatchRigidBody: '{}' has no RigidBody", entity.GetName());
			entity.Patch<RigidBodyComponent>([&patch](RigidBodyComponent& body)
			{
				patch(body);
			});
		}

		PlaySessionSpecification MakePhysicsSessionSpecification(SceneTestFixture& fixture, uint64_t seed, PlayMode mode)
		{
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = seed;
			specification.Mode = mode;
			return specification;
		}

		Result<Scope<PlaySession>> StartPhysicsSession(SceneTestFixture& fixture, const PlaySessionSpecification& specification)
		{
			return PlaySession::CreateFromScene(specification, fixture.GetScene());
		}

		void RunTicks(PlaySession& session, uint32_t ticks)
		{
			for (uint32_t tick = 0; tick < ticks; ++tick)
				session.Tick();
		}

		glm::vec3 GetWorldPosition(PlaySession& session, std::string_view path)
		{
			const Entity entity = session.GetScene().FindEntityByPath(path);
			ENGINE_CORE_ASSERT(entity.IsValid(), "GetWorldPosition: no entity at '{}'", path);
			return TransformSystem::GetWorldPosition(entity);
		}

		UUID GetEntityId(const Scene& scene, std::string_view path)
		{
			const ConstEntity entity = scene.FindEntityByPath(path);
			return entity.IsValid() ? entity.GetUUID() : UUID();
		}

		void RecordingPhysicsListener::OnPhysicsEvent(const PhysicsEvent& event)
		{
			Events.push_back(event);
			if (OnEvent)
				OnEvent(event);
		}

		void RecordingPhysicsListener::OnPhysicsDiagnostic(const PhysicsDiagnostic& diagnostic)
		{
			Diagnostics.push_back(diagnostic);
		}

		std::vector<PhysicsEvent> RecordingPhysicsListener::Find(UUID self, PhysicsEventType type) const
		{
			std::vector<PhysicsEvent> found;
			std::copy_if(Events.begin(), Events.end(), std::back_inserter(found), [self, type](const PhysicsEvent& event)
			{
				return event.Self == self && event.Type == type;
			});
			return found;
		}

	}

}
