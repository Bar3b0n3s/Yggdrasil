#include "EnginePCH.h"
#include "Engine/Scene/JointDebugDraw.h"

#include "Engine/Core/Assert.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/SliderJointComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/PhysicsJoints.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <cmath>
#include <optional>

namespace Engine {

	namespace Utils {

		// The matrix an entity is drawn at (as the colliders' records are: ColliderDebugDrawOptions::Alpha): in a runtime scene
		// below alpha 1, its rendered pose; otherwise its world matrix.
		static glm::mat4 GetJointDrawnMatrix(ConstEntity entity, float alpha)
		{
			if (alpha < 1.0f && entity.GetScene()->IsRuntime())
				return ComputeRenderedWorldMatrix(entity, alpha);
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		static bool IsFiniteJointVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// A world point of the body `owner` (the invalid UUID: the world) moved from where the body is now to where it is drawn.
		static glm::vec3 ToDrawnPoint(const Scene& scene, UUID owner, const glm::vec3& point, float alpha)
		{
			const ConstEntity entity = owner.IsValid() ? scene.FindEntityByID(owner) : ConstEntity();
			if (!entity.IsValid() || alpha >= 1.0f)
				return point;
			const glm::vec3 local = glm::vec3(glm::affineInverse(TransformSystem::ComputeWorldMatrix(entity)) * glm::vec4(point, 1.0f));
			return glm::vec3(GetJointDrawnMatrix(entity, alpha) * glm::vec4(local, 1.0f));
		}

		// As ToDrawnPoint, for a direction.
		static glm::vec3 ToDrawnDirection(const Scene& scene, UUID owner, const glm::vec3& direction, float alpha)
		{
			const ConstEntity entity = owner.IsValid() ? scene.FindEntityByID(owner) : ConstEntity();
			if (!entity.IsValid() || alpha >= 1.0f || direction == glm::vec3(0.0f))
				return direction;
			const glm::vec3 local = glm::inverse(glm::mat3(TransformSystem::ComputeWorldMatrix(entity))) * direction;
			return glm::normalize(glm::mat3(GetJointDrawnMatrix(entity, alpha)) * local);
		}

	}

	glm::vec4 GetJointDebugColor(JointDebugState state)
	{
		switch (state)
		{
			case JointDebugState::InForce: return glm::vec4(0.0f, 0.75f, 0.8f, 1.0f);
			case JointDebugState::Broken:  return glm::vec4(1.0f, 0.45f, 0.0f, 1.0f);
			case JointDebugState::Refused: return glm::vec4(0.85f, 0.0f, 0.35f, 1.0f);
		}
		ENGINE_CORE_ASSERT(false, "Unknown JointDebugState {}", std::to_underlying(state));
		return glm::vec4(1.0f);
	}

	std::vector<JointDebugShape> BuildJointDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const PhysicsSystem* physics, float alpha)
	{
		ENGINE_CORE_ASSERT(alpha >= 0.0f && alpha <= 1.0f, "BuildJointDebugDraw: the alpha {} is outside [0, 1]", alpha);
		std::vector<JointDebugShape> shapes;
		const entt::registry& registry = scene.GetRegistry();
		if (registry.view<HingeJointComponent>().empty() && registry.view<SliderJointComponent>().empty() && registry.view<DistanceJointComponent>().empty()
			&& registry.view<FixedJointComponent>().empty())
		{
			return shapes;
		}
		const PhysicsJointComposition joints = ComposePhysicsJoints(scene, ComposePhysicsBodies(scene, layers));
		for (const PhysicsJointPlan& plan : joints.Joints)
		{
			const ConstEntity entity = scene.FindEntityByID(plan.Entity);
			const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(entity);
			if (!fields.has_value())
				continue;
			JointDebugShape shape;
			shape.Entity = plan.Entity;
			shape.Type = plan.Type;
			shape.State = !plan.IsCreatable ? JointDebugState::Refused : (fields->Broken ? JointDebugState::Broken : JointDebugState::InForce);

			const std::optional<PhysicsJointReport> report = physics != nullptr ? physics->GetJointInfo(plan.Entity) : std::nullopt;
			if (report.has_value() && report->Created)
			{
				// Where the bodies hold it, moved to where they are drawn.
				shape.Point = Utils::ToDrawnPoint(scene, report->Body, report->Point, alpha);
				shape.ConnectedPoint = Utils::ToDrawnPoint(scene, report->Connected, report->ConnectedPoint, alpha);
				shape.Axis = Utils::ToDrawnDirection(scene, report->Connected, report->Axis, alpha);
			}
			else
			{
				// From the components: where the session would make it now.
				if (physics != nullptr && shape.State == JointDebugState::InForce)
					shape.State = JointDebugState::Refused;
				const glm::mat4 world = Utils::GetJointDrawnMatrix(entity, alpha);
				if (const std::optional<PhysicsJointFrame> frame = MakePhysicsJointWorldFrame(world, *fields))
				{
					shape.Point = frame->Point;
					if (plan.Type == PhysicsJointType::Hinge || plan.Type == PhysicsJointType::Slider)
						shape.Axis = frame->Axis;
					// The world refuses a frame beyond its range (PhysicsWorld::CreateJoint), as the session would.
					if (!IsWithinPhysicsRange(frame->Point) && shape.State == JointDebugState::InForce)
						shape.State = JointDebugState::Refused;
				}
				else
				{
					// An axis the entity's transform makes vanish: the session's "frame" refusal.
					shape.Point = glm::vec3(world[3]);
					if (shape.State == JointDebugState::InForce)
						shape.State = JointDebugState::Refused;
				}
				shape.ConnectedPoint = shape.Point;
				if (plan.Type == PhysicsJointType::Distance)
				{
					const ConstEntity connected = fields->ConnectedBody.IsValid() ? scene.FindEntityByID(fields->ConnectedBody) : ConstEntity();
					const glm::mat4 connectedWorld = connected.IsValid() ? Utils::GetJointDrawnMatrix(connected, alpha) : glm::mat4(1.0f);
					shape.ConnectedPoint = glm::vec3(connectedWorld * glm::vec4(fields->ConnectedAnchor, 1.0f));
				}
			}
			if (!Utils::IsFiniteJointVector(shape.Point) || !Utils::IsFiniteJointVector(shape.ConnectedPoint) || !Utils::IsFiniteJointVector(shape.Axis))
				continue;
			shapes.push_back(shape);
		}
		return shapes;
	}

	void AppendJointDebugDraw(std::span<const JointDebugShape> shapes, DebugDrawList& list)
	{
		constexpr float Half = JointDebugCrossSize * 0.5f;
		for (const JointDebugShape& shape : shapes)
		{
			const glm::vec4 color = GetJointDebugColor(shape.State);
			for (const glm::vec3& axis : { glm::vec3(Half, 0.0f, 0.0f), glm::vec3(0.0f, Half, 0.0f), glm::vec3(0.0f, 0.0f, Half) })
				list.AddLine(shape.Point - axis, shape.Point + axis, color, 0.0f, DebugDepthMode::OnTop);
			if (shape.Axis != glm::vec3(0.0f))
			{
				const glm::vec3 half = shape.Axis * (JointDebugAxisLength * 0.5f);
				list.AddLine(shape.Point - half, shape.Point + half, color, 0.0f, DebugDepthMode::OnTop);
			}
			if (shape.ConnectedPoint != shape.Point)
				list.AddLine(shape.Point, shape.ConnectedPoint, color, 0.0f, DebugDepthMode::OnTop);
		}
	}

}
