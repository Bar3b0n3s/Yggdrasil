#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/NavMeshData.h"
#include "Engine/Core/Random.h"
#include "Engine/Navigation/NavigationWorld.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <format>
#include <optional>
#include <string>

// The Navigation script namespace (M30, Docs/Decisions/0045-m30-navigation.md decision 10; World Tree's brief 1): queries of
// the play session's navigation mesh (Navigation/NavigationWorld.h), usable in every callback, the fixed step's included.
// Every argument is checked before any query, so a call raises the same errors whether or not the scene has a mesh; a
// scene without one (no Navigation component, no bake, or a mesh that did not load: NAV_MESH_NOT_BAKED) finds nothing.

namespace Engine {

	namespace Utils {

		// The navigation world and the agent type a query names.
		struct ScriptNavigationTarget
		{
			NavigationWorld* World = nullptr;
			size_t Agent = 0;
		};

		// A query position: finite (Check<vector>) and within MaxNavigationCoordinate of the origin.
		static glm::vec3 CheckNavigationPosition(ScriptCall& call, int index)
		{
			const glm::vec3 position = Lua::Check<glm::vec3>(call, index);
			if (glm::any(glm::greaterThan(glm::abs(position), glm::vec3(MaxNavigationCoordinate))))
				Lua::RaiseError(call, std::format("argument #{} must lie within {} metres of the origin", index, MaxNavigationCoordinate));
			return position;
		}

		// A search distance or radius in (0, MaxNavigationSearchDistance]; `fallback` when omitted.
		static float CheckNavigationDistance(ScriptCall& call, int index, float fallback)
		{
			if (Lua::IsNoneOrNil(call, index))
				return fallback;
			const double value = Lua::Check<double>(call, index);
			if (!(value > 0.0) || value > static_cast<double>(MaxNavigationSearchDistance))
				Lua::RaiseError(call, std::format("argument #{} must be greater than 0 and at most {} metres", index, MaxNavigationSearchDistance));
			return static_cast<float>(value);
		}

		// The agent type argument at `index` (a name, the first agent type when omitted) in the session's navigation world;
		// nullopt when the scene has no navigation mesh. Raises for a name the mesh does not have.
		static std::optional<ScriptNavigationTarget> CheckNavigationAgent(ScriptCall& call, int index)
		{
			std::optional<std::string> name;
			if (!Lua::IsNoneOrNil(call, index))
				name = Lua::CheckBoundedString(call, index, MaxNavAgentNameLength);
			NavigationWorld* world = call.Engine->GetHost().GetNavigation();
			if (world == nullptr)
				return std::nullopt;
			if (!name.has_value())
				return ScriptNavigationTarget{ .World = world, .Agent = 0 };
			const std::optional<size_t> agent = world->FindAgent(*name);
			if (!agent.has_value())
			{
				std::string known;
				for (size_t candidate = 0; candidate < world->GetAgentCount(); ++candidate)
					known += std::format("{}'{}'", candidate == 0 ? "" : ", ", world->GetAgentName(candidate));
				Lua::RaiseError(call, std::format("unknown agent type '{}': the scene's navigation mesh has {}", *name, known));
			}
			return ScriptNavigationTarget{ .World = world, .Agent = *agent };
		}

		static int PushNavigationPoint(ScriptCall& call, const std::optional<glm::vec3>& point)
		{
			if (point.has_value())
				Lua::Push(call, *point);
			else
				Lua::PushNil(call);
			return 1;
		}

		static int ScriptNavigationFindPath(ScriptCall& call)
		{
			const glm::vec3 from = CheckNavigationPosition(call, 1);
			const glm::vec3 to = CheckNavigationPosition(call, 2);
			const std::optional<ScriptNavigationTarget> target = CheckNavigationAgent(call, 3);
			std::optional<NavigationPath> path;
			if (target.has_value())
				path = target->World->FindPath(target->Agent, from, to, MaxScriptNavigationPathCorners);
			if (!path.has_value())
			{
				Lua::PushNil(call);
				Lua::Push(call, false);
				return 2;
			}
			lua_createtable(call.State, static_cast<int>(path->Corners.size()), 0);
			int slot = 1;
			for (const glm::vec3& corner : path->Corners)
			{
				Lua::Push(call, corner);
				lua_rawseti(call.State, -2, slot++);
			}
			Lua::Push(call, path->Complete);
			return 2;
		}

		static int ScriptNavigationClosestPoint(ScriptCall& call)
		{
			const glm::vec3 position = CheckNavigationPosition(call, 1);
			const float maxDistance = CheckNavigationDistance(call, 2, NavigationEndpointSearchDistance);
			const std::optional<ScriptNavigationTarget> target = CheckNavigationAgent(call, 3);
			if (!target.has_value())
				return PushNavigationPoint(call, std::nullopt);
			return PushNavigationPoint(call, target->World->FindClosestPoint(target->Agent, position, maxDistance));
		}

