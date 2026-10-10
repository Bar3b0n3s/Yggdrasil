#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <cmath>
#include <vector>

namespace Engine {

	namespace Utils {

		static PhysicsSystem& ScriptPhysics(ScriptCall& call)
		{
			auto* physics = call.Engine->GetHost().GetPhysics();
			if (!physics)
				Lua::RaiseError(call, "physics is unavailable");
			return *physics;
		}
		static PhysicsLayerMask QueryMask(ScriptCall& call, int index)
		{
			return Lua::IsNoneOrNil(call, index) ? AllPhysicsLayers : Lua::Check<uint32_t>(call, index);
		}
		static void QueryEntity(ScriptCall& call, UUID id)
		{
			Lua::Push(call, ScriptEntityIdentity{ id, call.Engine->GetHost().GetSceneGeneration() });
		}
		static void QueryHit(ScriptCall& call, const PhysicsRaycastHit& hit)
		{
			lua_createtable(call.State, 0, 5);
			QueryEntity(call, hit.Entity);
			lua_setfield(call.State, -2, "Entity");
			QueryEntity(call, hit.Body);
			lua_setfield(call.State, -2, "Body");
			Lua::Push(call, hit.Point);
			lua_setfield(call.State, -2, "Point");
			Lua::Push(call, hit.Normal);
			lua_setfield(call.State, -2, "Normal");
			Lua::Push(call, hit.Distance);
			lua_setfield(call.State, -2, "Distance");
		}
		template<bool All>
		static int PhysicsRaycast(ScriptCall& call)
		{
			auto& physics = ScriptPhysics(call);
			const auto origin = Lua::Check<glm::vec3>(call, 1);
			const auto direction = Lua::Check<glm::vec3>(call, 2);
			const float distance = Lua::Check<float>(call, 3);
			const auto mask = QueryMask(call, 4);
			if constexpr (All)
			{
				auto hits = physics.RaycastAll(origin, direction, distance, mask);
				if (!hits)
					return Lua::RaiseError(call, hits.error());
				lua_newtable(call.State);
				int i = 1;
				for (const auto& hit : *hits)
				{
					QueryHit(call, hit);
					lua_rawseti(call.State, -2, i++);
				}
			}
			else
			{
				auto hit = physics.Raycast(origin, direction, distance, mask);
				if (!hit)
					return Lua::RaiseError(call, hit.error());
				if (*hit)
					QueryHit(call, **hit);
				else
					Lua::PushNil(call);
			}
			return 1;
		}
		static int PhysicsSphereCast(ScriptCall& call)
		{
			auto& physics = ScriptPhysics(call);
			const auto origin = Lua::Check<glm::vec3>(call, 1);
			const float radius = Lua::Check<float>(call, 2);
			const auto direction = Lua::Check<glm::vec3>(call, 3);
			const float distance = Lua::Check<float>(call, 4);
			const auto mask = QueryMask(call, 5);
			auto hit = physics.SphereCast(origin, radius, direction, distance, mask);
			if (!hit)
				return Lua::RaiseError(call, hit.error());
			if (*hit)
				QueryHit(call, **hit);
			else
				Lua::PushNil(call);
			return 1;
		}
		static int PushOverlap(ScriptCall& call, Result<std::vector<PhysicsOverlapHit>> hits)
		{
			if (!hits)
				return Lua::RaiseError(call, hits.error());
			lua_newtable(call.State);
			int i = 1;
			for (const auto& hit : *hits)
			{
				QueryEntity(call, hit.Entity);
				lua_rawseti(call.State, -2, i++);
			}
			return 1;
		}
		static int PhysicsOverlapSphere(ScriptCall& call)
		{
			auto& physics = ScriptPhysics(call);
			const auto center = Lua::Check<glm::vec3>(call, 1);
			const auto radius = Lua::Check<float>(call, 2);
			const auto mask = QueryMask(call, 3);
			return PushOverlap(call, physics.OverlapSphere(center, radius, mask));
		}
		static int PhysicsOverlapBox(ScriptCall& call)
		{
			auto& physics = ScriptPhysics(call);
			const auto center = Lua::Check<glm::vec3>(call, 1);
			const auto halfExtents = Lua::Check<glm::vec3>(call, 2);
			const glm::quat input = Lua::IsNoneOrNil(call, 3) ? glm::quat(1, 0, 0, 0) : Lua::Check<glm::quat>(call, 3);
			const glm::dquat rotation{ static_cast<double>(input.w), static_cast<double>(input.x), static_cast<double>(input.y), static_cast<double>(input.z) };
			const auto unit = rotation / std::sqrt(glm::dot(rotation, rotation));
			const glm::quat normalized{ static_cast<float>(unit.w), static_cast<float>(unit.x), static_cast<float>(unit.y), static_cast<float>(unit.z) };
			const auto mask = QueryMask(call, 4);
			return PushOverlap(call, physics.OverlapBox(center, halfExtents, normalized, mask));
		}
		template<bool Collider>
		static int PhysicsBounds(ScriptCall& call)
		{
			auto& physics = ScriptPhysics(call);
			const auto entity = Lua::Check<ScriptEntityIdentity>(call, 1);
			const auto valid = ScriptProxy::ValidateEntity(call.Engine->GetHost(), entity);
			if (!valid)
				return Lua::RaiseError(call, valid.error());
			const auto bounds = Collider ? physics.GetColliderBounds(entity.ID) : physics.GetBodyBounds(entity.ID);
			if (bounds)
			{
				Lua::Push(call, bounds->Min);
				Lua::Push(call, bounds->Max);
			}
			else
			{
				Lua::PushNil(call);
				Lua::PushNil(call);
			}
			return 2;
		}
		static int PhysicsGravity(ScriptCall& call)
		{
			Lua::Push(call, ScriptPhysics(call).GetGravity());
			return 1;
		}
		static int PhysicsSetGravity(ScriptCall& call)
		{
			auto& physics = ScriptPhysics(call);
			const auto gravity = Lua::Check<glm::vec3>(call, 1);
			if (glm::any(glm::greaterThan(glm::abs(gravity), glm::vec3(MaxPhysicsGravity))))
				return Lua::RaiseError(call, "gravity exceeds the physics range");
			auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			auto status = physics.SetGravity(gravity);
			if (!status)
				return Lua::RaiseError(call, status.error());
			return 0;
		}
		static int PhysicsMask(ScriptCall& call)
		{
			std::vector<std::string> names;
			const int count = Lua::GetArgumentCount(call);
			names.reserve(static_cast<size_t>(count));
			for (int i = 1; i <= count; ++i)
				names.push_back(Lua::Check<std::string>(call, i));
			std::vector<std::string_view> views;
			views.reserve(names.size());
			for (const auto& name : names)
				views.emplace_back(name);
			auto mask = ScriptPhysics(call).MakeLayerMask(views);
			if (!mask)
				return Lua::RaiseError(call, mask.error());
			Lua::Push(call, *mask);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterPhysics(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			ENGINE_TRY(api.RegisterAlias("RaycastHit", "{Entity: Entity, Body: Entity, Point: vector, Normal: vector, Distance: number}", "A query hit distinguishes the collider entity from its body owner."));
			api.Module("Physics", "Queries and gravity for the active scene's physics system.")
				.Function("Raycast", Utils::PhysicsRaycast<false>, "(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?", "Closest hit, including sensors; normalize direction and reject invalid finite ranges.", read)
				.Function("RaycastAll", Utils::PhysicsRaycast<true>, "(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> {RaycastHit}", "All hits sorted by distance then collider UUID.", read)
				.Function("SphereCast", Utils::PhysicsSphereCast, "(origin: vector, radius: number, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?", "Closest swept sphere hit; radius obeys collider size bounds.", read)
				.Function("OverlapSphere", Utils::PhysicsOverlapSphere, "(center: vector, radius: number, layerMask: number?) -> {Entity}", "Unique overlapping collider entities sorted by UUID.", read)
				.Function("OverlapBox", Utils::PhysicsOverlapBox, "(center: vector, halfExtents: vector, rotation: Quat?, layerMask: number?) -> {Entity}", "Unique overlapping collider entities sorted by UUID; default identity rotation.", read)
				.Function("GetBodyBounds", Utils::PhysicsBounds<false>, "(entity: Entity) -> (vector?, vector?)", "World AABB of the body's complete compound, or nil if no body is owned.", read)
				.Function("GetColliderBounds", Utils::PhysicsBounds<true>, "(entity: Entity) -> (vector?, vector?)", "World AABB of this entity's collider sub-shapes, or nil if absent.", read)
				.Function("GetGravity", Utils::PhysicsGravity, "() -> vector", "Current world gravity.", read)
				.Function("SetGravity", Utils::PhysicsSetGravity, "(gravity: vector) -> ()", "Set finite gravity within physics bounds for the session.")
				.Function("LayerMask", Utils::PhysicsMask, "(...string) -> number", "Combine named project layers; omitted query masks include every layer.", read);
			return {};
		}

	}
}
