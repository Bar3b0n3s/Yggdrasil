#include "EnginePCH.h"
#include "Engine/Scene/PhysicsVehicles.h"

#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/DisabledTag.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// Why a vehicle cannot be made: its Subject, the WheeledVehicle field at fault (empty for a wheel's or the hierarchy's),
		// the message's reason and the hint.
		struct VehicleRefusal
		{
			std::string Subject{};
			std::string Field{};
			std::string Reason{};
			std::string Hint{};
		};

		// A wheel the walk up from a VehicleWheel found, and whether something stands between it and its vehicle.
		struct FoundWheel
		{
			UUID Entity{};
			// The entity on the way with a RigidBody or CharacterController (the wheel's own included), or the vehicle's own
			// entity for a wheel on it; invalid when none.
			UUID Blocker{};
		};

	}

	namespace Utils {

		// How far the engine torque shares of a vehicle's driven differentials may sum from 1 (Jolt's own check, the world's).
		constexpr float VehicleTorqueRatioTolerance = 1.0e-6f;

		// Whether `settings`' differentials drive a wheel, with shares that sum, in float in list order over the driven ones (those
		// that name a wheel), to 1 within VehicleTorqueRatioTolerance: what Jolt asserts at every step.
		static std::string DescribeVehicleEntity(ConstEntity entity)
		{
			return std::format("'{}' ({})", entity.GetName(), entity.GetUUID());
		}

		// Whether neither `entity` nor an ancestor holds DisabledTag (the body composition's rule).
		static bool IsVehicleEntityEnabled(ConstEntity entity)
		{
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (current.HasComponent<DisabledTag>())
					return false;
			}
			return true;
		}

		// A limited-slip ratio is 0 (open) or above 1 (the registry bounds its top).
		static bool IsValidLimitedSlip(float ratio)
		{
			return ratio == 0.0f || ratio > 1.0f;
		}

		// The first reason `keys` is not a curve the world accepts: more than MaxPhysicsVehicleCurveKeys keys, X not strictly
		// increasing, or (`unitX`, the torque curve) an X outside [0, 1]; nullopt when it is one.
		static std::optional<std::string> CheckVehicleCurve(std::span<const VehicleCurveKey> keys, bool unitX)
		{
			if (keys.size() > MaxPhysicsVehicleCurveKeys)
				return std::format("it has {} keys, more than {}", keys.size(), MaxPhysicsVehicleCurveKeys);
			for (size_t index = 0; index < keys.size(); ++index)
			{
				if (unitX && !(keys[index].X >= 0.0f && keys[index].X <= 1.0f))
					return std::format("key {}'s X {} is outside [0, 1]", index, keys[index].X);
				if (index > 0 && !(keys[index].X > keys[index - 1].X))
					return std::format("key {}'s X {} does not increase from key {}'s {}", index, keys[index].X, index - 1, keys[index - 1].X);
			}
			return std::nullopt;
		}

		// Whether `id` names an existing VehicleWheel entity of the vehicle `vehicle` (its nearest WheeledVehicle) that is
		// effectively disabled: a wheel reference to it is no wheel. Another vehicle's disabled wheel is not one.
		static bool IsDisabledWheel(const Scene& scene, ConstEntity vehicle, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			return entity.IsValid() && entity.HasComponent<VehicleWheelComponent>() && !IsVehicleEntityEnabled(entity) && FindPhysicsVehicleEntity(entity) == vehicle;
		}

		// The names of the entities `ids`, quoted and joined: 'A', 'A' and 'B', or 'A', 'B' and 'C'.
		static std::string JoinVehicleEntityNames(const Scene& scene, std::span<const UUID> ids)
		{
			std::string joined;
			for (size_t index = 0; index < ids.size(); ++index)
			{
				if (index > 0)
					joined += index + 1 == ids.size() ? " and " : ", ";
				joined += std::format("'{}'", scene.FindEntityByID(ids[index]).GetName());
			}
			return joined;
		}

		// The first reason a wheel reference of a differential (null is no wheel) or a bar is refused: it names an entity that
		// is not one of `wheels` and not a disabled VehicleWheel of the vehicle `vehicle`; nullopt when it is valid.
		static std::optional<std::string> CheckWheelReference(const Scene& scene, ConstEntity vehicle, std::span<const UUID> wheels, UUID wheel, std::string_view side)
		{
			if (!wheel.IsValid() || std::find(wheels.begin(), wheels.end(), wheel) != wheels.end() || IsDisabledWheel(scene, vehicle, wheel))
				return std::nullopt;
			const ConstEntity entity = scene.FindEntityByID(wheel);
			if (!entity.IsValid())
				return std::format("its {} {} names no entity of the scene", side, wheel);
			return std::format("its {} {} is not a wheel of the vehicle", side, DescribeVehicleEntity(entity));
		}

		// The first reason the fields of `vehicle`, the WheeledVehicle of `owner`, or its wheels' are refused (the subjects
		// "suspension" to "curve"; the hierarchy's subjects come first, in ComposePhysicsVehicles); nullopt when the world accepts
		// them. `wheels` are its enabled wheels (the plan's).
		static std::optional<VehicleRefusal> CheckVehicleFields(const Scene& scene, ConstEntity owner, const WheeledVehicleComponent& vehicle, std::span<const UUID> wheels)
		{
			for (const UUID id : wheels)
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				const VehicleWheelComponent& wheel = entity.GetComponent<VehicleWheelComponent>();
				if (wheel.SuspensionMinLength > wheel.SuspensionMaxLength)
				{
					return VehicleRefusal{ .Subject = "suspension",
						.Reason = std::format("the SuspensionMinLength {} of its wheel {} is above its SuspensionMaxLength {}", wheel.SuspensionMinLength,
							DescribeVehicleEntity(entity), wheel.SuspensionMaxLength),
						.Hint = "set the wheel's SuspensionMinLength at or below its SuspensionMaxLength" };
				}
				if (!(wheel.SpringFrequency > 0.0f))
				{
					return VehicleRefusal{ .Subject = "suspension",
						.Reason = std::format("the SpringFrequency of its wheel {} is 0, so its suspension has no spring", DescribeVehicleEntity(entity)),
						.Hint = "give the wheel a SpringFrequency above 0 (1.5 Hz is a car's)" };
				}
			}

			if (vehicle.MinRpm > vehicle.MaxRpm)
			{
				return VehicleRefusal{ .Subject = "engine",
					.Field = "MinRpm",
					.Reason = std::format("its MinRpm {} is above its MaxRpm {}", vehicle.MinRpm, vehicle.MaxRpm),
					.Hint = "set MinRpm at or below MaxRpm" };
			}

			const auto transmission = [](std::string field, std::string reason, std::string hint)
			{
				return VehicleRefusal{ .Subject = "transmission", .Field = std::move(field), .Reason = std::move(reason), .Hint = std::move(hint) };
			};
			if (vehicle.GearRatios.empty() || vehicle.GearRatios.size() > MaxPhysicsVehicleGears)
			{
				return transmission("GearRatios", std::format("it has {} forward gears; a vehicle has 1 to {}", vehicle.GearRatios.size(), MaxPhysicsVehicleGears),
					"give GearRatios one to eight ratios, first gear first");
			}
			if (vehicle.ReverseGearRatios.size() > MaxPhysicsVehicleGears)
			{
				return transmission("ReverseGearRatios", std::format("it has {} reverse gears; at most {}", vehicle.ReverseGearRatios.size(), MaxPhysicsVehicleGears),
					"give ReverseGearRatios at most eight ratios");
			}
			for (size_t gear = 0; gear < vehicle.GearRatios.size(); ++gear)
			{
				if (!(vehicle.GearRatios[gear] > 0.0f && vehicle.GearRatios[gear] <= MaxPhysicsVehicleRatio))
				{
					return transmission("GearRatios", std::format("forward gear {}'s ratio {} is not above 0 and at most {}", gear + 1, vehicle.GearRatios[gear], MaxPhysicsVehicleRatio),
						"give each forward gear a ratio above 0");
				}
			}
			for (size_t gear = 0; gear < vehicle.ReverseGearRatios.size(); ++gear)
			{
				if (!(vehicle.ReverseGearRatios[gear] < 0.0f && vehicle.ReverseGearRatios[gear] >= -MaxPhysicsVehicleRatio))
				{
					return transmission("ReverseGearRatios",
						std::format("reverse gear {}'s ratio {} is not below 0 and at least {}", gear + 1, vehicle.ReverseGearRatios[gear], -MaxPhysicsVehicleRatio),
						"give each reverse gear a ratio below 0");
				}
			}
			if (!(vehicle.ShiftDownRpm > 0.0f))
				return transmission("ShiftDownRpm", "its ShiftDownRpm is 0", "set ShiftDownRpm above 0 and below ShiftUpRpm");
			if (!(vehicle.ShiftUpRpm > vehicle.ShiftDownRpm))
			{
				return transmission("ShiftUpRpm", std::format("its ShiftUpRpm {} is not above its ShiftDownRpm {}", vehicle.ShiftUpRpm, vehicle.ShiftDownRpm),
					"set ShiftUpRpm above ShiftDownRpm");
			}
			if (vehicle.Transmission == VehicleTransmissionMode::Auto && !(vehicle.ShiftUpRpm < vehicle.MaxRpm))
			{
				return transmission("ShiftUpRpm", std::format("its Auto transmission's ShiftUpRpm {} is not below its MaxRpm {}", vehicle.ShiftUpRpm, vehicle.MaxRpm),
					"set ShiftUpRpm below MaxRpm, or the engine never reaches it");
			}
			if (!(vehicle.ClutchStrength > 0.0f))
				return transmission("ClutchStrength", "its ClutchStrength is 0, so its clutch couples nothing", "set ClutchStrength above 0 (10 is Jolt's)");

			const auto differential = [](std::string reason, std::string hint)
			{
				return VehicleRefusal{ .Subject = "differential", .Field = "Differentials", .Reason = std::move(reason), .Hint = std::move(hint) };
			};
			if (vehicle.Differentials.size() > MaxPhysicsVehicleDifferentials)
			{
				return differential(std::format("it has {} differentials; at most {}", vehicle.Differentials.size(), MaxPhysicsVehicleDifferentials),
					"drive the wheels through at most eight differentials");
			}
			// As Jolt sums them: in float, in list order, over the differentials that name a wheel; and again over those that name
			// an enabled wheel, since a disabled wheel of the vehicle is no wheel (MakePhysicsVehicleSettings).
			float torqueRatios = 0.0f;
			bool driven = false;
			float enabledTorqueRatios = 0.0f;
			bool enabledDriven = false;
			std::vector<UUID> disabled;
			const auto isEnabledWheel = [wheels](UUID wheel)
			{
				return std::find(wheels.begin(), wheels.end(), wheel) != wheels.end();
			};
			for (size_t index = 0; index < vehicle.Differentials.size(); ++index)
			{
				const VehicleDifferential& entry = vehicle.Differentials[index];
				for (const auto& [wheel, side] : { std::pair{ entry.LeftWheel, "LeftWheel" }, std::pair{ entry.RightWheel, "RightWheel" } })
				{
					if (const std::optional<std::string> reason = CheckWheelReference(scene, owner, wheels, wheel, side))
						return differential(std::format("differential {}: {}", index, *reason), "name VehicleWheel entities of this vehicle in the differential, or null");
				}
				if (entry.LeftWheel.IsValid() && entry.LeftWheel == entry.RightWheel)
					return differential(std::format("differential {} names one wheel twice", index), "name two different wheels, or one and null");
				if (!(entry.Ratio > 0.0f))
					return differential(std::format("differential {}'s Ratio is 0", index), "set the differential's Ratio above 0 (3.42 is a car's)");
				if (!IsValidLimitedSlip(entry.LimitedSlipRatio))
				{
					return differential(std::format("differential {}'s LimitedSlipRatio {} is in (0, 1]", index, entry.LimitedSlipRatio),
						"set LimitedSlipRatio to 0 for an open differential, or above 1");
				}
				if (entry.LeftWheel.IsValid() || entry.RightWheel.IsValid())
				{
					torqueRatios += entry.EngineTorqueRatio;
					driven = true;
				}
				if (isEnabledWheel(entry.LeftWheel) || isEnabledWheel(entry.RightWheel))
				{
					enabledTorqueRatios += entry.EngineTorqueRatio;
					enabledDriven = true;
				}
				for (const UUID wheel : { entry.LeftWheel, entry.RightWheel })
				{
					if (wheel.IsValid() && !isEnabledWheel(wheel) && std::find(disabled.begin(), disabled.end(), wheel) == disabled.end())
						disabled.push_back(wheel);
				}
			}
			if (!driven)
			{
				return differential("it drives no wheel: no differential names one",
					"add a differential naming the driven wheels; a vehicle that drives none (a trailer) has one, in Manual at Gear 0");
			}
			if (!(std::abs(torqueRatios - 1.0f) < VehicleTorqueRatioTolerance))
			{
				return differential(std::format("the EngineTorqueRatio of its differentials that name a wheel sum to {}, not 1", torqueRatios),
					"give the differentials that name a wheel EngineTorqueRatio shares that sum to 1");
			}
			// What its disabled wheels leave (the W44.2 review's finding 3: never a silent vehicle): no driven wheel, or shares
			// that do not sum to 1.
			constexpr std::string_view DisabledHint = "enable the wheels, or give the differentials of its enabled wheels shares that sum to 1";
			if (!enabledDriven)
			{
				return differential(std::format("its driven {} {} {} disabled, so no differential drives a wheel", disabled.size() == 1 ? "wheel" : "wheels",
										JoinVehicleEntityNames(scene, disabled), disabled.size() == 1 ? "is" : "are"),
					std::string(DisabledHint));
			}
			if (!(std::abs(enabledTorqueRatios - 1.0f) < VehicleTorqueRatioTolerance))
			{
				return differential(std::format("with {} disabled, its driven differentials' EngineTorqueRatio sum to {}", JoinVehicleEntityNames(scene, disabled),
										enabledTorqueRatios),
					std::string(DisabledHint));
			}
			if (!IsValidLimitedSlip(vehicle.DifferentialLimitedSlipRatio))
			{
				return VehicleRefusal{ .Subject = "differential",
					.Field = "DifferentialLimitedSlipRatio",
					.Reason = std::format("its DifferentialLimitedSlipRatio {} is in (0, 1]", vehicle.DifferentialLimitedSlipRatio),
					.Hint = "set DifferentialLimitedSlipRatio to 0 (open) or above 1" };
			}

			const auto bar = [](std::string reason, std::string hint)
			{
				return VehicleRefusal{ .Subject = "bar", .Field = "AntiRollBars", .Reason = std::move(reason), .Hint = std::move(hint) };
			};
			if (vehicle.AntiRollBars.size() > MaxPhysicsVehicleAntiRollBars)
				return bar(std::format("it has {} anti-roll bars; at most {}", vehicle.AntiRollBars.size(), MaxPhysicsVehicleAntiRollBars), "keep at most eight anti-roll bars");
			for (size_t index = 0; index < vehicle.AntiRollBars.size(); ++index)
			{
				const VehicleAntiRollBar& entry = vehicle.AntiRollBars[index];
				for (const auto& [wheel, side] : { std::pair{ entry.LeftWheel, "LeftWheel" }, std::pair{ entry.RightWheel, "RightWheel" } })
				{
					if (!wheel.IsValid())
						return bar(std::format("anti-roll bar {}'s {} is null", index, side), "name two VehicleWheel entities of this vehicle in the bar");
					if (const std::optional<std::string> reason = CheckWheelReference(scene, owner, wheels, wheel, side))
						return bar(std::format("anti-roll bar {}: {}", index, *reason), "name two VehicleWheel entities of this vehicle in the bar");
				}
				if (entry.LeftWheel == entry.RightWheel)
					return bar(std::format("anti-roll bar {} names one wheel twice", index), "name two different wheels, a left and a right one");
			}

			if (const std::optional<std::string> reason = CheckVehicleCurve(vehicle.TorqueCurve, true))
			{
				return VehicleRefusal{ .Subject = "curve",
					.Field = "TorqueCurve",
					.Reason = std::format("its TorqueCurve is invalid: {}", *reason),
					.Hint = "give the curve at most eight keys whose X increases from 0 to 1" };
			}
			for (const UUID id : wheels)
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				const VehicleWheelComponent& wheel = entity.GetComponent<VehicleWheelComponent>();
				for (const auto& [keys, name] : { std::pair{ std::span<const VehicleCurveKey>(wheel.LongitudinalFriction), "LongitudinalFriction" },
						 std::pair{ std::span<const VehicleCurveKey>(wheel.LateralFriction), "LateralFriction" } })
				{
					if (const std::optional<std::string> reason = CheckVehicleCurve(keys, false))
					{
						return VehicleRefusal{ .Subject = "curve",
							.Reason = std::format("the {} of its wheel {} is invalid: {}", name, DescribeVehicleEntity(entity), *reason),
							.Hint = "give the wheel's curve at most eight keys whose X increases from key to key" };
					}
				}
			}
			return std::nullopt;
		}

		static PhysicsDiagnostic MakeVehicleDiagnostic(ConstEntity entity, const VehicleRefusal& refusal)
		{
			PhysicsDiagnostic diagnostic;
			diagnostic.Code = std::string(PhysicsVehicleInvalidCode);
			diagnostic.Severity = DiagnosticSeverity::Error;
			diagnostic.Entity = entity.GetUUID();
			diagnostic.Component = "WheeledVehicle";
			diagnostic.Field = refusal.Field;
			diagnostic.Subject = refusal.Subject;
			diagnostic.Message = std::format("the WheeledVehicle of {} cannot be made: {}; the vehicle is not created", DescribeVehicleEntity(entity), refusal.Reason);
			diagnostic.Hint = refusal.Hint;
			return diagnostic;
		}

	}

	PhysicsVehicleComposition ComposePhysicsVehicles(const Scene& scene, const PhysicsComposition& bodies)
	{
		PhysicsVehicleComposition composition;
		std::map<UUID, const PhysicsBodyPlan*> rigidBodies;
		for (const PhysicsBodyPlan& plan : bodies.Bodies)
		{
			if (plan.Origin == PhysicsBodyOrigin::RigidBody)
				rigidBodies.emplace(plan.Owner, &plan);
		}

		// Every effectively enabled VehicleWheel under its vehicle, in canonical order: the walk up stops at the first
		// WheeledVehicle (the wheel's own entity included), noting the first RigidBody or CharacterController on the way.
		std::map<UUID, std::vector<FoundWheel>> wheelsByVehicle;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.HasComponent<VehicleWheelComponent>() || !Utils::IsVehicleEntityEnabled(entity))
				continue;
			FoundWheel wheel{ .Entity = id };
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (current.HasComponent<WheeledVehicleComponent>())
				{
					if (current == entity)
						wheel.Blocker = id;
					wheelsByVehicle[current.GetUUID()].push_back(wheel);
					break;
				}
				if (!wheel.Blocker.IsValid() && (current.HasComponent<RigidBodyComponent>() || current.HasComponent<CharacterControllerComponent>()))
					wheel.Blocker = current.GetUUID();
			}
		}

		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			const WheeledVehicleComponent* vehicle = entity.TryGetComponent<WheeledVehicleComponent>();
			if (vehicle == nullptr || !Utils::IsVehicleEntityEnabled(entity))
				continue;

			PhysicsVehiclePlan plan;
			plan.Entity = id;
			const std::vector<FoundWheel>& found = wheelsByVehicle[id];
			for (const FoundWheel& wheel : found)
				plan.Wheels.push_back(wheel.Entity);
			const auto refuse = [&composition, &plan, entity](const VehicleRefusal& refusal)
			{
				composition.Diagnostics.push_back(Utils::MakeVehicleDiagnostic(entity, refusal));
				plan.IsCreatable = false;
			};

			// "body": its own RigidBody's plan; one a body diagnostic refused (it has colliders) has its diagnostic already.
			const auto body = rigidBodies.find(id);
			if (body == rigidBodies.end() || (!body->second->IsCreatable && body->second->Colliders.empty()))
			{
				refuse({ .Subject = "body",
					.Reason = "its RigidBody makes no body: it has no solid collider",
					.Hint = "give the vehicle's entity, or a child of it that is not a wheel, a solid collider (the chassis)" });
				composition.Vehicles.push_back(std::move(plan));
				continue;
			}
			const PhysicsBodyPlan& bodyPlan = *body->second;
			if (!bodyPlan.IsCreatable)
			{
				plan.IsCreatable = false;
				composition.Vehicles.push_back(std::move(plan));
				continue;
			}
			if (bodyPlan.MotionType != PhysicsMotionType::Dynamic || bodyPlan.IsSensor)
			{
				std::string_view kind = "Kinematic";
				if (bodyPlan.IsSensor)
					kind = "a trigger";
				else if (bodyPlan.MotionType == PhysicsMotionType::Static)
					kind = "Static";
				refuse({ .Subject = "body",
					.Reason = std::format("its RigidBody makes no Dynamic body: it is {}", kind),
					.Hint = "set the RigidBody's Type to Dynamic, with a collider that is not a trigger" });
				composition.Vehicles.push_back(std::move(plan));
				continue;
			}
			plan.Body = id;

			if (found.empty() || found.size() > MaxPhysicsVehicleWheels)
			{
				refuse({ .Subject = "wheels",
					.Reason = found.empty() ? std::string("it has no wheel: no VehicleWheel entity below it")
											: std::format("it has {} wheels, more than {}", found.size(), MaxPhysicsVehicleWheels),
					.Hint = "add a child entity with a VehicleWheel for each wheel, at most sixteen" });
				composition.Vehicles.push_back(std::move(plan));
				continue;
			}

			// "wheel": a wheel on the vehicle's own entity, or with a RigidBody or CharacterController at or above it below the
			// vehicle.
			const auto blocked = std::find_if(found.begin(), found.end(), [](const FoundWheel& wheel)
			{
				return wheel.Blocker.IsValid();
			});
			if (blocked != found.end())
			{
				const ConstEntity wheel = scene.FindEntityByID(blocked->Entity);
				const ConstEntity blocker = scene.FindEntityByID(blocked->Blocker);
				const std::string reason = blocked->Entity == id
					? std::string("a VehicleWheel is on the vehicle's own entity")
					: std::format("its wheel {} is under the {} of {}, which is no part of the vehicle's body", Utils::DescribeVehicleEntity(wheel),
						  blocker.HasComponent<RigidBodyComponent>() ? "RigidBody" : "CharacterController", Utils::DescribeVehicleEntity(blocker));
				refuse({ .Subject = "wheel", .Reason = reason, .Hint = "put each VehicleWheel on its own child entity of the vehicle, with no RigidBody or CharacterController" });
				composition.Vehicles.push_back(std::move(plan));
				continue;
			}

			// "collider": a solid collider of the body on or below a wheel (the body would gather it, so the wheel would be part
			// of the chassis).
			std::optional<std::pair<UUID, UUID>> wheelCollider;
			for (const PhysicsColliderPlan& collider : bodyPlan.Colliders)
			{
				if (collider.IsTrigger || wheelCollider.has_value())
					continue;
				for (ConstEntity current = scene.FindEntityByID(collider.Entity); current.IsValid() && current.GetUUID() != id; current = current.GetParent())
				{
					if (std::find(plan.Wheels.begin(), plan.Wheels.end(), current.GetUUID()) != plan.Wheels.end())
					{
						wheelCollider = std::pair{ collider.Entity, current.GetUUID() };
						break;
					}
				}
			}
			if (wheelCollider.has_value())
			{
				refuse({ .Subject = "collider",
					.Reason = std::format("{} holds a solid collider on or below its wheel {}, which the body would gather into the chassis",
						Utils::DescribeVehicleEntity(scene.FindEntityByID(wheelCollider->first)), Utils::DescribeVehicleEntity(scene.FindEntityByID(wheelCollider->second))),
					.Hint = "remove the collider from the wheel (the wheel finds the ground by its cast), or make it a trigger" });
				composition.Vehicles.push_back(std::move(plan));
				continue;
			}

			if (const std::optional<VehicleRefusal> refusal = Utils::CheckVehicleFields(scene, entity, *vehicle, plan.Wheels))
				refuse(*refusal);
			composition.Vehicles.push_back(std::move(plan));
		}

		std::sort(composition.Diagnostics.begin(), composition.Diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
		{
			return std::tie(a.Entity, a.Code, a.Subject) < std::tie(b.Entity, b.Code, b.Subject);
		});
		return composition;
	}

	ConstEntity FindPhysicsVehicleEntity(ConstEntity entity)
	{
		if (!entity.IsReadable())
			return {};
		if (entity.HasComponent<WheeledVehicleComponent>())
			return entity;
		if (!entity.HasComponent<VehicleWheelComponent>())
			return {};
		for (ConstEntity current = entity.GetParent(); current.IsReadable(); current = current.GetParent())
		{
			if (current.HasComponent<WheeledVehicleComponent>())
				return current;
		}
		return {};
	}

	glm::mat4 GetPhysicsVehicleChainMatrix(ConstEntity vehicle, ConstEntity entity)
	{
		std::vector<ConstEntity> chain;
		for (ConstEntity current = entity; current.IsReadable() && current != vehicle; current = current.GetParent())
			chain.push_back(current);
		glm::mat4 matrix = glm::scale(glm::mat4(1.0f), TransformSystem::GetWorldScale(vehicle));
		for (auto link = chain.rbegin(); link != chain.rend(); ++link)
			matrix = matrix * TransformSystem::ComputeLocalMatrix(link->GetComponent<TransformComponent>());
		return matrix;
	}

	std::optional<PhysicsVehicleWheelFrame> MakePhysicsVehicleWheelFrame(ConstEntity vehicle, ConstEntity wheel)
	{
		const TransformComponent& transform = wheel.GetComponent<TransformComponent>();
		return MakePhysicsVehicleWheelFrame(vehicle, wheel, transform.Translation, transform.Rotation);
	}

	std::optional<PhysicsVehicleWheelFrame> MakePhysicsVehicleWheelFrame(ConstEntity vehicle, ConstEntity wheel, const glm::vec3& translation, const glm::quat& rotation)
	{
		TransformComponent local = wheel.GetComponent<TransformComponent>();
		local.Translation = translation;
		local.Rotation = rotation;
		// The vehicle's own entity as its wheel (which the composition refuses) is its world scale alone, as the chain matrix's.
		const glm::mat4 chain = wheel == vehicle ? GetPhysicsVehicleChainMatrix(vehicle, vehicle)
												 : GetPhysicsVehicleChainMatrix(vehicle, wheel.GetParent()) * TransformSystem::ComputeLocalMatrix(local);
		const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(chain);
		if (!decomposed.has_value())
			return std::nullopt;
		PhysicsVehicleWheelFrame frame;
		frame.Center = decomposed->Translation;
		frame.Up = glm::normalize(decomposed->Rotation * glm::vec3(0.0f, 1.0f, 0.0f));
		frame.Forward = glm::normalize(decomposed->Rotation * glm::vec3(0.0f, 0.0f, -1.0f));
		const auto finite = [](const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		};
		if (!finite(frame.Center) || !finite(frame.Up) || !finite(frame.Forward))
			return std::nullopt;
		return frame;
	}

	PhysicsVehicleWheelDescription MakePhysicsVehicleWheel(const VehicleWheelComponent& component, const PhysicsVehicleWheelFrame& frame)
	{
		const auto toKeys = [](const std::vector<VehicleCurveKey>& keys)
		{
			std::vector<PhysicsVehicleCurveKey> converted;
			converted.reserve(keys.size());
			for (const VehicleCurveKey& key : keys)
				converted.push_back(PhysicsVehicleCurveKey{ .X = key.X, .Y = key.Y });
			return converted;
		};
		PhysicsVehicleWheelDescription wheel;
		// The suspension runs along the body's -Y from the attachment point, so the wheel hangs at its centre at full droop.
		wheel.Position = frame.Center + glm::vec3(0.0f, component.SuspensionMaxLength, 0.0f);
		wheel.Up = frame.Up;
		wheel.Forward = frame.Forward;
		wheel.Radius = component.Radius;
		wheel.Width = component.Width;
		wheel.SuspensionMinLength = component.SuspensionMinLength;
		wheel.SuspensionMaxLength = component.SuspensionMaxLength;
		wheel.SuspensionPreload = component.SuspensionPreload;
		wheel.SpringFrequency = component.SpringFrequency;
		wheel.SpringDamping = component.SpringDamping;
		// 90 degrees in float radians may round above the world's pi / 2.
		wheel.MaxSteerAngle = std::min(component.MaxSteerAngle * VehicleDegreesToRadians, 0.5f * std::numbers::pi_v<float>);
		wheel.MaxBrakeTorque = component.MaxBrakeTorque;
		wheel.MaxHandBrakeTorque = component.MaxHandBrakeTorque;
		wheel.Inertia = component.Inertia;
		wheel.AngularDamping = component.AngularDamping;
		wheel.LongitudinalFriction = toKeys(component.LongitudinalFriction);
		wheel.LateralFriction = toKeys(component.LateralFriction);
		return wheel;
	}

	PhysicsVehicleSettings MakePhysicsVehicleSettings(const WheeledVehicleComponent& component, std::span<const UUID> wheels)
	{
		const auto indexOf = [wheels](UUID wheel) -> int32_t
		{
			const auto found = std::find(wheels.begin(), wheels.end(), wheel);
			return wheel.IsValid() && found != wheels.end() ? static_cast<int32_t>(found - wheels.begin()) : -1;
		};
		PhysicsVehicleSettings settings;
		settings.Engine.MaxTorque = component.MaxTorque;
		settings.Engine.MinRpm = component.MinRpm;
		settings.Engine.MaxRpm = component.MaxRpm;
		settings.Engine.TorqueCurve.clear();
		for (const VehicleCurveKey& key : component.TorqueCurve)
			settings.Engine.TorqueCurve.push_back(PhysicsVehicleCurveKey{ .X = key.X, .Y = key.Y });
		settings.Engine.Inertia = component.EngineInertia;
		settings.Engine.AngularDamping = component.EngineDamping;
		settings.Transmission.Mode = component.Transmission == VehicleTransmissionMode::Manual ? PhysicsVehicleTransmissionMode::Manual
																							   : PhysicsVehicleTransmissionMode::Auto;
		settings.Transmission.GearRatios = component.GearRatios;
		settings.Transmission.ReverseGearRatios = component.ReverseGearRatios;
		settings.Transmission.SwitchTime = component.SwitchTime;
		settings.Transmission.ClutchReleaseTime = component.ClutchReleaseTime;
		settings.Transmission.SwitchLatency = component.SwitchLatency;
		settings.Transmission.ShiftUpRpm = component.ShiftUpRpm;
		settings.Transmission.ShiftDownRpm = component.ShiftDownRpm;
		settings.Transmission.ClutchStrength = component.ClutchStrength;
		for (const VehicleDifferential& differential : component.Differentials)
		{
			settings.Differentials.push_back(PhysicsVehicleDifferential{ .LeftWheel = indexOf(differential.LeftWheel),
				.RightWheel = indexOf(differential.RightWheel),
				.Ratio = differential.Ratio,
				.LeftRightSplit = differential.LeftRightSplit,
				.LimitedSlipRatio = differential.LimitedSlipRatio,
				.EngineTorqueRatio = differential.EngineTorqueRatio });
		}
		settings.DifferentialLimitedSlipRatio = component.DifferentialLimitedSlipRatio;
		for (const VehicleAntiRollBar& bar : component.AntiRollBars)
		{
			const int32_t left = indexOf(bar.LeftWheel);
			const int32_t right = indexOf(bar.RightWheel);
			if (left >= 0 && right >= 0)
				settings.AntiRollBars.push_back(PhysicsVehicleAntiRollBar{ .LeftWheel = left, .RightWheel = right, .Stiffness = bar.Stiffness });
		}
		// 180 degrees in float radians may round above the world's pi.
		settings.MaxPitchRollAngle = std::min(component.MaxPitchRollAngle * VehicleDegreesToRadians, std::numbers::pi_v<float>);
		return settings;
	}

	PhysicsVehicleInput MakePhysicsVehicleInput(const WheeledVehicleComponent& component)
	{
		return PhysicsVehicleInput{ .Forward = component.Throttle,
			.Right = component.Steering,
			.Brake = component.Brake,
			.HandBrake = component.HandBrake,
			.Gear = component.Gear,
			.Clutch = component.Clutch };
	}

	PhysicsVehicleCast MakePhysicsVehicleCast(const WheeledVehicleComponent& component)
	{
		switch (component.WheelCast)
		{
			case VehicleWheelCast::Ray:      return PhysicsVehicleCast::Ray;
			case VehicleWheelCast::Sphere:   return PhysicsVehicleCast::Sphere;
			case VehicleWheelCast::Cylinder: return PhysicsVehicleCast::Cylinder;
		}
		return PhysicsVehicleCast::Cylinder;
	}

	WheeledVehicleComponent StripPhysicsVehicleInput(const WheeledVehicleComponent& component)
	{
		WheeledVehicleComponent stripped = component;
		const WheeledVehicleComponent defaults;
		stripped.Throttle = defaults.Throttle;
		stripped.Steering = defaults.Steering;
		stripped.Brake = defaults.Brake;
		stripped.HandBrake = defaults.HandBrake;
		stripped.Gear = defaults.Gear;
		stripped.Clutch = defaults.Clutch;
		return stripped;
	}

}
