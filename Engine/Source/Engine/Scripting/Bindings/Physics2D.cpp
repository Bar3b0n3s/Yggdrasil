#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

// The 2D physics script surface (Docs/Decisions/0032-m21-contract.md decision 13): the RigidBody2D proxy's methods and the
// Physics2D module, over the host's Physics2DSystem. They speak the plane: a vector argument's X and Y are used and its Z
// ignored, a returned vector has Z 0, angles are degrees about +Z (counter-clockwise seen from +Z) and angular velocities
// numbers in radians per second about +Z, as the 3D RigidBody's and Physics module's are vectors.

namespace Engine {

	namespace Utils {

		static void Physics2DChecked(ScriptCall& call, Status result)
		{
			if (!result)
				Lua::RaiseError(call, result.error());
		}

		static Physics2DSystem& ScriptPhysics2D(ScriptCall& call)
		{
			auto* physics = call.Engine->GetHost().GetPhysics2D();
			if (physics == nullptr)
				Lua::RaiseError(call, "2D physics is unavailable");
			return *physics;
		}

		// The receiver of a RigidBody2D method, whose entity must be live (ValidateComponent refuses one pending destruction).
		static Entity Body2DSelf(ScriptCall& call)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			auto& host = call.Engine->GetHost();
			Physics2DChecked(call, ScriptProxy::ValidateComponent(host, proxy));
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != "RigidBody2D")
				Lua::RaiseError(call, "wrong component receiver");
			return host.GetScene().FindEntityByID(proxy.Entity.ID);
		}

		// A vector argument in the plane: its X and Y, each at most `bound` in magnitude; its Z is ignored.
		static glm::vec2 PlanarArgument(ScriptCall& call, int index, float bound)
		{
			const auto value = Lua::Check<glm::vec3>(call, index);
			const glm::vec2 planar(value.x, value.y);
			if (glm::any(glm::greaterThan(glm::abs(planar), glm::vec2(bound))))
				Lua::RaiseError(call, "vector exceeds the physics range");
			return planar;
		}

		static void PushPlanar(ScriptCall& call, const glm::vec2& value)
		{
			Lua::Push(call, glm::vec3(value, 0.0f));
		}

		// The largest force, impulse, torque or velocity component the body functions accept (Physics2DSystem.cpp's bound).
		constexpr float MaxBody2DMagnitude = 1.0e12f;

		// A number argument at most `bound` in magnitude (Lua::Check refuses NaN and infinity).
		static float BoundedScalar(ScriptCall& call, int index, float bound)
		{
			const float value = Lua::Check<float>(call, index);
			if (std::abs(value) > bound)
				Lua::RaiseError(call, "number exceeds the physics range");
			return value;
		}

		// The receiver's 2D body must exist, with the motion type `motion` when one is given, before the host mutation is
		// prepared, as the 3D proxy checks its body (Bindings/Components.cpp): a call refused here changes nothing, and an
		// evaluation or test driver that makes it never marks the session changed, so an active recording stays valid.
		static void CheckBody2D(ScriptCall& call, Physics2DSystem& physics, UUID entity, std::optional<PhysicsMotionType> motion = {})
		{
			const Result<PhysicsMotionType> type = physics.GetMotionType(entity);
			if (!type)
				Lua::RaiseError(call, type.error());
			if (motion && *type != *motion)
				Lua::RaiseError(call, "2D body must be " + std::string(PhysicsMotionTypeToString(*motion)));
		}

		template<auto Method>
		static int Body2DVectorWrite(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			auto& physics = ScriptPhysics2D(call);
			const glm::vec2 value = PlanarArgument(call, 2, MaxBody2DMagnitude);
			CheckBody2D(call, physics, entity.GetUUID(), PhysicsMotionType::Dynamic);
			Physics2DChecked(call, call.PrepareHostMutation());
			Physics2DChecked(call, (physics.*Method)(entity.GetUUID(), value));
			return 0;
		}

		template<auto Method>
		static int Body2DScalarWrite(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			auto& physics = ScriptPhysics2D(call);
			const float value = BoundedScalar(call, 2, MaxBody2DMagnitude);
			CheckBody2D(call, physics, entity.GetUUID(), PhysicsMotionType::Dynamic);
			Physics2DChecked(call, call.PrepareHostMutation());
			Physics2DChecked(call, (physics.*Method)(entity.GetUUID(), value));
			return 0;
		}

		static int Body2DForceAtPosition(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			auto& physics = ScriptPhysics2D(call);
			const glm::vec2 force = PlanarArgument(call, 2, MaxBody2DMagnitude);
			const glm::vec2 position = PlanarArgument(call, 3, MaxPhysicsCoordinate);
			CheckBody2D(call, physics, entity.GetUUID(), PhysicsMotionType::Dynamic);
			Physics2DChecked(call, call.PrepareHostMutation());
			Physics2DChecked(call, physics.AddForceAtPosition(entity.GetUUID(), force, position));
			return 0;
		}

		static int Body2DLinearVelocity(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			const Result<glm::vec2> velocity = ScriptPhysics2D(call).GetLinearVelocity(entity.GetUUID());
			if (!velocity)
				return Lua::RaiseError(call, velocity.error());
			PushPlanar(call, *velocity);
			return 1;
		}

		static int Body2DAngularVelocity(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			const Result<float> velocity = ScriptPhysics2D(call).GetAngularVelocity(entity.GetUUID());
			if (!velocity)
				return Lua::RaiseError(call, velocity.error());
			Lua::Push(call, *velocity);
			return 1;
		}

		static int Body2DIsSleeping(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			const Result<bool> sleeping = ScriptPhysics2D(call).IsSleeping(entity.GetUUID());
			if (!sleeping)
				return Lua::RaiseError(call, sleeping.error());
			Lua::Push(call, *sleeping);
			return 1;
		}

		template<bool Teleport>
		static int Body2DPose(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			auto& physics = ScriptPhysics2D(call);
			const glm::vec2 position = PlanarArgument(call, 2, MaxPhysicsCoordinate);
			const std::optional<float> angle = Teleport && Lua::IsNoneOrNil(call, 3) ? std::nullopt : std::optional<float>(Lua::Check<float>(call, 3));
			CheckBody2D(call, physics, entity.GetUUID(), Teleport ? std::nullopt : std::optional{ PhysicsMotionType::Kinematic });
			Physics2DChecked(call, call.PrepareHostMutation());
			if constexpr (Teleport)
			{
				Physics2DChecked(call, physics.Teleport(entity.GetUUID(), position, angle));
				call.Engine->GetHost().MarkTeleported(entity.GetUUID());
			}
			else
			{
				Physics2DChecked(call, physics.MoveKinematic(entity.GetUUID(), position, *angle));
			}
			return 0;
		}

		static int Body2DWake(ScriptCall& call)
		{
			const Entity entity = Body2DSelf(call);
			auto& physics = ScriptPhysics2D(call);
			CheckBody2D(call, physics, entity.GetUUID());
			Physics2DChecked(call, call.PrepareHostMutation());
			Physics2DChecked(call, physics.WakeUp(entity.GetUUID()));
			return 0;
		}

		static PhysicsLayerMask Query2DMask(ScriptCall& call, int index)
		{
			return Lua::IsNoneOrNil(call, index) ? AllPhysicsLayers : Lua::Check<uint32_t>(call, index);
		}

		static void Query2DEntity(ScriptCall& call, UUID id)
		{
			Lua::Push(call, ScriptEntityIdentity{ id, call.Engine->GetHost().GetSceneGeneration() });
		}

		// Physics.Raycast's RaycastHit, its vectors in the plane.
		static void Query2DHit(ScriptCall& call, const Physics2DRaycastHit& hit)
		{
			lua_createtable(call.State, 0, 5);
			Query2DEntity(call, hit.Entity);
			lua_setfield(call.State, -2, "Entity");
			Query2DEntity(call, hit.Body);
			lua_setfield(call.State, -2, "Body");
			PushPlanar(call, hit.Point);
			lua_setfield(call.State, -2, "Point");
			PushPlanar(call, hit.Normal);
			lua_setfield(call.State, -2, "Normal");
			Lua::Push(call, hit.Distance);
			lua_setfield(call.State, -2, "Distance");
		}

		template<bool All>
		static int Physics2DRaycastBinding(ScriptCall& call)
		{
			auto& physics = ScriptPhysics2D(call);
			const auto origin = Lua::Check<glm::vec3>(call, 1);
			const auto direction = Lua::Check<glm::vec3>(call, 2);
			const float distance = Lua::Check<float>(call, 3);
			const PhysicsLayerMask mask = Query2DMask(call, 4);
			if constexpr (All)
			{
				const auto hits = physics.RaycastAll(glm::vec2(origin), glm::vec2(direction), distance, mask);
				if (!hits)
					return Lua::RaiseError(call, hits.error());
				lua_newtable(call.State);
				int index = 1;
				for (const Physics2DRaycastHit& hit : *hits)
				{
					Query2DHit(call, hit);
					lua_rawseti(call.State, -2, index++);
				}
			}
			else
			{
				const auto hit = physics.Raycast(glm::vec2(origin), glm::vec2(direction), distance, mask);
				if (!hit)
					return Lua::RaiseError(call, hit.error());
				if (*hit)
					Query2DHit(call, **hit);
				else
					Lua::PushNil(call);
			}
			return 1;
		}

		static int Push2DOverlap(ScriptCall& call, const Result<std::vector<PhysicsOverlapHit>>& hits)
		{
			if (!hits)
				return Lua::RaiseError(call, hits.error());
			lua_newtable(call.State);
			int index = 1;
			for (const PhysicsOverlapHit& hit : *hits)
			{
				Query2DEntity(call, hit.Entity);
				lua_rawseti(call.State, -2, index++);
			}
			return 1;
		}

		static int Physics2DOverlapCircleBinding(ScriptCall& call)
		{
			auto& physics = ScriptPhysics2D(call);
			const auto center = Lua::Check<glm::vec3>(call, 1);
			const float radius = Lua::Check<float>(call, 2);
			return Push2DOverlap(call, physics.OverlapCircle(glm::vec2(center), radius, Query2DMask(call, 3)));
		}

		static int Physics2DOverlapBoxBinding(ScriptCall& call)
		{
			auto& physics = ScriptPhysics2D(call);
			const auto center = Lua::Check<glm::vec3>(call, 1);
			const auto halfExtents = Lua::Check<glm::vec3>(call, 2);
			const float angle = Lua::IsNoneOrNil(call, 3) ? 0.0f : Lua::Check<float>(call, 3);
			return Push2DOverlap(call, physics.OverlapBox(glm::vec2(center), glm::vec2(halfExtents), angle, Query2DMask(call, 4)));
		}

		static int Physics2DGravityBinding(ScriptCall& call)
		{
			PushPlanar(call, ScriptPhysics2D(call).GetGravity());
			return 1;
		}

		static int Physics2DSetGravityBinding(ScriptCall& call)
		{
			auto& physics = ScriptPhysics2D(call);
			const glm::vec2 gravity = PlanarArgument(call, 1, MaxPhysicsGravity);
			Physics2DChecked(call, call.PrepareHostMutation());
			Physics2DChecked(call, physics.SetGravity(gravity));
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterPhysics2D(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			const ScriptMemberOptions write{};
			api.Type("RigidBody2D")
				.Method("AddForce", Utils::Body2DVectorWrite<&Physics2DSystem::AddForce>, "(self: RigidBody2D, force: vector) -> ()", "Apply a force in the XY plane to a Dynamic 2D body.", write)
				.Method("AddForceAtPosition", Utils::Body2DForceAtPosition, "(self: RigidBody2D, force: vector, position: vector) -> ()", "Apply a force in the XY plane at a world position of the plane to a Dynamic 2D body.", write)
				.Method("AddTorque", Utils::Body2DScalarWrite<&Physics2DSystem::AddTorque>, "(self: RigidBody2D, torque: number) -> ()", "Apply a torque about +Z, in newton metres, to a Dynamic 2D body.", write)
				.Method("AddImpulse", Utils::Body2DVectorWrite<&Physics2DSystem::AddImpulse>, "(self: RigidBody2D, impulse: vector) -> ()", "Apply an impulse in the XY plane to a Dynamic 2D body.", write)
				.Method("AddAngularImpulse", Utils::Body2DScalarWrite<&Physics2DSystem::AddAngularImpulse>, "(self: RigidBody2D, impulse: number) -> ()", "Apply an angular impulse about +Z to a Dynamic 2D body.", write)
				.Method("GetLinearVelocity", Utils::Body2DLinearVelocity, "(self: RigidBody2D) -> vector", "Read the linear velocity in the XY plane (Z is 0).", read)
				.Method("SetLinearVelocity", Utils::Body2DVectorWrite<&Physics2DSystem::SetLinearVelocity>, "(self: RigidBody2D, velocity: vector) -> ()", "Set a Dynamic 2D body's linear velocity from the vector's X and Y.", write)
				.Method("GetAngularVelocity", Utils::Body2DAngularVelocity, "(self: RigidBody2D) -> number", "Read the angular velocity about +Z in radians per second.", read)
				.Method("SetAngularVelocity", Utils::Body2DScalarWrite<&Physics2DSystem::SetAngularVelocity>, "(self: RigidBody2D, velocity: number) -> ()", "Set a Dynamic 2D body's angular velocity about +Z in radians per second.", write)
				.Method("MoveKinematic", Utils::Body2DPose<false>, "(self: RigidBody2D, position: vector, angle: number) -> ()", "Set a Kinematic 2D target in the plane, the angle in degrees about +Z, for the next fixed step.", write)
				.Method("Teleport", Utils::Body2DPose<true>, "(self: RigidBody2D, position: vector, angle: number?) -> ()", "Place a 2D body at once in the plane (the entity keeps its Z), the angle in degrees about +Z kept when omitted, preserving velocity and resetting interpolation.", write)
				.Method("IsSleeping", Utils::Body2DIsSleeping, "(self: RigidBody2D) -> boolean", "Whether the 2D body is sleeping.", read)
				.Method("WakeUp", Utils::Body2DWake, "(self: RigidBody2D) -> ()", "Wake a sleeping 2D body.", write);
			api.Module("Physics2D", "Queries and gravity for the active scene's 2D physics world, in the XY plane.")
				.Function("Raycast", Utils::Physics2DRaycastBinding<false>, "(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?", "Closest 2D hit, including sensors; vectors in the plane (Z ignored, results with Z 0).", read)
				.Function("RaycastAll", Utils::Physics2DRaycastBinding<true>, "(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> {RaycastHit}", "All 2D hits sorted by distance then collider UUID.", read)
				.Function("OverlapCircle", Utils::Physics2DOverlapCircleBinding, "(center: vector, radius: number, layerMask: number?) -> {Entity}", "Unique overlapping 2D collider entities sorted by UUID; radius obeys collider size bounds.", read)
				.Function("OverlapBox", Utils::Physics2DOverlapBoxBinding, "(center: vector, halfExtents: vector, angle: number?, layerMask: number?) -> {Entity}", "Unique overlapping 2D collider entities sorted by UUID; the box rotated by angle degrees about +Z (0 by default).", read)
				.Function("GetGravity", Utils::Physics2DGravityBinding, "() -> vector", "Current 2D world gravity (Z is 0).", read)
				.Function("SetGravity", Utils::Physics2DSetGravityBinding, "(gravity: vector) -> ()", "Set the 2D world's gravity from the vector's X and Y for the session.");
			return {};
		}

	}

}
