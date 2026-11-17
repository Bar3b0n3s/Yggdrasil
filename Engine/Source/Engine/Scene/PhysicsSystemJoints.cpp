#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SliderJointComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <utility>

// The play session's joints (M37, Docs/Decisions/0054-m37-joints.md; PhysicsSystem.h's M37 paragraph): the joint
// components' signals, the synchronization after the bodies, the kept frames, the break pass and its dispatch, the flush,
// the hash's joint part and the reads of scripts and physics.jointInfo. The hooks that call them are in PhysicsSystem.cpp.

namespace Engine {

	namespace Utils {

		template<typename Component, typename Instance>
		static void ConnectJointComponentSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().template connect<&Instance::OnJointAddedOrRemoved>(instance);
			registry.on_update<Component>().template connect<&Instance::OnJointChanged>(instance);
			registry.on_destroy<Component>().template connect<&Instance::OnJointAddedOrRemoved>(instance);
		}

		template<typename Component, typename Instance>
		static void DisconnectJointComponentSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().disconnect(&instance);
			registry.on_update<Component>().disconnect(&instance);
			registry.on_destroy<Component>().disconnect(&instance);
		}

		// The entity's world matrix: WorldTransformComponent, current in the step hooks (PlaySession runs TransformSystem::
		// Update before them), else computed from the parent chain.
		static glm::mat4 GetJointWorldMatrix(ConstEntity entity)
		{
			if (const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>())
				return world->Matrix;
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		// A body origin's world pose from its entity (the body poses' convention: translation and rotation, no scale).
		static PhysicsPose GetJointBodyPose(ConstEntity entity)
		{
			return PhysicsPose{ .Position = glm::vec3(GetJointWorldMatrix(entity)[3]), .Rotation = TransformSystem::GetWorldRotation(entity) };
		}

		static PhysicsJointKey MakeJointKey(const PhysicsJointFields& fields)
		{
			return PhysicsJointKey{ .Type = fields.Type,
				.ConnectedBody = fields.ConnectedBody,
				.Anchor = fields.Anchor,
				.Axis = fields.Axis,
				.ConnectedAnchor = fields.ConnectedAnchor,
				.UseLimits = fields.Settings.UseLimits,
				.LowerLimit = fields.Settings.LowerLimit,
				.UpperLimit = fields.Settings.UpperLimit };
		}

		// The joint's entity and its ancestors up to its body's owner, inclusive.
		static std::vector<UUID> GetJointChain(ConstEntity entity, UUID body)
		{
			std::vector<UUID> chain;
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				chain.push_back(current.GetUUID());
				if (current.GetUUID() == body)
					break;
			}
			return chain;
		}

		// §9.10: each joint in force solves at its entity's rank in UUID order among them (`joints` is ordered by UUID), so
		// the solver's order does not depend on the order the joints were made in.
		static void RankJoints(const std::map<UUID, PhysicsJointEntry>& joints, PhysicsWorld& world)
		{
			uint32_t rank = 0;
			for (const auto& [id, entry] : joints)
			{
				if (entry.Handle.IsValid())
					world.SetJointPriority(entry.Handle, rank++);
			}
		}

		static void AppendJointFloat(std::vector<std::byte>& bytes, float value)
		{
			const auto bits = std::bit_cast<uint32_t>(value);
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffu));
		}

		static void AppendJointFrame(std::vector<std::byte>& bytes, const PhysicsJointFrame& frame)
		{
			for (const glm::vec3& vector : { frame.Point, frame.Axis, frame.Normal })
			{
				AppendJointFloat(bytes, vector.x);
				AppendJointFloat(bytes, vector.y);
				AppendJointFloat(bytes, vector.z);
			}
		}

	}

	// --- Signals --------------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::ConnectJointSignals(entt::registry& registry)
	{
		Utils::ConnectJointComponentSignals<HingeJointComponent>(registry, *this);
		Utils::ConnectJointComponentSignals<SliderJointComponent>(registry, *this);
		Utils::ConnectJointComponentSignals<DistanceJointComponent>(registry, *this);
		Utils::ConnectJointComponentSignals<FixedJointComponent>(registry, *this);
	}

	void PhysicsSystem::State::DisconnectJointSignals(entt::registry& registry)
	{
		Utils::DisconnectJointComponentSignals<HingeJointComponent>(registry, *this);
		Utils::DisconnectJointComponentSignals<SliderJointComponent>(registry, *this);
		Utils::DisconnectJointComponentSignals<DistanceJointComponent>(registry, *this);
		Utils::DisconnectJointComponentSignals<FixedJointComponent>(registry, *this);
	}

	void PhysicsSystem::State::OnJointChanged(entt::registry& registry, entt::entity entity)
	{
		JointsChanged = true;
		if (const IDComponent* id = registry.try_get<IDComponent>(entity))
			ChangedJoints.insert(id->ID);
	}

	void PhysicsSystem::State::OnJointAddedOrRemoved(entt::registry& registry, entt::entity entity)
	{
		OnJointChanged(registry, entity);
		JointPlansStale = true;
	}

	// --- Synchronization ------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::SynchronizeJoints()
	{
		if (!JointsChanged)
			return;
		JointsChanged = false;
		const Scene& scene = *Specification.RuntimeScene;

		// The plan is composed again when the bodies or the set of joints changed, or a changed joint's key moved; a change
		// of anything else (a motor target, a threshold, Broken) uses the plan it has.
		bool replan = JointPlansStale;
		for (const UUID changed : ChangedJoints)
		{
			if (replan)
				break;
			const auto entry = Joints.find(changed);
			const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(scene.FindEntityByID(changed));
			replan = entry == Joints.end() || !fields.has_value() || Utils::MakeJointKey(*fields) != entry->second.PlannedKey;
		}
		ChangedJoints.clear();
		JointPlansStale = false;

		std::set<UUID> planned;
		if (replan)
		{
			PhysicsJointComposition composition = ComposePhysicsJoints(scene, Composition);
			for (PhysicsDiagnostic& diagnostic : composition.Diagnostics)
				RaiseDiagnostic(std::move(diagnostic));
			for (const PhysicsJointPlan& plan : composition.Joints)
			{
				PhysicsJointEntry& entry = Joints[plan.Entity];
				entry.Entity = plan.Entity;
				entry.Plan = plan;
				if (const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(scene.FindEntityByID(plan.Entity)))
					entry.PlannedKey = Utils::MakeJointKey(*fields);
				planned.insert(plan.Entity);
			}
		}

		bool inForceChanged = false;
		const auto stop = [this, &inForceChanged](PhysicsJointEntry& entry)
		{
			if (!entry.Handle.IsValid())
				return;
			World->DestroyJoint(entry.Handle);
			entry.Handle = JointHandle();
			inForceChanged = true;
		};

		for (auto joint = Joints.begin(); joint != Joints.end();)
		{
			PhysicsJointEntry& entry = joint->second;
			const UUID id = joint->first;
			const ConstEntity readable = scene.FindReadableEntityByID(id);
			const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(readable);
			// A joint component that is gone, on an entity that is gone or stays: the joint and its frames go.
			if (!readable.IsReadable() || !fields.has_value())
			{
				stop(entry);
				joint = Joints.erase(joint);
				continue;
			}
			++joint;
			// A joint whose entity is leaving play keeps what it has until FlushDestroyed; one the last plan did not list
			// (its entity disabled) has nothing in force.
			if (!IsPresent(id))
				continue;
			if (replan && !planned.contains(id))
			{
				stop(entry);
				continue;
			}

			const PhysicsJointPlan& plan = entry.Plan;
			const ConstEntity entity = scene.FindEntityByID(id);
			// Frames formed from what no longer holds are formed again (PhysicsJointFormation).
			if (entry.HasFrames && plan.IsCreatable)
			{
				const PhysicsJointFormation formation{ .Type = plan.Type,
					.Body = plan.Body,
					.Connected = plan.Connected,
					.ConnectedOrigin = plan.ConnectedOrigin,
					.ConnectedBody = fields->ConnectedBody,
					.Anchor = fields->Anchor,
					.Axis = fields->Axis,
					.Chain = Utils::GetJointChain(entity, plan.Body) };
				if (formation != entry.Formation)
				{
					stop(entry);
					entry.HasFrames = false;
				}
			}
			if (!plan.IsCreatable || fields->Broken || fields->Type != plan.Type)
			{
				stop(entry);
				continue;
			}

			// Both ends need their bodies; an end that is leaving play keeps the joint it has until FlushDestroyed.
			const PhysicsBodyEntry* body = FindOwnedBody(plan.Body, PhysicsBodyOrigin::RigidBody);
			const PhysicsBodyEntry* connected = plan.Connected.IsValid() ? FindOwnedBody(plan.Connected, plan.ConnectedOrigin) : nullptr;
			if (body == nullptr || (plan.Connected.IsValid() && connected == nullptr))
			{
				stop(entry);
				continue;
			}
			if (!IsPresent(plan.Body) || (plan.Connected.IsValid() && !IsPresent(plan.Connected)))
				continue;
			const BodyHandle connectedHandle = connected != nullptr ? connected->Handle : BodyHandle();

			// In force with the same bodies: its settings change in place.
			if (entry.Handle.IsValid() && World->IsJointValid(entry.Handle) && entry.Body == body->Handle && entry.Connected == connectedHandle)
			{
				if (entry.Settings == fields->Settings)
					continue;
				if (const Status changed = World->SetJointSettings(entry.Handle, fields->Settings); !changed)
				{
					stop(entry);
					RaiseDiagnostic(PhysicsDiagnostic{ .Code = std::string(PhysicsJointInvalidCode),
						.Severity = DiagnosticSeverity::Error,
						.Entity = id,
						.Component = std::string(GetPhysicsJointComponentName(plan.Type)),
						.Message = std::format("the {} of '{}' ({}) cannot take its settings: {}; the joint is not in force", GetPhysicsJointComponentName(plan.Type),
							entity.GetName(), id, changed.error().GetMessageText()),
						.Hint = "correct the joint's limits, motor or spring",
						.Subject = "refused" });
					continue;
				}
				entry.Settings = fields->Settings;
				continue;
			}

			// Created, or created again after a body of it was: from the kept frames, formed now when there are none.
			stop(entry);
			const std::optional<PhysicsJointFrame> world = MakePhysicsJointWorldFrame(Utils::GetJointWorldMatrix(entity), *fields);
			PhysicsJointFrame frame;
			PhysicsJointFrame connectedFrame;
			const PhysicsPose bodyPose = Utils::GetJointBodyPose(scene.FindEntityByID(plan.Body));
			const PhysicsPose connectedPose = plan.Connected.IsValid() ? Utils::GetJointBodyPose(scene.FindEntityByID(plan.Connected)) : PhysicsPose{};
			if (!world.has_value())
			{
				RaiseDiagnostic(PhysicsDiagnostic{ .Code = std::string(PhysicsJointInvalidCode),
					.Severity = DiagnosticSeverity::Error,
					.Entity = id,
					.Component = std::string(GetPhysicsJointComponentName(plan.Type)),
					.Message = std::format("the {} of '{}' ({}) cannot be made: its axis or anchor vanishes or is not finite under its entity's "
										   "transform; the joint is not created",
						GetPhysicsJointComponentName(plan.Type), entity.GetName(), id),
					.Hint = "give the joint's entity a scale that keeps the axis, and a finite anchor",
					.Subject = "frame" });
				continue;
			}
			if (plan.Type == PhysicsJointType::Distance)
			{
				// Both anchors are read from the scene at every creation; nothing is kept.
				frame = ToPhysicsJointBodyFrame(bodyPose, *world);
				const ConstEntity connectedEntity = fields->ConnectedBody.IsValid() ? scene.FindEntityByID(fields->ConnectedBody) : ConstEntity();
				const glm::mat4 connectedWorld = connectedEntity.IsValid() ? Utils::GetJointWorldMatrix(connectedEntity) : glm::mat4(1.0f);
				PhysicsJointFrame other = *world;
				other.Point = glm::vec3(connectedWorld * glm::vec4(fields->ConnectedAnchor, 1.0f));
				connectedFrame = ToPhysicsJointBodyFrame(connectedPose, other);
			}
			else
			{
				if (!entry.HasFrames)
				{
					entry.Frame = ToPhysicsJointBodyFrame(bodyPose, *world);
					entry.ConnectedFrame = ToPhysicsJointBodyFrame(connectedPose, *world);
					entry.Formation = PhysicsJointFormation{ .Type = plan.Type,
						.Body = plan.Body,
						.Connected = plan.Connected,
						.ConnectedOrigin = plan.ConnectedOrigin,
						.ConnectedBody = fields->ConnectedBody,
						.Anchor = fields->Anchor,
						.Axis = fields->Axis,
						.Chain = Utils::GetJointChain(entity, plan.Body) };
					entry.HasFrames = true;
				}
				frame = entry.Frame;
				connectedFrame = entry.ConnectedFrame;
			}

			PhysicsJointDescription description;
			description.Type = plan.Type;
			description.Body = body->Handle;
			description.ConnectedBody = connectedHandle;
			description.Frame = frame;
			description.ConnectedFrame = connectedFrame;
			description.Settings = fields->Settings;
			const Result<JointHandle> created = World->CreateJoint(description);
			if (!created)
			{
				RaiseDiagnostic(PhysicsDiagnostic{ .Code = std::string(PhysicsJointInvalidCode),
					.Severity = DiagnosticSeverity::Error,
					.Entity = id,
					.Component = std::string(GetPhysicsJointComponentName(plan.Type)),
					.Message = std::format("the {} of '{}' ({}) cannot be made: {}; the joint is not created", GetPhysicsJointComponentName(plan.Type),
						entity.GetName(), id, created.error().GetMessageText()),
					.Hint = "correct the joint's anchors, axis, limits, motor or spring",
					.Subject = "refused" });
				continue;
			}
			entry.Handle = *created;
			entry.Body = body->Handle;
			entry.Connected = connectedHandle;
			entry.Settings = fields->Settings;
			inForceChanged = true;
		}

		if (inForceChanged)
			Utils::RankJoints(Joints, *World);
	}

	void PhysicsSystem::State::RemoveBodyJoints(BodyHandle handle)
	{
		for (auto& [id, entry] : Joints)
		{
			if (!entry.Handle.IsValid() || (entry.Body != handle && entry.Connected != handle))
				continue;
			World->DestroyJoint(entry.Handle);
			entry.Handle = JointHandle();
		}
		JointsChanged = true;
	}

	void PhysicsSystem::State::DestroyAllJoints()
	{
		for (auto& [id, entry] : Joints)
		{
			if (entry.Handle.IsValid())
				World->DestroyJoint(entry.Handle);
			entry.Handle = JointHandle();
		}
	}

	// --- Breaking -------------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::BreakJoints(uint64_t tick)
	{
		LastJointBreaks.clear();
		Scene& scene = *Specification.RuntimeScene;
		bool broke = false;
		for (auto& [id, entry] : Joints)
		{
			// A joint whose entity is leaving play goes at the flush, unbroken: its component cannot be written any more.
			if (!entry.Handle.IsValid() || !IsPresent(id))
				continue;
			const Entity entity = scene.FindEntityByID(id);
			const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(entity);
			if (!fields.has_value() || (fields->BreakForce <= 0.0f && fields->BreakTorque <= 0.0f))
				continue;
			const PhysicsJointState reading = World->GetJointState(entry.Handle);
			const bool overForce = fields->BreakForce > 0.0f && reading.Force > fields->BreakForce;
			const bool overTorque = fields->BreakTorque > 0.0f && reading.Torque > fields->BreakTorque;
			if (!overForce && !overTorque)
				continue;
			World->DestroyJoint(entry.Handle);
			entry.Handle = JointHandle();
			WritePhysicsJointBroken(entity, true);
			LastJointBreaks.push_back(PhysicsJointBreak{ .Joint = id, .Force = reading.Force, .Torque = reading.Torque, .Tick = tick });
			broke = true;
		}
		if (broke)
			Utils::RankJoints(Joints, *World);
	}

	void PhysicsSystem::State::DispatchJointBreaks()
	{
		if (Listener == nullptr || LastJointBreaks.empty())
			return;
		Dispatching = true;
		for (const PhysicsJointBreak& jointBreak : LastJointBreaks)
		{
			// The drop rule of OnPhysicsEvent: a joint whose entity an earlier callback destroyed or disabled hears nothing.
			if (IsPresent(jointBreak.Joint))
				Listener->OnPhysicsJointBreak(jointBreak);
		}
		Dispatching = false;
	}

	// --- Flush and hash -------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::RemoveLeavingJoints(const std::set<UUID>& candidates)
	{
		const Scene& scene = *Specification.RuntimeScene;
		for (const UUID candidate : candidates)
		{
			const auto found = Joints.find(candidate);
			if (found == Joints.end() || IsPresent(candidate))
				continue;
			PhysicsJointEntry& entry = found->second;
			if (entry.Handle.IsValid())
				World->DestroyJoint(entry.Handle);
			entry.Handle = JointHandle();
			JointsChanged = true;
			// A destroyed entity's joint goes with its frames; a disabled one keeps its frames for its return.
			if (!scene.FindEntityByID(candidate).IsValid())
				Joints.erase(found);
		}
	}

	bool PhysicsSystem::State::HasKeptJointFrames() const
	{
		return std::any_of(Joints.begin(), Joints.end(), [](const auto& joint)
		{
			return joint.second.HasFrames;
		});
	}

	void PhysicsSystem::State::AppendJointHash(std::vector<std::byte>& bytes) const
	{
		if (!HasKeptJointFrames())
			return;
		for (const UUID id : Specification.RuntimeScene->GetCanonicalOrder())
		{
			const auto found = Joints.find(id);
			if (found == Joints.end() || !found->second.HasFrames)
				continue;
			const uint64_t value = id.GetValue();
			for (uint32_t shift = 0; shift < 64; shift += 8)
				bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
			Utils::AppendJointFrame(bytes, found->second.Frame);
			Utils::AppendJointFrame(bytes, found->second.ConnectedFrame);
		}
	}

	// --- Reads ----------------------------------------------------------------------------------------------------------

	std::optional<PhysicsJointReport> PhysicsSystem::GetJointInfo(UUID entity) const
	{
		const State& state = *m_State;
		const ConstEntity found = std::as_const(*state.Specification.RuntimeScene).FindEntityByID(entity);
		const std::optional<PhysicsJointFields> fields = ReadPhysicsJoint(found);
		if (!fields.has_value())
			return std::nullopt;

		PhysicsJointReport report;
		report.Entity = entity;
		report.Type = fields->Type;
		report.Broken = fields->Broken;
		report.Motor = fields->Type == PhysicsJointType::Hinge || fields->Type == PhysicsJointType::Slider ? fields->Settings.Motor : PhysicsJointMotor::Off;
		const auto joint = state.Joints.find(entity);
		if (joint == state.Joints.end())
			return report;
		const PhysicsJointEntry& entry = joint->second;
		report.Body = entry.Plan.Body;
		report.Connected = entry.Plan.Connected;
		report.Created = entry.Handle.IsValid() && state.World->IsJointValid(entry.Handle) && state.IsPresent(entity);
		if (!report.Created)
			return report;
		const PhysicsJointState reading = state.World->GetJointState(entry.Handle);
		switch (fields->Type)
		{
			case PhysicsJointType::Hinge:    report.Angle = reading.Value * JointRadiansToDegrees; break;
			case PhysicsJointType::Slider:   report.Position = reading.Value; break;
			case PhysicsJointType::Distance: report.Distance = reading.Value; break;
			case PhysicsJointType::Fixed:    break;
		}
		report.Force = reading.Force;
		report.Torque = reading.Torque;
		report.Point = reading.Point;
		report.ConnectedPoint = reading.ConnectedPoint;
		report.Axis = reading.Axis;
		return report;
	}

	std::span<const PhysicsJointBreak> PhysicsSystem::GetLastJointBreaks() const
	{
		return m_State->LastJointBreaks;
	}

}
