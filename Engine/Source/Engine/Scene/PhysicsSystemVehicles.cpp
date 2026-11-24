#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

// The play session's vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; PhysicsSystem.h's M44 paragraph):
// the vehicle components' signals, the synchronization after the bodies and joints, the kept wheel frames, the driver input
// of PreStep's item 4b, the wheel poses of PostStep, the flush, the constraint priorities, the hash's vehicle part and the
// reads of scripts and physics.vehicleInfo. The hooks that call them, and the wheel poses' writes, are in PhysicsSystem.cpp.

namespace Engine {

	namespace Utils {

		template<typename Component, typename Instance>
		static void ConnectVehicleComponentSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().template connect<&Instance::OnVehicleAddedOrRemoved>(instance);
			registry.on_update<Component>().template connect<&Instance::OnVehicleChanged>(instance);
			registry.on_destroy<Component>().template connect<&Instance::OnVehicleAddedOrRemoved>(instance);
		}

		template<typename Component, typename Instance>
		static void DisconnectVehicleComponentSignals(entt::registry& registry, Instance& instance)
		{
			registry.on_construct<Component>().disconnect(&instance);
			registry.on_update<Component>().disconnect(&instance);
			registry.on_destroy<Component>().disconnect(&instance);
		}

		// Whether `a` and `b` have anti-roll bars between the same wheels, in the same order (their stiffness aside).
		static bool SameBarPairs(const PhysicsVehicleSettings& a, const PhysicsVehicleSettings& b)
		{
			return std::equal(a.AntiRollBars.begin(), a.AntiRollBars.end(), b.AntiRollBars.begin(), b.AntiRollBars.end(),
				[](const PhysicsVehicleAntiRollBar& left, const PhysicsVehicleAntiRollBar& right)
			{
				return left.LeftWheel == right.LeftWheel && left.RightWheel == right.RightWheel;
			});
		}

