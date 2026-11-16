#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// A play session's navigation (M30, Docs/Decisions/0045-m30-navigation.md decisions 10 and 11; World Tree's brief 1): the
// run-time form of a scene's baked NavMesh (Asset/NavMeshData.h) and the queries the Navigation script namespace calls.
//   - One Detour navigation mesh and query per agent type, built at Create from the validated payload with
//     dtCreateNavMeshData (one tile, every polygon's flags 1), never from trusted bytes. An agent type without polygons (a
//     bake over nothing walkable) has no Detour mesh: its queries find nothing. Each agent type's filter costs a NavModifier
//     area 1 + i at AreaCosts[i] times its length, the walkable area at 1.
//   - Queries read only the mesh and their arguments and keep no state between calls (the query objects' node pools are
//     reset by each search), so equal calls on equal meshes give equal results in every configuration: Detour and these
//     functions use exactly rounded float operations only (precise floating point, Recast's and Detour's audit in ADR
//     0045 decision 2), the order of every search is the mesh's own (polygon indices, Detour's BV tree), and a random
//     point is drawn first-party from the caller's Random, never through Detour's frand callback.
//   - A position is "on the mesh" through its closest point: the point of the agent's surface (its detail mesh) nearest to
//     it in 3D within a search distance, ties going to the polygon Detour's BV tree visits first. Every search box is
//     clipped to the mesh's bounds before Detour sees it, so a far position never makes Detour walk an unbounded range of
//     tiles.
//   - Crafted payloads cannot hang a query: A* is bounded by the node pool (MaxNavigationQueryNodes), the corridor and the
//     corners by their caps, and Raycast's walk across polygons by the agent's polygon count.
// Not thread-safe (the query objects are scratch state): the main thread, like the scene it serves.

namespace Engine {

	class Random;
	struct NavMeshData;

	// The positions a query takes: finite and at most this far from the origin on every axis (metres, as
	// MaxPhysicsCoordinate). Callers check script arguments against it; the world asserts it.
	inline constexpr float MaxNavigationCoordinate = 1.0e9f;
	// The longest search distance or radius a query takes (metres): ClosestPoint's maxDistance, RandomPoint's radius.
	inline constexpr float MaxNavigationSearchDistance = 1.0e4f;
	// How far FindPath and Raycast look for the polygon under each end (metres, ClosestPoint's distance), and ClosestPoint's
	// default distance: an entity's origin may sit up to its height above the floor it stands on.
	inline constexpr float NavigationEndpointSearchDistance = 2.0f;
	// A* visits at most this many polygons per FindPath (Detour's node pool per agent type); a search that runs out ends at
	// the polygon closest to the goal so far, as an unreachable goal does.
	inline constexpr uint32_t MaxNavigationQueryNodes = 8192;
	// The polygons of one FindPath corridor; a longer corridor ends where it is cut, as an unreachable goal does.
	inline constexpr uint32_t MaxNavigationPathPolygons = 4096;
	// The draws RandomPoint makes around a center before it settles for the drawn polygon's point nearest the center.
	inline constexpr uint32_t MaxNavigationRandomPointAttempts = 8;

	// FindPath's result: the path's corners, from the start's point on the mesh to its end, and whether it ends at the goal.
	struct NavigationPath
	{
		std::vector<glm::vec3> Corners{}; // at least one
		// True when the corners end at the goal's point on the mesh, a path of exactly the corner cap included; false for the
		// partial path to the reachable point closest to an unreachable goal, and for a path cut at a cap (the corners, the
		// corridor or the node pool).
		bool Complete = false;
	};

	// Raycast's result: how far a straight walk along the surface gets toward the target.
	struct NavigationRaycast
	{
		bool Clear = false;       // nothing blocks the walk: it reaches the target's XZ
		glm::vec3 Point{ 0.0f };  // where the walk ends, on the surface: the target's point when Clear, else the blocking edge
		glm::vec3 Normal{ 0.0f }; // the blocking edge's horizontal unit normal, facing the walk; zero when Clear
		float Fraction = 0.0f;    // of the way from the start's point to the target, in XZ: 1 when Clear
	};