		static int ScriptNavigationRaycast(ScriptCall& call)
		{
			const glm::vec3 from = CheckNavigationPosition(call, 1);
			const glm::vec3 to = CheckNavigationPosition(call, 2);
			const std::optional<ScriptNavigationTarget> target = CheckNavigationAgent(call, 3);
			std::optional<NavigationRaycast> ray;
			if (target.has_value())
				ray = target->World->Raycast(target->Agent, from, to);
			if (!ray.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			lua_createtable(call.State, 0, 4);
			Lua::Push(call, ray->Clear);
			lua_setfield(call.State, -2, "Clear");
			Lua::Push(call, ray->Point);
			lua_setfield(call.State, -2, "Point");
			Lua::Push(call, ray->Normal);
			lua_setfield(call.State, -2, "Normal");
			Lua::Push(call, ray->Fraction);
			lua_setfield(call.State, -2, "Fraction");
			return 1;
		}

		static int ScriptNavigationRandomPoint(ScriptCall& call)
		{
			std::optional<glm::vec3> center;
			float radius = 0.0f;
			if (!Lua::IsNoneOrNil(call, 1))
			{
				center = CheckNavigationPosition(call, 1);
				if (Lua::IsNoneOrNil(call, 2))
					Lua::RaiseError(call, "a center needs a radius (argument #2)");
				radius = CheckNavigationDistance(call, 2, 0.0f);
			}
			else if (!Lua::IsNoneOrNil(call, 2))
			{
				Lua::RaiseError(call, "a radius needs a center (argument #1)");
			}
			const std::optional<ScriptNavigationTarget> target = CheckNavigationAgent(call, 3);
			Random* random = Lua::IsNoneOrNil(call, 4) ? nullptr : &Lua::CheckRandomStorage(call, 4);
			if (!target.has_value())
				return PushNavigationPoint(call, std::nullopt);
			if (random == nullptr)
			{
				// The session's stream, a shared random write (as Random.Number).
				const Status permission = call.PrepareHostMutation();
				if (!permission)
					return Lua::RaiseError(call, permission.error());
				random = &call.Engine->GetHost().GetRandom();
			}
			if (center.has_value())
				return PushNavigationPoint(call, target->World->FindRandomPointAround(target->Agent, *center, radius, *random));
			return PushNavigationPoint(call, target->World->FindRandomPoint(target->Agent, *random));
		}

	}

	namespace ScriptBindings {

		Status RegisterNavigation(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			ENGINE_TRY(api.RegisterAlias("NavigationRaycast", "{Clear: boolean, Point: vector, Normal: vector, Fraction: number}",
				"A walk along the navigation surface: Clear when nothing blocks it, Point where it ends on the surface, the blocking edge's horizontal Normal "
				"(pointing back along the walk; zero when Clear) and the Fraction of the way it got in XZ."));
			api.Module("Navigation", "Queries of the play session's baked navigation mesh (the scene's Navigation component), deterministic and usable in every callback.")
				.Function("FindPath", Utils::ScriptNavigationFindPath, "(from: vector, to: vector, agent: string?) -> ({vector}?, boolean)",
					"The path's corners (at most 256) from from's point on the mesh to to's, and whether it reaches to: an unreachable goal gives the partial path "
					"to the closest reachable point. nil and false when either end is more than 2 m from the mesh or the scene has no mesh. agent names an agent "
					"type of the Navigation component (default: its first).",
					read)
				.Function("ClosestPoint", Utils::ScriptNavigationClosestPoint, "(position: vector, maxDistance: number?, agent: string?) -> vector?",
					"The point of the agent type's surface nearest to position within maxDistance metres (default 2, at most 10,000), or nil.", read)
				.Function("Raycast", Utils::ScriptNavigationRaycast, "(from: vector, to: vector, agent: string?) -> NavigationRaycast?",
					"A straight walk along the surface from from's point on the mesh toward to's XZ: whether it is clear, where it stops and the blocking edge's normal; "
					"nil when from is more than 2 m from the mesh.",
					read)
				.Function("RandomPoint", Utils::ScriptNavigationRandomPoint, "(center: vector?, radius: number?, agent: string?, generator: RandomGenerator?) -> vector?",
					"A random point of the surface, uniform over its area: anywhere, or within radius metres (in XZ) of center on polygons reachable from it. Draws "
					"from generator, or from the session's Random stream when omitted; nil, without a draw, when there is no area to draw from.");
			return {};
		}

	}

}