		static void AppendVehicleFloat(std::vector<std::byte>& bytes, float value)
		{
			const auto bits = std::bit_cast<uint32_t>(value);
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffu));
		}

		static void AppendVehicleVector(std::vector<std::byte>& bytes, const glm::vec3& value)
		{
			AppendVehicleFloat(bytes, value.x);
			AppendVehicleFloat(bytes, value.y);
			AppendVehicleFloat(bytes, value.z);
		}

		static void AppendVehicleQuat(std::vector<std::byte>& bytes, const glm::quat& value)
		{
			AppendVehicleFloat(bytes, value.x);
			AppendVehicleFloat(bytes, value.y);
			AppendVehicleFloat(bytes, value.z);
			AppendVehicleFloat(bytes, value.w);
		}

		static void AppendVehicleUUID(std::vector<std::byte>& bytes, UUID id)
		{
			const uint64_t value = id.GetValue();
			for (uint32_t shift = 0; shift < 64; shift += 8)
				bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
		}

		// The motion `carried` had on the wheels `carriedWheels`, mapped onto the wheels `wheels` by entity (a wheel it did not
		// have starts still).
		static PhysicsVehicleState MapCarriedMotion(const PhysicsVehicleState& carried, std::span<const UUID> carriedWheels, std::span<const UUID> wheels)
		{
			PhysicsVehicleState mapped;
			mapped.EngineRpm = carried.EngineRpm;
			mapped.Gear = carried.Gear;
			mapped.ClutchFriction = carried.ClutchFriction;
			mapped.Wheels.resize(wheels.size());
			for (size_t index = 0; index < wheels.size(); ++index)
			{
				const auto found = std::find(carriedWheels.begin(), carriedWheels.end(), wheels[index]);
				const auto position = static_cast<size_t>(found - carriedWheels.begin());
				if (found == carriedWheels.end() || position >= carried.Wheels.size())
					continue;
				mapped.Wheels[index].AngularVelocity = carried.Wheels[position].AngularVelocity;
				mapped.Wheels[index].Angle = carried.Wheels[position].Angle;
			}
			return mapped;
		}

	}

	// --- Signals --------------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::ConnectVehicleSignals(entt::registry& registry)
	{
		Utils::ConnectVehicleComponentSignals<WheeledVehicleComponent>(registry, *this);
		Utils::ConnectVehicleComponentSignals<VehicleWheelComponent>(registry, *this);
	}

	void PhysicsSystem::State::DisconnectVehicleSignals(entt::registry& registry)
	{
		Utils::DisconnectVehicleComponentSignals<WheeledVehicleComponent>(registry, *this);
		Utils::DisconnectVehicleComponentSignals<VehicleWheelComponent>(registry, *this);
	}

	void PhysicsSystem::State::OnVehicleChanged(entt::registry& registry, entt::entity entity)
	{
		VehiclesChanged = true;
		if (const IDComponent* id = registry.try_get<IDComponent>(entity))
			ChangedVehicles.insert(id->ID);
	}

	void PhysicsSystem::State::OnVehicleAddedOrRemoved(entt::registry& registry, entt::entity entity)
	{
		OnVehicleChanged(registry, entity);
		VehiclePlansStale = true;
	}

	// --- Synchronization ------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::SynchronizeVehicles()
	{
		if (!VehiclesChanged)
			return;
		VehiclesChanged = false;
		const Scene& scene = *Specification.RuntimeScene;

		// The plan is composed again when the bodies or the set of vehicle components changed, a wheel changed, or a changed
		// vehicle's fields beyond its driver input differ from what the last plan read. A write of driver input alone stops
		// here: PreStep's item 4b applies it.
		bool replan = VehiclePlansStale;
		for (const UUID changed : ChangedVehicles)
		{
			if (replan)
				break;
			const auto entry = Vehicles.find(changed);
			const ConstEntity changedEntity = scene.FindEntityByID(changed);
			const WheeledVehicleComponent* vehicle = changedEntity.IsValid() ? changedEntity.TryGetComponent<WheeledVehicleComponent>() : nullptr;
			replan = entry == Vehicles.end() || vehicle == nullptr || StripPhysicsVehicleInput(*vehicle) != entry->second.PlannedVehicle;
		}
		ChangedVehicles.clear();
		if (!replan && !VehicleBodiesChanged)
			return;
		VehiclePlansStale = false;
		VehicleBodiesChanged = false;

		// An entity leaving play: disabled or destroyed since the last FlushDestroyed, which removes what it held.
		const auto isLeaving = [this](UUID entity)
		{
			return FlushCandidates.contains(entity);
		};
		// A vehicle in force with a wheel leaving play keeps what it has until FlushDestroyed, which composes the plans again
		// (RemoveLeavingVehicles): a reason it has then is raised after the flush.
		const auto keepsUntilFlush = [&isLeaving](const PhysicsVehicleEntry& entry)
		{
			return entry.Handle.IsValid() && std::any_of(entry.WheelEntities.begin(), entry.WheelEntities.end(), isLeaving);
		};

		std::set<UUID> planned;
		if (replan)
		{
			PhysicsVehicleComposition composition = ComposePhysicsVehicles(scene, Composition);
			for (PhysicsDiagnostic& diagnostic : composition.Diagnostics)
			{
				if (const auto entry = Vehicles.find(diagnostic.Entity); entry != Vehicles.end() && keepsUntilFlush(entry->second))
					continue;
				RaiseDiagnostic(std::move(diagnostic));
			}
			for (PhysicsVehiclePlan& plan : composition.Vehicles)
			{
				PhysicsVehicleEntry& entry = Vehicles[plan.Entity];
				entry.Entity = plan.Entity;
				entry.PlannedVehicle = StripPhysicsVehicleInput(scene.FindEntityByID(plan.Entity).GetComponent<WheeledVehicleComponent>());
				planned.insert(plan.Entity);
				entry.Plan = std::move(plan);
			}
		}

		// The vehicle a wheel entity belongs to by the hierarchy: the nearest WheeledVehicle at or above it, whether or not it
		// holds a VehicleWheel now; invalid without one.
		const auto ownerOf = [](ConstEntity wheel)
		{
			for (ConstEntity current = wheel; current.IsReadable(); current = current.GetParent())
			{
				if (current.HasComponent<WheeledVehicleComponent>())
					return current.GetUUID();
			}
			return UUID();
		};

		bool inForceChanged = false;
		const auto stop = [this, &inForceChanged](PhysicsVehicleEntry& entry)
		{
			if (!entry.Handle.IsValid())
				return;
			World->DestroyVehicle(entry.Handle);
			entry.Handle = VehicleHandle();
			entry.LastInput.reset();
			inForceChanged = true;
		};
		const auto refuse = [this](UUID id, ConstEntity entity, std::string_view reason)
		{
			RaiseDiagnostic(PhysicsDiagnostic{ .Code = std::string(PhysicsVehicleInvalidCode),
				.Severity = DiagnosticSeverity::Error,
				.Entity = id,
				.Component = "WheeledVehicle",
				.Message = std::format("the WheeledVehicle of '{}' ({}) cannot be made: {}; the vehicle is not created", entity.GetName(), id, reason),
				.Hint = "correct the vehicle's or its wheels' fields, or the wheels' transforms",
				.Subject = "refused" });
		};

		for (auto vehicle = Vehicles.begin(); vehicle != Vehicles.end();)
		{
			PhysicsVehicleEntry& entry = vehicle->second;
			const UUID id = vehicle->first;
			const ConstEntity readable = scene.FindReadableEntityByID(id);
			// An entity that is gone: the vehicle and its frames go.
			if (!readable.IsReadable())
			{
				stop(entry);
				vehicle = Vehicles.erase(vehicle);
				continue;
			}
			++vehicle;
			// A kept frame lasts while its wheel belongs to this vehicle (the W44.2 review's finding 1: never formed again from a
			// Transform the engine wrote): it goes only when the wheel's entity is gone or another vehicle holds it now.
			std::erase_if(entry.Frames, [&scene, &ownerOf, id](const auto& kept)
			{
				const ConstEntity wheel = scene.FindReadableEntityByID(kept.first);
				if (!wheel.IsReadable())
					return true;
				const UUID owner = ownerOf(wheel);
				return owner.IsValid() && owner != id;
			});
			// A WheeledVehicle removed: the vehicle is not in force, and its entry keeps its frames for the component's return.
			if (!readable.HasComponent<WheeledVehicleComponent>())
			{
				stop(entry);
				continue;
			}
			// A vehicle whose entity is leaving play keeps what it has until FlushDestroyed; one the last plan did not list (its
			// entity disabled) has nothing in force.
			if (!IsPresent(id))
				continue;
			if (replan && !planned.contains(id))
			{
				stop(entry);
				continue;
			}
			if (keepsUntilFlush(entry))
				continue;

			const PhysicsVehiclePlan& plan = entry.Plan;
			const ConstEntity entity = scene.FindEntityByID(id);

			const PhysicsBodyEntry* body = plan.IsCreatable ? FindOwnedBody(plan.Body, PhysicsBodyOrigin::RigidBody) : nullptr;
			if (body == nullptr)
			{
				stop(entry);
				continue;
			}

			// What it is made of now: each wheel at its kept frame, or, for a wheel the session never placed for this vehicle, at a
			// frame formed now from the wheel's authored pose (its Transform's, or the one it had before the engine first placed
			// it for another vehicle) under its current chain.
			const WheeledVehicleComponent& component = entity.GetComponent<WheeledVehicleComponent>();
			std::vector<PhysicsVehicleWheelDescription> wheels;
			std::map<UUID, PhysicsVehicleWheelFrame> formed;
			bool framed = true;
			for (const UUID wheelId : plan.Wheels)
			{
				const ConstEntity wheel = scene.FindEntityByID(wheelId);
				const auto kept = entry.Frames.find(wheelId);
				const auto authored = AuthoredWheelPoses.find(wheelId);
				const std::optional<PhysicsVehicleWheelFrame> made = kept != entry.Frames.end() ? std::optional(kept->second)
					: authored != AuthoredWheelPoses.end()                                      ? MakePhysicsVehicleWheelFrame(entity, wheel, authored->second.Translation, authored->second.Rotation)
																								: MakePhysicsVehicleWheelFrame(entity, wheel);
				PhysicsVehicleWheelFrame frame;
				if (made.has_value())
				{
					frame = *made;
					if (kept == entry.Frames.end())
						formed.emplace(wheelId, frame);
				}
				else
				{
					refuse(id, entity, std::format("the transform of its wheel '{}' ({}) does not decompose", wheel.GetName(), wheelId));
					framed = false;
					break;
				}
				wheels.push_back(MakePhysicsVehicleWheel(wheel.GetComponent<VehicleWheelComponent>(), frame));
			}
			if (!framed)
			{
				stop(entry);
				continue;
			}
			const PhysicsVehicleSettings settings = MakePhysicsVehicleSettings(component, plan.Wheels);
			const PhysicsVehicleCast cast = MakePhysicsVehicleCast(component);

			// In force on the same body with the same wheels, cast and anti-roll bar pairs: its settings change in place, keeping
			// its motion (a bar's stiffness included; Jolt keeps a wheel's last anti-roll impulse while no bar covers it, so a bar
			// added, removed or moved makes the vehicle again).
			if (entry.Handle.IsValid() && World->IsVehicleValid(entry.Handle) && entry.Body == body->Handle && entry.WheelEntities == plan.Wheels
				&& entry.Wheels == wheels && entry.Cast == cast && Utils::SameBarPairs(entry.Settings, settings))
			{
				if (entry.Settings == settings)
					continue;
				if (const Status changed = World->SetVehicleSettings(entry.Handle, settings); !changed)
				{
					stop(entry);
					refuse(id, entity, changed.error().GetMessageText());
					continue;
				}
				entry.Settings = settings;
				continue;
			}

			// Created, or made again (a wheel's field, the wheel set or the cast changed, or its body was made again): the motion of
			// the vehicle it replaces is carried over.
			if (entry.Handle.IsValid() && World->IsVehicleValid(entry.Handle))
			{
				entry.Carried = World->GetVehicleState(entry.Handle);
				entry.CarriedWheels = entry.WheelEntities;
			}
			stop(entry);
			PhysicsVehicleDescription description;
			description.Body = body->Handle;
			description.Wheels = wheels;
			description.Settings = settings;
			description.Cast = cast;
			const Result<VehicleHandle> created = World->CreateVehicle(description);
			if (!created)
			{
				refuse(id, entity, created.error().GetMessageText());
				continue;
			}
			entry.Handle = *created;
			entry.Body = body->Handle;
			entry.WheelEntities = plan.Wheels;
			entry.Wheels = std::move(wheels);
			entry.Settings = settings;
			entry.Cast = cast;
			entry.LastInput.reset();
			for (const auto& [wheelId, frame] : formed)
			{
				entry.Frames.insert_or_assign(wheelId, frame);
				// The pose the frame came from, kept before the engine first writes the wheel's Transform.
				const TransformComponent& transform = scene.FindEntityByID(wheelId).GetComponent<TransformComponent>();
				AuthoredWheelPoses.try_emplace(wheelId, PhysicsWheelAuthoredPose{ .Translation = transform.Translation, .Rotation = transform.Rotation });
			}
			if (entry.Carried.has_value())
				World->RestoreVehicleMotion(entry.Handle, Utils::MapCarriedMotion(*entry.Carried, entry.CarriedWheels, entry.WheelEntities));
			inForceChanged = true;
		}

		// Carried motion lives for one pass: a body that did not come back in it starts afresh, as its body does.
		for (auto& [id, entry] : Vehicles)
		{
			entry.Carried.reset();
			entry.CarriedWheels.clear();
		}
		if (inForceChanged)
			RankConstraints();
	}

	void PhysicsSystem::State::SynchronizeVehiclesAfterRebuild()
	{
		VehiclePlansStale = true;
		VehiclesChanged = true;
		SynchronizeVehicles();
	}

	void PhysicsSystem::State::RemoveBodyVehicles(BodyHandle handle)
	{
		for (auto& [id, entry] : Vehicles)
		{
			if (!entry.Handle.IsValid() || entry.Body != handle)
				continue;
			entry.Carried = World->GetVehicleState(entry.Handle);
			entry.CarriedWheels = entry.WheelEntities;
			World->DestroyVehicle(entry.Handle);
			entry.Handle = VehicleHandle();
			entry.LastInput.reset();
		}
		VehiclesChanged = true;
		VehicleBodiesChanged = true;
	}

	void PhysicsSystem::State::DestroyAllVehicles()
	{
		for (auto& [id, entry] : Vehicles)
		{
			if (entry.Handle.IsValid())
				World->DestroyVehicle(entry.Handle);
			entry.Handle = VehicleHandle();
		}
	}

	void PhysicsSystem::State::RankConstraints()
	{
		uint32_t rank = 0;
		for (const auto& [id, entry] : Joints)
		{
			if (entry.Handle.IsValid())
				World->SetJointPriority(entry.Handle, rank++);
		}
		for (const auto& [id, entry] : Vehicles)
		{
			if (entry.Handle.IsValid())
				World->SetVehiclePriority(entry.Handle, rank++);
		}
	}

	// --- The step -------------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::ApplyVehicleInput(std::span<const UUID> order)
	{
		if (Vehicles.empty())
			return;
		const Scene& scene = *Specification.RuntimeScene;
		for (const UUID id : order)
		{
			const auto found = Vehicles.find(id);
			if (found == Vehicles.end() || !found->second.Handle.IsValid() || !World->IsVehicleValid(found->second.Handle) || !IsPresent(id))
				continue;
			PhysicsVehicleEntry& entry = found->second;
			const PhysicsVehicleInput input = MakePhysicsVehicleInput(scene.FindEntityByID(id).GetComponent<WheeledVehicleComponent>());
			World->SetVehicleInput(entry.Handle, input);
			// Jolt keeps a vehicle awake only while its wheels spin: a sleeping one wakes for a throttle, or for any new input.
			const bool wake = input.Forward != 0.0f || !entry.LastInput.has_value() || *entry.LastInput != input;
			if (wake && World->IsSleeping(entry.Body))
				World->WakeUp(entry.Body);
			entry.LastInput = input;
		}
	}

	std::vector<PhysicsWheelPose> PhysicsSystem::State::CollectWheelPoses() const
	{
		std::vector<PhysicsWheelPose> poses;
		if (Vehicles.empty())
			return poses;
		const Scene& scene = *Specification.RuntimeScene;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const auto found = Vehicles.find(id);
			if (found == Vehicles.end() || !found->second.Handle.IsValid() || !IsPresent(id))
				continue;
			const PhysicsVehicleEntry& entry = found->second;
			const ConstEntity vehicle = scene.FindEntityByID(id);
			const PhysicsVehicleState reading = World->GetVehicleState(entry.Handle);
			for (size_t index = 0; index < entry.WheelEntities.size() && index < reading.Wheels.size(); ++index)
			{
				const UUID wheelId = entry.WheelEntities[index];
				if (!IsPresent(wheelId))
					continue;
				const ConstEntity wheel = scene.FindEntityByID(wheelId);
				// The wheel's pose in the body's space, into its parent's space through the chain of local matrices from the
				// vehicle's entity (its scale included), so the wheel is placed where the body is whatever the world matrices.
				const glm::mat4 parent = GetPhysicsVehicleChainMatrix(vehicle, wheel.GetParent());
				const Result<TransformDecomposition> local = TransformSystem::DecomposeMatrix(glm::affineInverse(parent) * reading.Wheels[index].LocalPose);
				if (!local.has_value())
					continue;
				poses.push_back(PhysicsWheelPose{ .Entity = wheelId, .Translation = local->Translation, .Rotation = local->Rotation });
			}
		}
		return poses;
	}

	// --- Flush and hash -------------------------------------------------------------------------------------------------

	void PhysicsSystem::State::RemoveLeavingVehicles(const std::set<UUID>& candidates)
	{
		if (candidates.empty() || (Vehicles.empty() && AuthoredWheelPoses.empty()))
			return;
		const Scene& scene = *Specification.RuntimeScene;
		// What left play may have been a vehicle's wheel, kept until now: the plans are composed again at the next
		// synchronization, which makes the vehicle again without it and raises a reason deferred while it was leaving.
		VehiclePlansStale = true;
		VehiclesChanged = true;
		VehicleBodiesChanged = true;
		for (const UUID candidate : candidates)
		{
			const auto found = Vehicles.find(candidate);
			if (found == Vehicles.end() || IsPresent(candidate))
				continue;
			PhysicsVehicleEntry& entry = found->second;
			if (entry.Handle.IsValid())
				World->DestroyVehicle(entry.Handle);
			entry.Handle = VehicleHandle();
			entry.LastInput.reset();
			// A destroyed entity's vehicle goes with its frames; a disabled one keeps its frames for its return.
			if (!scene.FindEntityByID(candidate).IsValid())
				Vehicles.erase(found);
		}
		// A destroyed wheel's frames and authored pose go (an entity created again is a new wheel).
		for (const UUID candidate : candidates)
		{
			if (scene.FindEntityByID(candidate).IsValid())
				continue;
			AuthoredWheelPoses.erase(candidate);
			for (auto& [id, entry] : Vehicles)
				entry.Frames.erase(candidate);
		}
	}

	bool PhysicsSystem::State::HasKeptVehicleFrames() const
	{
		return !AuthoredWheelPoses.empty() || std::any_of(Vehicles.begin(), Vehicles.end(), [](const auto& vehicle)
		{
			return !vehicle.second.Frames.empty();
		});
	}

	void PhysicsSystem::State::AppendVehicleHash(std::vector<std::byte>& bytes) const
	{
		if (!HasKeptVehicleFrames())
			return;
		for (const UUID id : Specification.RuntimeScene->GetCanonicalOrder())
		{
			const auto found = Vehicles.find(id);
			if (found == Vehicles.end() || found->second.Frames.empty())
				continue;
			const PhysicsVehicleEntry& entry = found->second;
			Utils::AppendVehicleUUID(bytes, id);
			for (const auto& [wheel, frame] : entry.Frames)
			{
				Utils::AppendVehicleUUID(bytes, wheel);
				Utils::AppendVehicleVector(bytes, frame.Center);
				Utils::AppendVehicleVector(bytes, frame.Up);
				Utils::AppendVehicleVector(bytes, frame.Forward);
			}
			// While in force: the engine's, the transmission's and the wheels' state (PhysicsWorld::AppendVehicleState).
			if (entry.Handle.IsValid())
				World->AppendVehicleState(entry.Handle, bytes);
		}
		// Then each placed wheel's authored pose, in UUID order of its wheel: what a frame of it formed later comes from.
		for (const auto& [wheel, pose] : AuthoredWheelPoses)
		{
			Utils::AppendVehicleUUID(bytes, wheel);
			Utils::AppendVehicleVector(bytes, pose.Translation);
			Utils::AppendVehicleQuat(bytes, pose.Rotation);
		}
	}

	// --- Reads ----------------------------------------------------------------------------------------------------------

	std::optional<PhysicsVehicleReport> PhysicsSystem::GetVehicleInfo(UUID entity) const
	{
		const State& state = *m_State;
		const Scene& scene = *state.Specification.RuntimeScene;
		const ConstEntity vehicle = FindPhysicsVehicleEntity(scene.FindEntityByID(entity));
		if (!vehicle.IsValid())
			return std::nullopt;

		PhysicsVehicleReport report;
		report.Entity = vehicle.GetUUID();
		const auto found = state.Vehicles.find(report.Entity);
		if (found == state.Vehicles.end())
			return report;
		const PhysicsVehicleEntry& entry = found->second;
		report.Body = entry.Plan.Body;
		report.Created = entry.Handle.IsValid() && state.World->IsVehicleValid(entry.Handle) && state.IsPresent(report.Entity);
		const std::vector<UUID>& wheels = report.Created ? entry.WheelEntities : entry.Plan.Wheels;
		for (const UUID wheel : wheels)
			report.Wheels.push_back(PhysicsVehicleWheelReport{ .Entity = wheel });
		if (!report.Created)
			return report;

		const PhysicsPose pose = state.World->GetPose(entry.Body);
		report.Speed = glm::dot(state.World->GetLinearVelocity(entry.Body), pose.Rotation * glm::vec3(0.0f, 0.0f, -1.0f));
		const PhysicsVehicleState reading = state.World->GetVehicleState(entry.Handle);
		report.EngineRpm = reading.EngineRpm;
		report.Gear = reading.Gear;
		report.Clutch = reading.ClutchFriction;
		report.Shifting = reading.Shifting;
		for (size_t index = 0; index < report.Wheels.size() && index < reading.Wheels.size(); ++index)
		{
			const PhysicsVehicleWheelState& wheel = reading.Wheels[index];
			PhysicsVehicleWheelReport& out = report.Wheels[index];
			out.Touching = wheel.Touching;
			if (wheel.Touching)
			{
				out.Point = wheel.Point;
				out.Normal = wheel.Normal;
				if (const std::optional<PhysicsOverlapHit> hit = state.ResolveHit(wheel.ContactBody, wheel.ContactCollider))
					out.Collider = hit->Entity;
			}
			out.SuspensionLength = wheel.SuspensionLength;
			out.SuspensionForce = wheel.SuspensionForce;
			// Jolt's angle is signed in (-2 pi, 2 pi) (PhysicsVehicleWheelState::Angle); the read is in [0, 360).
			out.Rotation = wheel.Angle * VehicleRadiansToDegrees;
			if (out.Rotation < 0.0f)
				out.Rotation += 360.0f;
			if (out.Rotation >= 360.0f)
				out.Rotation -= 360.0f;
			// Jolt's steer angle is positive to the left; the components' Steering, and the reads, positive to the right.
			out.SteerAngle = -wheel.SteerAngle * VehicleRadiansToDegrees;
			out.AngularVelocity = wheel.AngularVelocity * VehicleRadiansToDegrees;
			out.LongitudinalSlip = wheel.LongitudinalSlip;
			out.LateralSlip = wheel.LateralSlip * VehicleRadiansToDegrees;
		}
		return report;
	}

}