	class NavigationWorld
	{
	public:
		// One agent type's Detour mesh and query; opaque (NavigationWorld.cpp).
		struct Agent;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class NavigationWorld;
		};

		explicit NavigationWorld(ConstructionKey key);
		~NavigationWorld();

		NavigationWorld(const NavigationWorld&) = delete;
		NavigationWorld& operator=(const NavigationWorld&) = delete;
		NavigationWorld(NavigationWorld&&) = delete;
		NavigationWorld& operator=(NavigationWorld&&) = delete;

		// Builds Detour's mesh and query for every agent type of `mesh`. Requires the Navigation step of the process context
		// (Detour's allocator, NavigationRuntime.h; asserted). Errors: Validation from ValidateNavMeshData (the payload is
		// checked again, so no caller can hand Detour unvalidated data) or when Detour refuses an agent's data (naming it);
		// OutOfMemory when Detour cannot allocate. Nothing is kept on failure.
		[[nodiscard]] static Result<Scope<NavigationWorld>> Create(const NavMeshData& mesh);

		// The agent types, in the mesh's order (the Navigation component's).
		[[nodiscard]] size_t GetAgentCount() const;
		[[nodiscard]] std::string_view GetAgentName(size_t agent) const; // agent below GetAgentCount(), asserted
		// The index of the agent type named `name` (exact, case-sensitive), or nullopt.
		[[nodiscard]] std::optional<size_t> FindAgent(std::string_view name) const;

		// The path from `from` to `to` for agent type `agent` (below GetAgentCount()), each a position within
		// MaxNavigationCoordinate (asserted): nullopt when either end has no point on the mesh within
		// NavigationEndpointSearchDistance; otherwise the corners from the start's point to the goal's point (Complete), or
		// to the point of the corridor's last polygon closest to the goal when the goal is unreachable or a cap cuts the
		// path (not Complete). At most `maxCorners` corners (at least 1, asserted).
		[[nodiscard]] std::optional<NavigationPath> FindPath(size_t agent, const glm::vec3& from, const glm::vec3& to, size_t maxCorners);

		// The point of agent type `agent`'s surface closest to `position` (3D distance), or nullopt when none lies within
		// `maxDistance` (in (0, MaxNavigationSearchDistance], asserted).
		[[nodiscard]] std::optional<glm::vec3> FindClosestPoint(size_t agent, const glm::vec3& position, float maxDistance);

		// A straight walk along agent type `agent`'s surface from `from`'s point on the mesh (within
		// NavigationEndpointSearchDistance) toward `to`'s XZ, polygon to polygon, as Detour's raycast walks: nullopt when
		// `from` has no point on the mesh; otherwise where it ends and what blocks it (NavigationRaycast).
		[[nodiscard]] std::optional<NavigationRaycast> Raycast(size_t agent, const glm::vec3& from, const glm::vec3& to);

		// A random point of agent type `agent`'s surface, uniform over its polygons' area: one draw of random.NextDouble()
		// picks the polygon (by cumulative area in polygon order, so polygons of zero area are never picked) and two of
		// random.NextFloat() the point in it (Detour's dtRandomPointInConvexPoly), lifted onto the detail surface. nullopt
		// (no draw) when the agent has no polygon of positive area.
		[[nodiscard]] std::optional<glm::vec3> FindRandomPoint(size_t agent, Random& random);

		// A random point within `radius` (in (0, MaxNavigationSearchDistance], asserted) of `center` in XZ, on polygons
		// reachable from `center`'s: the polygon of the closest point within `radius` of `center` (nullopt, no draw, when
		// there is none), then every polygon reached across an edge that comes within `radius` of `center` in XZ. Each
		// attempt draws as FindRandomPoint does over those polygons; the first point within `radius` in XZ is the result,
		// and after MaxNavigationRandomPointAttempts the last drawn polygon's point closest to `center`. nullopt (no draw)
		// when the reached polygons have no area.
		[[nodiscard]] std::optional<glm::vec3> FindRandomPointAround(size_t agent, const glm::vec3& center, float radius, Random& random);
	private:
		std::vector<Scope<Agent>> m_Agents{};
	};

}
