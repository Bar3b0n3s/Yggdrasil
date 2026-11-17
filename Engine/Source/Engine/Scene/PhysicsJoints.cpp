#include "EnginePCH.h"
#include "Engine/Scene/PhysicsJoints.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SliderJointComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace Engine {

	namespace {

		// The body plans of a composition by owner, for the joints' lookups (output order comes from the canonical walk).
		struct BodyIndex
		{
			std::map<UUID, const PhysicsBodyPlan*> RigidBodies;
			std::map<UUID, const PhysicsBodyPlan*> StaticOwners;
			// The implicit static body each solid collider entity belongs to (its own, or the one an ancestor gathers it into).
			std::map<UUID, const PhysicsBodyPlan*> StaticColliders;
		};

		// Where one end of a joint resolved to.
		struct ResolvedEnd
		{
			UUID Owner{};
			PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody;
			// The reason it resolved to no body (a PHYSICS_JOINT_INVALID subject) and the message's middle part; empty when it
			// resolved, or when it cannot be made without a diagnostic of its own (Silent).
			std::string Subject{};
			std::string Reason{};
			bool Silent = false;

			[[nodiscard]] bool IsResolved() const { return Subject.empty() && !Silent; }
		};

	}

	namespace Utils {

		// The smallest axis length a joint accepts (below it the axis has no direction).
		constexpr float MinJointAxisLength = 1.0e-6f;

		// "'Name' (<uuid>)", how diagnostics name an entity (as the body composition's).
		static std::string DescribeJointEntity(ConstEntity entity)
		{
			return std::format("'{}' ({})", entity.GetName(), entity.GetUUID());
		}

		// Whether neither `entity` nor an ancestor holds DisabledTag (the body composition's rule).
		static bool IsEffectivelyEnabled(ConstEntity entity)
		{
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (current.HasComponent<DisabledTag>())
					return false;
			}
			return true;
		}

		static BodyIndex IndexBodies(const PhysicsComposition& bodies)
		{
			BodyIndex index;
			for (const PhysicsBodyPlan& plan : bodies.Bodies)
			{
				if (plan.Origin == PhysicsBodyOrigin::RigidBody)
				{
					index.RigidBodies.emplace(plan.Owner, &plan);
				}
				else if (plan.Origin == PhysicsBodyOrigin::ImplicitStatic)
				{
					index.StaticOwners.emplace(plan.Owner, &plan);
					for (const PhysicsColliderPlan& collider : plan.Colliders)
						index.StaticColliders.emplace(collider.Entity, &plan);
				}
			}
			return index;
		}

		// The end at `entity`: the RigidBody at or above it with no CharacterController or RigidBody2D on the way, else (when
		// `allowStatic`, for ConnectedBody) the implicit static body its own colliders belong to. `role` names the end in the
		// messages.
		static ResolvedEnd ResolveEnd(ConstEntity entity, const BodyIndex& index, bool allowStatic, std::string_view role)
		{
			ResolvedEnd end;
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (current.HasComponent<RigidBodyComponent>())
				{
					const auto plan = index.RigidBodies.find(current.GetUUID());
					if (plan == index.RigidBodies.end() || !plan->second->IsCreatable)
					{
						// A RigidBody without colliders is listed but makes no body, without a diagnostic of its own; one a body
						// diagnostic refused (it has colliders) has its diagnostic already.
						if (plan != index.RigidBodies.end() && plan->second->Colliders.empty())
						{
							end.Subject = role == "joint" ? "body" : "connected";
							end.Reason = std::format("the RigidBody of {} makes no body: it has no solid collider", DescribeJointEntity(current));
						}
						else
						{
							end.Silent = true;
						}
						return end;
					}
					end.Owner = current.GetUUID();
					end.Origin = PhysicsBodyOrigin::RigidBody;
					return end;
				}
				if (current.HasComponent<CharacterControllerComponent>())
				{
					end.Subject = "character";
					end.Reason = std::format("its {} end {} is under the CharacterController of {}, and a character is no body", role,
						DescribeJointEntity(entity), DescribeJointEntity(current));
					return end;
				}
				if (current.HasComponent<RigidBody2DComponent>())
				{
					end.Subject = "2d";
					end.Reason = std::format("its {} end {} is under the RigidBody2D of {}, which belongs to the 2D world", role, DescribeJointEntity(entity),
						DescribeJointEntity(current));
					return end;
				}
			}
			if (allowStatic)
			{
				const UUID id = entity.GetUUID();
				const PhysicsBodyPlan* plan = nullptr;
				if (const auto owned = index.StaticOwners.find(id); owned != index.StaticOwners.end())
					plan = owned->second;
				else if (const auto gathered = index.StaticColliders.find(id); gathered != index.StaticColliders.end())
					plan = gathered->second;
				if (plan != nullptr)
				{
					if (!plan->IsCreatable)
					{
						end.Silent = true;
						return end;
					}
					end.Owner = plan->Owner;
					end.Origin = PhysicsBodyOrigin::ImplicitStatic;
					return end;
				}
				end.Subject = "connected";
				end.Reason = std::format("its ConnectedBody {} has no RigidBody at or above it and no solid collider of its own, so it is no body",
					DescribeJointEntity(entity));
				return end;
			}
			end.Subject = "body";
			end.Reason = std::format("{} has no RigidBody at or above it, so the joint has no body", DescribeJointEntity(entity));
			return end;
		}

		static PhysicsDiagnostic MakeJointDiagnostic(ConstEntity entity, PhysicsJointType type, std::string_view field, std::string_view subject,
			std::string_view reason, std::string hint)
		{
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsJointInvalidCode);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Entity = entity.GetUUID();
			diagnostic.Component = std::string(GetPhysicsJointComponentName(type));
			diagnostic.Field = std::string(field);
			diagnostic.Subject = std::string(subject);
			diagnostic.Message = std::format("the {} of {} cannot be made: {}; the joint is not created", GetPhysicsJointComponentName(type), DescribeJointEntity(entity),
				reason);
			diagnostic.Hint = std::move(hint);
			return diagnostic;
		}

		// The world axis least aligned with `axis` (a unit vector), crossed with it and normalized: a unit vector perpendicular
		// to it, by cross products only.
		static glm::vec3 MakePerpendicular(const glm::vec3& axis)
		{
			const glm::vec3 magnitude = glm::abs(axis);
			glm::vec3 other(0.0f, 0.0f, 1.0f);
			if (magnitude.x <= magnitude.y && magnitude.x <= magnitude.z)
				other = glm::vec3(1.0f, 0.0f, 0.0f);
			else if (magnitude.y <= magnitude.z)
				other = glm::vec3(0.0f, 1.0f, 0.0f);
			return glm::normalize(glm::cross(axis, other));
		}

		static bool IsFiniteVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// `axis` normalized and `normal` made perpendicular to it and normalized (Gram-Schmidt), falling back to a constructed
		// perpendicular when `normal` is (nearly) parallel to it.
		static std::pair<glm::vec3, glm::vec3> Orthonormalize(const glm::vec3& axis, const glm::vec3& normal)
		{
			const glm::vec3 unitAxis = glm::normalize(axis);
			const glm::vec3 projected = normal - unitAxis * glm::dot(unitAxis, normal);
			const float length = glm::length(projected);
			if (!(length > 1.0e-4f))
				return { unitAxis, MakePerpendicular(unitAxis) };
			return { unitAxis, projected / length };
		}

	}

	PhysicsJointComposition ComposePhysicsJoints(const Scene& scene, const PhysicsComposition& bodies)
	{
		PhysicsJointComposition composition;
		const BodyIndex index = Utils::IndexBodies(bodies);
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(entity);
			if (!fields.has_value() || !Utils::IsEffectivelyEnabled(entity))
				continue;

			PhysicsJointPlan plan;
			plan.Entity = id;
			plan.Type = fields->Type;
			const auto refuse = [&composition, &plan, entity](std::string_view field, std::string_view subject, std::string_view reason, std::string hint)
			{
				composition.Diagnostics.push_back(Utils::MakeJointDiagnostic(entity, plan.Type, field, subject, reason, std::move(hint)));
				plan.IsCreatable = false;
			};

			const ResolvedEnd body = Utils::ResolveEnd(entity, index, false, "joint");
			if (!body.IsResolved())
			{
				if (!body.Silent)
				{
					refuse("", body.Subject, body.Reason,
						"put the joint on the entity of a RigidBody with a collider, or on a child of it, outside any CharacterController or "
						"RigidBody2D");
				}
				plan.IsCreatable = false;
				composition.Joints.push_back(plan);
				continue;
			}
			plan.Body = body.Owner;

			if (fields->ConnectedBody.IsValid())
			{
				const ConstEntity connected = scene.FindEntityByID(fields->ConnectedBody);
				if (!connected.IsValid())
				{
					refuse("ConnectedBody", "connected", std::format("its ConnectedBody {} names no entity of the scene", fields->ConnectedBody),
						"set ConnectedBody to an entity of the scene, or null to join the body to the world");
					composition.Joints.push_back(plan);
					continue;
				}
				if (!Utils::IsEffectivelyEnabled(connected))
				{
					// It joins when its connected entity is enabled again, as a body comes back with its entity.
					plan.IsCreatable = false;
					composition.Joints.push_back(plan);
					continue;
				}
				const ResolvedEnd other = Utils::ResolveEnd(connected, index, true, "connected");
				if (!other.IsResolved())
				{
					if (!other.Silent)
					{
						refuse("ConnectedBody", other.Subject, other.Reason,
							"name an entity with a RigidBody at or above it, or one whose solid colliders form a static body, or null for the world");
					}
					plan.IsCreatable = false;
					composition.Joints.push_back(plan);
					continue;
				}
				plan.Connected = other.Owner;
				plan.ConnectedOrigin = other.Origin;
				if (plan.Connected == plan.Body && plan.ConnectedOrigin == PhysicsBodyOrigin::RigidBody)
				{
					refuse("ConnectedBody", "same", std::format("both of its ends are the body of {}", Utils::DescribeJointEntity(scene.FindEntityByID(plan.Body))),
						"name an entity of another body in ConnectedBody, or null for the world");
					composition.Joints.push_back(plan);
					continue;
				}
			}

			const bool hasAxis = fields->Type == PhysicsJointType::Hinge || fields->Type == PhysicsJointType::Slider;
			if (hasAxis && !(glm::length(fields->Axis) >= Utils::MinJointAxisLength))
			{
				refuse("Axis", "axis", "its Axis is zero, so it has no direction", "give Axis a direction, (1, 0, 0) for the entity's X axis");
			}
			else if (hasAxis && fields->Settings.UseLimits && fields->Settings.LowerLimit == fields->Settings.UpperLimit)
			{
				const bool hinge = fields->Type == PhysicsJointType::Hinge;
				refuse(hinge ? "UpperAngle" : "UpperLimit", "limits", "its limits are equal, so it cannot move",
					"use a FixedJoint for a joint that does not move, or separate the limits");
			}
			else if (fields->Type == PhysicsJointType::Distance && fields->Settings.LowerLimit > fields->Settings.UpperLimit)
			{
				refuse("MinDistance", "distance",
					std::format("its MinDistance {} is above its MaxDistance {}", fields->Settings.LowerLimit, fields->Settings.UpperLimit),
					"set MinDistance at or below MaxDistance");
			}
			composition.Joints.push_back(plan);
		}

		std::sort(composition.Diagnostics.begin(), composition.Diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return std::tie(a.Entity, a.Code, a.Subject) < std::tie(b.Entity, b.Code, b.Subject);
		});
		return composition;
	}

	std::string_view GetPhysicsJointComponentName(PhysicsJointType type)
	{
		switch (type)
		{
			case PhysicsJointType::Hinge:    return "HingeJoint";
			case PhysicsJointType::Slider:   return "SliderJoint";
			case PhysicsJointType::Distance: return "DistanceJoint";
			case PhysicsJointType::Fixed:    return "FixedJoint";
		}
		return "";
	}

	std::optional<PhysicsJointFields> ReadPhysicsJoint(ConstEntity entity)
	{
		// A runtime entity marked for destruction is still read (until the flush that destroys it).
		if (!entity.IsReadable())
			return std::nullopt;
		PhysicsJointFields fields;
		if (const HingeJointComponent* hinge = entity.TryGetComponent<HingeJointComponent>())
		{
			fields.Type = PhysicsJointType::Hinge;
			fields.ConnectedBody = hinge->ConnectedBody;
			fields.Anchor = hinge->Anchor;
			fields.Axis = hinge->Axis;
			fields.Settings.UseLimits = hinge->UseLimits;
			fields.Settings.LowerLimit = hinge->LowerAngle * JointDegreesToRadians;
			fields.Settings.UpperLimit = hinge->UpperAngle * JointDegreesToRadians;
			fields.Settings.MaxFriction = hinge->MaxFrictionTorque;
			fields.Settings.Motor = static_cast<PhysicsJointMotor>(std::to_underlying(hinge->Motor));
			fields.Settings.MotorTargetVelocity = hinge->MotorTargetVelocity * JointDegreesToRadians;
			fields.Settings.MotorTargetPosition = hinge->MotorTargetAngle * JointDegreesToRadians;
			fields.Settings.MotorMaxForce = hinge->MotorMaxTorque;
			fields.Settings.MotorFrequency = hinge->MotorFrequency;
			fields.Settings.MotorDamping = hinge->MotorDamping;
			fields.Settings.CollideConnected = hinge->CollideConnected;
			fields.BreakForce = hinge->BreakForce;
			fields.BreakTorque = hinge->BreakTorque;
			fields.Broken = hinge->Broken;
			return fields;
		}
		if (const SliderJointComponent* slider = entity.TryGetComponent<SliderJointComponent>())
		{
			fields.Type = PhysicsJointType::Slider;
			fields.ConnectedBody = slider->ConnectedBody;
			fields.Anchor = slider->Anchor;
			fields.Axis = slider->Axis;
			fields.Settings.UseLimits = slider->UseLimits;
			fields.Settings.LowerLimit = slider->LowerLimit;
			fields.Settings.UpperLimit = slider->UpperLimit;
			fields.Settings.MaxFriction = slider->MaxFrictionForce;
			fields.Settings.Motor = static_cast<PhysicsJointMotor>(std::to_underlying(slider->Motor));
			fields.Settings.MotorTargetVelocity = slider->MotorTargetVelocity;
			fields.Settings.MotorTargetPosition = slider->MotorTargetPosition;
			fields.Settings.MotorMaxForce = slider->MotorMaxForce;
			fields.Settings.MotorFrequency = slider->MotorFrequency;
			fields.Settings.MotorDamping = slider->MotorDamping;
			fields.Settings.CollideConnected = slider->CollideConnected;
			fields.BreakForce = slider->BreakForce;
			fields.BreakTorque = slider->BreakTorque;
			fields.Broken = slider->Broken;
			return fields;
		}
		if (const DistanceJointComponent* distance = entity.TryGetComponent<DistanceJointComponent>())
		{
			fields.Type = PhysicsJointType::Distance;
			fields.ConnectedBody = distance->ConnectedBody;
			fields.Anchor = distance->Anchor;
			fields.ConnectedAnchor = distance->ConnectedAnchor;
			fields.Settings.LowerLimit = distance->MinDistance;
			fields.Settings.UpperLimit = distance->MaxDistance;
			fields.Settings.LimitFrequency = distance->Frequency;
			fields.Settings.LimitDamping = distance->Damping;
			fields.Settings.CollideConnected = distance->CollideConnected;
			fields.BreakForce = distance->BreakForce;
			fields.Broken = distance->Broken;
			return fields;
		}
		if (const FixedJointComponent* fixed = entity.TryGetComponent<FixedJointComponent>())
		{
			fields.Type = PhysicsJointType::Fixed;
			fields.ConnectedBody = fixed->ConnectedBody;
			fields.Anchor = fixed->Anchor;
			fields.Settings.CollideConnected = fixed->CollideConnected;
			fields.BreakForce = fixed->BreakForce;
			fields.BreakTorque = fixed->BreakTorque;
			fields.Broken = fixed->Broken;
			return fields;
		}
		return std::nullopt;
	}

	void WritePhysicsJointBroken(Entity entity, bool broken)
	{
		if (HingeJointComponent* hinge = entity.TryGetComponent<HingeJointComponent>())
			hinge->Broken = broken;
		else if (SliderJointComponent* slider = entity.TryGetComponent<SliderJointComponent>())
			slider->Broken = broken;
		else if (DistanceJointComponent* distance = entity.TryGetComponent<DistanceJointComponent>())
			distance->Broken = broken;
		else if (FixedJointComponent* fixed = entity.TryGetComponent<FixedJointComponent>())
			fixed->Broken = broken;
		else
			ENGINE_CORE_ASSERT(false, "WritePhysicsJointBroken: entity {} holds no joint component", entity.GetUUID());
	}

	std::optional<PhysicsJointFrame> MakePhysicsJointWorldFrame(const glm::mat4& entityWorld, const PhysicsJointFields& fields)
	{
		PhysicsJointFrame frame;
		frame.Point = glm::vec3(entityWorld * glm::vec4(fields.Anchor, 1.0f));
		if (fields.Type == PhysicsJointType::Distance)
		{
			// A distance joint has no axis: only its point, whatever the entity's scale.
			if (!Utils::IsFiniteVector(frame.Point))
				return std::nullopt;
			frame.Axis = glm::vec3(1.0f, 0.0f, 0.0f);
			frame.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			return frame;
		}
		const glm::vec3 worldAxis = glm::mat3(entityWorld) * fields.Axis;
		const float length = glm::length(worldAxis);
		if (!(length >= Utils::MinJointAxisLength) || !Utils::IsFiniteVector(frame.Point) || !Utils::IsFiniteVector(worldAxis))
			return std::nullopt;
		frame.Axis = worldAxis / length;
		frame.Normal = Utils::MakePerpendicular(frame.Axis);
		if (!Utils::IsFiniteVector(frame.Axis) || !Utils::IsFiniteVector(frame.Normal))
			return std::nullopt;
		return frame;
	}

	PhysicsJointFrame ToPhysicsJointBodyFrame(const PhysicsPose& pose, const PhysicsJointFrame& frame)
	{
		const glm::quat inverse = glm::conjugate(pose.Rotation);
		PhysicsJointFrame local;
		local.Point = inverse * (frame.Point - pose.Position);
		std::tie(local.Axis, local.Normal) = Utils::Orthonormalize(inverse * frame.Axis, inverse * frame.Normal);
		return local;
	}

	PhysicsJointFrame ToPhysicsJointWorldFrame(const PhysicsPose& pose, const PhysicsJointFrame& frame)
	{
		PhysicsJointFrame world;
		world.Point = pose.Position + pose.Rotation * frame.Point;
		std::tie(world.Axis, world.Normal) = Utils::Orthonormalize(pose.Rotation * frame.Axis, pose.Rotation * frame.Normal);
		return world;
	}

}
